/**
 * @file storage.c
 * @brief TF 卡存储模块 - SPI 模式
 *
 * 功能：
 * - SPI 模式挂载 FAT32 TF 卡
 * - 文件读写测试
 * - 剩余空间查询
 * - 目录生命周期管理
 *
 * GPIO 配置：
 *   CS   = GPIO10
 *   MOSI = GPIO11
 *   SCK  = GPIO12
 *   MISO = GPIO13
 *
 * ===== 文件系统策略（强制） =====
 *
 * 本项目统一使用 ESP-IDF VFS（Virtual File System）API：
 *   mkdir(), opendir(), readdir(), stat(), unlink(), fopen()
 *
 * 禁止直接使用 FatFs-native API（f_mkdir, f_opendir, f_stat 等）。
 * esp_vfs_fat_sdspi_mount() 已将 FatFs 封装进 VFS，drive prefix "0:/"、
 * "1:/" 属于底层实现细节，业务层不应依赖。
 *
 * 原因：
 * - drive mapping 不稳定（取决于 SPI Flash FAT 注册顺序）
 * - FR_INVALID_NAME 问题
 * - 与 VFS mount point 状态不一致
 *
 * 见 docs/storage-path-policy.md。
 */

#include "storage.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/sdmmc_types.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_bit_defs.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include <errno.h>

static const char *TAG = "storage";

#define MOUNT_POINT "/sdcard"
#define MAX_FILE_PATH 128

/* SPI GPIO 配置 */
#define PIN_NUM_MISO  13
#define PIN_NUM_MOSI  11
#define PIN_NUM_CLK   12
#define PIN_NUM_CS     10

/* SPI 总线主机号 */
#define SPI_BUS_HOST  SPI2_HOST

static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;

/*======================================================================
 * Directory Name Registry
 *
 * 唯一的目录名字符串定义点。
 * 禁止在其他模块中硬编码目录名。
 *======================================================================*/

/* 索引 = storage_path_type_t 枚举值 */
static const char* s_dir_names[] = {
    [STORAGE_PATH_RECORDINGS]   = "recordings",
    [STORAGE_PATH_UPLOADED]     = "uploaded",
    [STORAGE_PATH_UPLOAD_QUEUE] = "upload_queue",
    [STORAGE_PATH_TEMP]         = "temp",
    [STORAGE_PATH_LOGS]         = "logs",
};

/* 内部宏：验证 type 有效性 */
#define IS_VALID_PATH_TYPE(t)  ((t) >= 0 && (t) < STORAGE_PATH_COUNT)

/*======================================================================
 * Public Path Construction API (VFS/POSIX only)
 *======================================================================*/

const char* storage_get_vfs_mount(void)
{
    return MOUNT_POINT;
}

/**
 * 目录类型 → 目录名字符串（用于日志）
 */
const char* storage_path_type_to_string(storage_path_type_t type)
{
    if (!IS_VALID_PATH_TYPE(type)) {
        return "???";
    }
    return s_dir_names[type];
}

/**
 * 构造 VFS 路径: "/sdcard/recordings" 或 "/sdcard/recordings/test.wav"
 *
 * 这是业务层获取文件路径的唯一合法方式。
 * 返回的路径直接用于 fopen()/mkdir()/stat()/opendir()。
 */
esp_err_t storage_build_vfs_path(char *buf, size_t size,
                                 storage_path_type_t type,
                                 const char *filename)
{
    /* 参数校验必须先于 memset */
    if (buf == NULL || size < 8) {
        ESP_LOGE(TAG, "BUILD: invalid args buf=%p size=%u", buf, (unsigned)size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!IS_VALID_PATH_TYPE(type)) {
        ESP_LOGE(TAG, "BUILD: invalid type=%d", type);
        return ESP_ERR_INVALID_ARG;
    }

    const char *dirname = s_dir_names[type];
    if (dirname == NULL) {
        ESP_LOGE(TAG, "BUILD: NULL dirname for type=%d", type);
        return ESP_ERR_INVALID_ARG;
    }

    /* 清除 buffer，消除残留内存 */
    memset(buf, 0, size);

    /* 构造路径：简单 snprintf */
    int snp_ret;
    if (filename != NULL && filename[0] != '\0') {
        snp_ret = snprintf(buf, size, "%s/%s/%s", MOUNT_POINT, dirname, filename);
    } else {
        snp_ret = snprintf(buf, size, "%s/%s", MOUNT_POINT, dirname);
    }

    /* 强制 null terminate（防止 snprintf 被截断） */
    buf[size - 1] = '\0';

    /* 验证 */
    size_t actual = strlen(buf);

    /* HEX DUMP：打印整个 buffer（不只看 strlen） */
    printf("[PATH BUILD] buf=%p size=%u actual=%u snp_ret=%d\n",
           (void*)buf, (unsigned)size, (unsigned)actual, snp_ret);
    printf("  HEX:");
    for (size_t i = 0; i < size; i++) {
        printf(" %02X", (uint8_t)buf[i]);
        if ((i + 1) % 32 == 0) printf("\n       ");
    }
    printf("\n");

    ESP_LOGI(TAG,
        "PATH CHECK: '%s' strlen=%u",
        buf,
        (unsigned)actual);

    /* 检查截断 */
    if (actual >= size - 1) {
        ESP_LOGE(TAG, "PATH TRUNCATED");
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/*======================================================================
 * Internal Directory Helpers (POSIX)
 *======================================================================*/

/**
 * 使用 POSIX stat + mkdir 确保目录存在
 * stat() 成功 = 目录已存在
 * mkdir() 成功 = 目录创建成功
 */
static esp_err_t ensure_dir_vfs(storage_path_type_t type)
{
    char path[64];
    esp_err_t err = storage_build_vfs_path(path, sizeof(path), type, NULL);
    if (err != ESP_OK) {
        return err;
    }

    /* 检查目录是否已存在 */
    struct stat st;
    int stat_ret = stat(path, &st);
    ESP_LOGI(TAG, "  stat('%s') -> ret=%d, errno=%d", path, stat_ret, stat_ret != 0 ? errno : 0);

    if (stat_ret == 0) {
        ESP_LOGI(TAG, "  [OK] %s/ — already exists (mode=0x%X)", s_dir_names[type], (unsigned int)st.st_mode);
        return ESP_OK;
    }

    /* 目录不存在，创建它 */
    ESP_LOGI(TAG, "  mkdir('%s', 0755)...", path);
    int mkdir_ret = mkdir(path, 0755);
    ESP_LOGI(TAG, "  mkdir() -> ret=%d, errno=%d (%s)", mkdir_ret, errno, strerror(errno));

    if (mkdir_ret != 0) {
        /* 诊断：尝试不带 mode 的方式 */
        ESP_LOGI(TAG, "  Retry with S_IFDIR...");
        mkdir_ret = mkdir(path, 0755);  // 再次尝试
        ESP_LOGI(TAG, "  mkdir() retry -> ret=%d, errno=%d (%s)", mkdir_ret, errno, strerror(errno));

        ESP_LOGE(TAG,
            "mkdir('%s') failed errno=%d (%s)",
            path,
            errno,
            strerror(errno));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "  [OK] %s/ — created", s_dir_names[type]);
    return ESP_OK;
}

/*======================================================================
 * storage_ensure_directories - Create all required subdirectories
 *
 * Called automatically by storage_mount() after successful mount.
 * errno == EEXIST is not an error — directory already exists.
 *======================================================================*/
esp_err_t storage_ensure_directories(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    for (storage_path_type_t t = 0; t < STORAGE_PATH_COUNT; t++) {
        esp_err_t err = ensure_dir_vfs(t);
        if (err != ESP_OK) {
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "All directories ensured (%d dirs)", (int)STORAGE_PATH_COUNT);
    return ESP_OK;
}

/*======================================================================
 * storage_validate_layout - Verify all required directories exist
 *
 * 职责：verify ONLY，不创建目录。
 * 使用 stat() 验证，打印 [OK] / [MISSING] 格式日志。
 *======================================================================*/
void storage_validate_layout(void)
{
    if (!s_mounted) {
        return;
    }

    ESP_LOGI(TAG, "Storage Layout:");

    for (storage_path_type_t t = 0; t < STORAGE_PATH_COUNT; t++) {
        char path[64];
        if (storage_build_vfs_path(path, sizeof(path), t, NULL) != ESP_OK) {
            continue;
        }

        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            ESP_LOGI(TAG, "  [OK] %s/", s_dir_names[t]);
        } else {
            ESP_LOGE(TAG, "  [MISSING] %s/", s_dir_names[t]);
        }
    }

    /*==========================================================
     * LFN Diagnostic: Create a brand-new long-named directory
     * at runtime to definitively prove mkdir works.
     *==========================================================*/
    {
        char test_path[64];
        snprintf(test_path, sizeof(test_path),
                 MOUNT_POINT "/lfntst_may2026");
        ESP_LOGI(TAG, "[LFN TEST] mkdir('%s', 0755)...", test_path);
        int ret = mkdir(test_path, 0755);
        if (ret == 0) {
            ESP_LOGI(TAG, "[LFN TEST] PASS - directory created");
            rmdir(test_path);
            ESP_LOGI(TAG, "[LFN TEST] cleanup done");
        } else {
            ESP_LOGE(TAG, "[LFN TEST] FAIL - errno=%d (%s)", errno, strerror(errno));
        }
    }
}

/*======================================================================
 * storage_print_card_info - 打印 SD 卡详细信息
 *======================================================================*/
static void storage_print_card_info(sdmmc_card_t *card)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "SD Card Info:");
    ESP_LOGI(TAG, "  Name: %s", card->cid.name);

    uint32_t ocr = card->ocr;
    if (ocr & BIT(30)) {
        ESP_LOGI(TAG, "  Type: SDHC/SDXC (High Capacity)");
    } else {
        ESP_LOGI(TAG, "  Type: SDSC (Standard Capacity)");
    }

    uint64_t capacity_bytes;
    if (ocr & BIT(30)) {
        capacity_bytes = (uint64_t)card->csd.capacity * 512;
    } else {
        capacity_bytes = (uint64_t)card->csd.capacity * ((uint64_t)1 << card->csd.read_block_len);
    }
    uint64_t capacity_mb = capacity_bytes / (1024 * 1024);

    if (capacity_mb >= 1000) {
        ESP_LOGI(TAG, "  Size: %.1f GB", (double)capacity_mb / 1024.0);
    } else {
        ESP_LOGI(TAG, "  Size: %llu MB", capacity_mb);
    }

    /* 使用 f_getfree 通过 VFS 路径查询剩余空间 */
    FATFS *fs;
    DWORD fre_clust;
    char vfs_root[32];
    storage_build_vfs_path(vfs_root, sizeof(vfs_root), STORAGE_PATH_RECORDINGS, NULL);

    if (f_getfree(vfs_root, &fre_clust, &fs) == FR_OK) {
        DWORD total_sectors = (fs->n_fatent - 2) * fs->csize;
        DWORD free_sectors = fre_clust * fs->csize;
        uint64_t total_kb_u = (uint64_t)total_sectors * fs->ssize / 1024;
        uint64_t free_kb_u = (uint64_t)free_sectors * fs->ssize / 1024;

        ESP_LOGI(TAG, "  Total: %.1f GB", (double)total_kb_u / 1024.0 / 1024.0);
        ESP_LOGI(TAG, "  Free:  %.1f GB", (double)free_kb_u / 1024.0 / 1024.0);
        ESP_LOGI(TAG, "  FAT Type: FAT%u", fs->fs_type == FS_FAT12 ? 12 :
                                      fs->fs_type == FS_FAT16 ? 16 : 32);
    }

    ESP_LOGI(TAG, "  Mount: %s", MOUNT_POINT);
    ESP_LOGI(TAG, "========================================");
}

/*======================================================================
 * storage_mount - SPI 模式挂载 TF 卡
 *======================================================================*/
esp_err_t storage_mount(const char *mount_point)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "Storage already mounted");
        return ESP_OK;
    }

    const char *mount = mount_point ? mount_point : MOUNT_POINT;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Initializing SD Card in SPI mode...");
    ESP_LOGI(TAG, "GPIO Config:");
    ESP_LOGI(TAG, "  CS:   GPIO%d", PIN_NUM_CS);
    ESP_LOGI(TAG, "  MOSI: GPIO%d", PIN_NUM_MOSI);
    ESP_LOGI(TAG, "  SCK:  GPIO%d", PIN_NUM_CLK);
    ESP_LOGI(TAG, "  MISO: GPIO%d", PIN_NUM_MISO);
    ESP_LOGI(TAG, "========================================");

    /* Step 1: 初始化 SPI 总线 */
    ESP_LOGI(TAG, "Step 1: Initializing SPI bus (SPI2)...");
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    esp_err_t ret = spi_bus_initialize(SPI_BUS_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "  SPI bus initialized successfully");

    /* Step 2: 配置 SDSPI 主机 */
    ESP_LOGI(TAG, "Step 2: Configuring SDSPI host...");
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    /* Step 3: 配置 SD 设备 */
    ESP_LOGI(TAG, "Step 3: Configuring SD device GPIO...");
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = SPI_BUS_HOST;

    /* Step 4: FATFS 挂载配置 */
    ESP_LOGI(TAG, "Step 4: Configuring FATFS mount...");
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    /* Step 5: 挂载 FATFS（通过 VFS） */
    ESP_LOGI(TAG, "Step 5: Mounting FAT filesystem at %s...", mount);

    ret = esp_vfs_fat_sdspi_mount(mount, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "  -> Partition not found");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "  -> SPI bus not initialized or driver error");
        } else if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "  -> FATFS mount failed (check card format)");
            ESP_LOGE(TAG, "  -> Try formatting card as FAT32");
        }
        spi_bus_free(SPI_BUS_HOST);
        s_card = NULL;
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted successfully!");

    /* Step 6: 创建所有子目录（stat + mkdir） */
    ret = storage_ensure_directories();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Directory creation failed");
    }

    /* 打印卡信息 */
    storage_print_card_info(s_card);

    /* Step 7: 验证目录布局（POSIX opendir） */
    storage_validate_layout();

    return ESP_OK;
}

/*======================================================================
 * storage_unmount - 卸载 TF 卡
 *======================================================================*/
void storage_unmount(void)
{
    if (!s_mounted) {
        return;
    }

    ESP_LOGI(TAG, "Unmounting SD card...");
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;

    spi_bus_free(SPI_BUS_HOST);
    ESP_LOGI(TAG, "SPI bus released");
    ESP_LOGI(TAG, "SD card unmounted");
}

/*======================================================================
 * storage_test_rw - 测试 SD 卡读写
 *======================================================================*/
esp_err_t storage_test_rw(void)
{
    if (!s_mounted) {
        ESP_LOGE(TAG, "storage_test_rw: card not mounted");
        return ESP_FAIL;
    }

    char vfs_path[MAX_FILE_PATH];
    storage_build_vfs_path(vfs_path, sizeof(vfs_path), STORAGE_PATH_TEMP, "test.txt");

    const char *write_content = "hello sdcard";
    char read_buf[128] = {0};

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "SD Card Read/Write Test");
    ESP_LOGI(TAG, "========================================");

    ESP_LOGI(TAG, "Step 1: Write test...");
    ESP_LOGI(TAG, "  File: %s", vfs_path);

    FILE *f = fopen(vfs_path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "  FAIL: fopen() for write failed");
        return ESP_FAIL;
    }

    size_t written = fwrite(write_content, 1, strlen(write_content), f);
    if (written != strlen(write_content)) {
        ESP_LOGE(TAG, "  FAIL: fwrite() wrote %zu bytes, expected %zu", written, strlen(write_content));
        fclose(f);
        return ESP_FAIL;
    }

    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "  FAIL: fclose() failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "  OK: Write %zu bytes", written);

    ESP_LOGI(TAG, "Step 2: Read test...");
    f = fopen(vfs_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "  FAIL: fopen() for read failed");
        return ESP_FAIL;
    }

    size_t read_bytes = fread(read_buf, 1, sizeof(read_buf) - 1, f);
    read_buf[read_bytes] = '\0';
    fclose(f);

    ESP_LOGI(TAG, "  Read %zu bytes: \"%s\"", read_bytes, read_buf);

    ESP_LOGI(TAG, "Step 3: Verify...");
    if (strcmp(read_buf, write_content) == 0) {
        ESP_LOGI(TAG, "  PASS: Data matches!");
    } else {
        ESP_LOGE(TAG, "  FAIL: Data mismatch!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "SD Card Test: ALL PASSED");
    ESP_LOGI(TAG, "========================================");

    return ESP_OK;
}

/*======================================================================
 * storage_get_space - 获取 TF 卡空间信息
 *======================================================================*/
esp_err_t storage_get_space(uint32_t *out_total_kb, uint32_t *out_free_kb)
{
    if (!s_mounted) {
        return ESP_FAIL;
    }

    FATFS *fs;
    DWORD fre_clust;
    char vfs_root[64];
    storage_build_vfs_path(vfs_root, sizeof(vfs_root), STORAGE_PATH_RECORDINGS, NULL);

    FRESULT res = f_getfree(vfs_root, &fre_clust, &fs);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "f_getfree failed: %d", res);
        return ESP_FAIL;
    }

    DWORD total_sectors = (fs->n_fatent - 2) * fs->csize;
    DWORD free_sectors = fre_clust * fs->csize;
    DWORD sector_size = fs->ssize ? fs->ssize : 512;

    if (out_total_kb) {
        *out_total_kb = (uint32_t)((uint64_t)total_sectors * sector_size / 1024);
    }
    if (out_free_kb) {
        *out_free_kb = (uint32_t)((uint64_t)free_sectors * sector_size / 1024);
    }

    return ESP_OK;
}

/*======================================================================
 * storage_list_wav_files - 列出目录下所有 WAV 文件
 *
 * 使用 POSIX opendir()/readdir() 遍历。
 * @param dir_path 目录名（如 "recordings"），或完整 VFS 路径
 *======================================================================*/
int storage_list_wav_files(const char *dir_path, char file_list[][64], int max_files)
{
    if (!s_mounted || file_list == NULL || max_files <= 0) {
        return 0;
    }

    /* 构造 VFS 目录路径 */
    char vfs_dir[64];
    if (dir_path != NULL && dir_path[0] != '\0') {
        /* 去除前置斜杠（如果有） */
        const char *p = dir_path;
        while (*p == '/') p++;
        snprintf(vfs_dir, sizeof(vfs_dir), "%s/%s", MOUNT_POINT, p);
    } else {
        snprintf(vfs_dir, sizeof(vfs_dir), "%s", MOUNT_POINT);
    }

    DIR *dir = opendir(vfs_dir);
    if (dir == NULL) {
        ESP_LOGW(TAG, "Failed to open directory: %s (errno=%d)", vfs_dir, errno);
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_files) {
        /* 跳过目录 */
        if (entry->d_type == DT_DIR) {
            continue;
        }
        /* 检查 .wav 扩展名（忽略大小写） */
        const char *ext = strrchr(entry->d_name, '.');
        if (ext != NULL && (strcasecmp(ext, ".wav") == 0)) {
            strncpy(file_list[count], entry->d_name, 63);
            file_list[count][63] = '\0';
            count++;
        }
    }

    closedir(dir);
    return count;
}

/*======================================================================
 * storage_delete_file - 删除文件（POSIX unlink）
 *
 * @param file_path 相对路径，如 "recordings/test.wav" 或 "test.wav"
 *======================================================================*/
esp_err_t storage_delete_file(const char *file_path)
{
    if (!s_mounted || file_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 构造 VFS 路径 */
    char vfs_path[MAX_FILE_PATH];
    esp_err_t err = storage_build_vfs_path(vfs_path, sizeof(vfs_path),
                                           STORAGE_PATH_RECORDINGS, file_path);
    if (err != ESP_OK) {
        return err;
    }

    if (unlink(vfs_path) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s (errno=%d)", vfs_path, errno);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/*======================================================================
 * storage_delete_file_vfs - 删除任意相对路径文件
 *======================================================================*/
esp_err_t storage_delete_file_vfs(const char *relative_path)
{
    if (!s_mounted || relative_path == NULL || relative_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char vfs_path[MAX_FILE_PATH];
    snprintf(vfs_path, sizeof(vfs_path), "%s/%s", MOUNT_POINT, relative_path);

    if (unlink(vfs_path) != 0) {
        ESP_LOGE(TAG, "[DELETE] failed: %s (errno=%d)", vfs_path, errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[DELETE] %s", vfs_path);
    return ESP_OK;
}

/*======================================================================
 * storage_rename_file - 跨目录重命名/移动文件
 *======================================================================*/
esp_err_t storage_rename_file(storage_path_type_t src_type, const char *src_filename,
                               storage_path_type_t dst_type, const char *dst_filename)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!IS_VALID_PATH_TYPE(src_type) || !IS_VALID_PATH_TYPE(dst_type)) {
        ESP_LOGE(TAG, "rename: invalid path type (src=%d dst=%d)", src_type, dst_type);
        return ESP_ERR_INVALID_ARG;
    }
    if (src_filename == NULL || dst_filename == NULL ||
        src_filename[0] == '\0' || dst_filename[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char src_path[MAX_FILE_PATH];
    char dst_path[MAX_FILE_PATH];

    esp_err_t err = storage_build_vfs_path(src_path, sizeof(src_path), src_type, src_filename);
    if (err != ESP_OK) return err;
    err = storage_build_vfs_path(dst_path, sizeof(dst_path), dst_type, dst_filename);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "[RENAME] %s -> %s", src_path, dst_path);

    int ret = rename(src_path, dst_path);
    if (ret != 0) {
        ESP_LOGE(TAG, "[RENAME] failed errno=%d (%s)", errno, strerror(errno));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[RENAME] success");
    return ESP_OK;
}

/*======================================================================
 * storage_file_exists - 检查文件是否存在（POSIX stat）
 *======================================================================*/
bool storage_file_exists(const char *file_path)
{
    if (!s_mounted || file_path == NULL) {
        return false;
    }

    char vfs_path[MAX_FILE_PATH];
    if (storage_build_vfs_path(vfs_path, sizeof(vfs_path),
                               STORAGE_PATH_RECORDINGS, file_path) != ESP_OK) {
        return false;
    }

    struct stat st;
    return (stat(vfs_path, &st) == 0 && S_ISREG(st.st_mode));
}

/*======================================================================
 * storage_get_file_size - 获取文件大小（POSIX stat）
 *======================================================================*/
uint32_t storage_get_file_size(const char *file_path)
{
    if (!s_mounted || file_path == NULL) {
        return 0;
    }

    char vfs_path[MAX_FILE_PATH];
    if (storage_build_vfs_path(vfs_path, sizeof(vfs_path),
                               STORAGE_PATH_RECORDINGS, file_path) != ESP_OK) {
        return 0;
    }

    struct stat st;
    if (stat(vfs_path, &st) == 0 && S_ISREG(st.st_mode)) {
        return (uint32_t)st.st_size;
    }

    return 0;
}
