/**
 * @file storage.c
 * @brief TF 卡存储模块 - SPI 模式
 *
 * 功能：
 * - SPI 模式挂载 FAT32 TF 卡
 * - 文件读写测试
 * - 剩余空间查询
 *
 * GPIO 配置：
 *   CS   = GPIO10
 *   MOSI = GPIO11
 *   SCK  = GPIO12
 *   MISO = GPIO13
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
#include <string.h>
#include <sys/stat.h>
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
 * storage_print_card_info - 打印 SD 卡详细信息
 *======================================================================*/
static void storage_print_card_info(sdmmc_card_t *card)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "SD Card Info:");
    ESP_LOGI(TAG, "  Name: %s", card->cid.name);

    /* 判断卡类型 */
    uint32_t ocr = card->ocr;
    if (ocr & BIT(30)) {
        ESP_LOGI(TAG, "  Type: SDHC/SDXC (High Capacity)");
    } else {
        ESP_LOGI(TAG, "  Type: SDSC (Standard Capacity)");
    }

    /* 计算容量 */
    /* ESP-IDF v5.2: card->csd.capacity 对于 SDHC/SDXC
     * 单位是 512 字节扇区；read_block_len = 9 (2^9=512) */
    uint64_t capacity_bytes;
    if (ocr & BIT(30)) {
        /* SDHC/SDXC: capacity 本身就是 512 字节扇区数 */
        capacity_bytes = (uint64_t)card->csd.capacity * 512;
    } else {
        /* SDSC: 用 shift 计算字节数 */
        capacity_bytes = (uint64_t)card->csd.capacity * ((uint64_t)1 << card->csd.read_block_len);
    }
    uint64_t capacity_mb = capacity_bytes / (1024 * 1024);

    if (capacity_mb >= 1000) {
        ESP_LOGI(TAG, "  Size: %.1f GB", (double)capacity_mb / 1024.0);
    } else {
        ESP_LOGI(TAG, "  Size: %llu MB", capacity_mb);
    }

    /* 获取剩余空间 */
    FATFS *fs;
    DWORD fre_clust;
    if (f_getfree(MOUNT_POINT, &fre_clust, &fs) == FR_OK) {
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
 *
 * 步骤：
 * 1. 初始化 SPI 总线
 * 2. 配置 SDSPI 设备
 * 3. 挂载 FATFS
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
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;  /* 20MHz */

    /* Step 3: 配置 SD 设备（GPIO 引脚） */
    ESP_LOGI(TAG, "Step 3: Configuring SD device GPIO...");
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = SPI_BUS_HOST;

    /* Step 4: FATFS 挂载配置 */
    ESP_LOGI(TAG, "Step 4: Configuring FATFS mount...");
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,     /* 不自动格式化 */
        .max_files = 5,                      /* 最多同时打开 5 个文件 */
        .allocation_unit_size = 16 * 1024
    };

    /* Step 5: 挂载 FATFS */
    ESP_LOGI(TAG, "Step 5: Mounting FAT filesystem at %s...", mount);

    ret = esp_vfs_fat_sdspi_mount(mount, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));

        /* 详细错误诊断 */
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "  -> Partition not found");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "  -> SPI bus not initialized or driver error");
        } else if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "  -> FATFS mount failed (check card format)");
            ESP_LOGE(TAG, "  -> Try formatting card as FAT32");
        }

        /* 清理：释放 SPI 总线 */
        spi_bus_free(SPI_BUS_HOST);
        s_card = NULL;
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted successfully!");

    /* 打印卡信息 */
    storage_print_card_info(s_card);

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

    /* 卸载 FATFS */
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;

    /* 释放 SPI 总线 */
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

    const char *test_file = MOUNT_POINT "/test.txt";
    const char *write_content = "hello sdcard";
    char read_buf[128] = {0};

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "SD Card Read/Write Test");
    ESP_LOGI(TAG, "========================================");

    /* Step 1: 写入测试 */
    ESP_LOGI(TAG, "Step 1: Write test...");
    ESP_LOGI(TAG, "  File: %s", test_file);
    ESP_LOGI(TAG, "  Content: \"%s\"", write_content);

    FILE *f = fopen(test_file, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "  FAIL: fopen() for write failed");
        ESP_LOGE(TAG, "  errno: %d", errno);
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

    /* Step 2: 重新打开并读取 */
    ESP_LOGI(TAG, "Step 2: Read test...");

    f = fopen(test_file, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "  FAIL: fopen() for read failed");
        return ESP_FAIL;
    }

    size_t read_bytes = fread(read_buf, 1, sizeof(read_buf) - 1, f);
    read_buf[read_bytes] = '\0';

    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "  FAIL: fclose() failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "  Read %zu bytes: \"%s\"", read_bytes, read_buf);

    /* Step 3: 验证数据 */
    ESP_LOGI(TAG, "Step 3: Verify...");

    if (strcmp(read_buf, write_content) == 0) {
        ESP_LOGI(TAG, "  PASS: Data matches!");
    } else {
        ESP_LOGE(TAG, "  FAIL: Data mismatch!");
        ESP_LOGE(TAG, "  Expected: \"%s\"", write_content);
        ESP_LOGE(TAG, "  Got:      \"%s\"", read_buf);
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
    FRESULT res = f_getfree(MOUNT_POINT, &fre_clust, &fs);
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

    ESP_LOGI(TAG, "Storage: Total=%lu KB, Free=%lu KB",
             (unsigned long)*out_total_kb, (unsigned long)*out_free_kb);

    return ESP_OK;
}

/*======================================================================
 * storage_list_wav_files - 列出目录下所有 WAV 文件
 *======================================================================*/
int storage_list_wav_files(const char *dir_path, char file_list[][64], int max_files)
{
    if (!s_mounted || file_list == NULL || max_files <= 0) {
        return 0;
    }

    FF_DIR dir;
    FILINFO fno;
    char path[MAX_FILE_PATH];

    if (dir_path && strlen(dir_path) > 0) {
        snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, dir_path);
    } else {
        snprintf(path, sizeof(path), "%s/", MOUNT_POINT);
    }

    FRESULT res = f_opendir(&dir, path);
    if (res != FR_OK) {
        ESP_LOGW(TAG, "Failed to open directory: %d", res);
        return 0;
    }

    int count = 0;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        if (fno.fattrib & AM_DIR) {
            continue;
        }
        const char *ext = strrchr(fno.fname, '.');
        if (ext && (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".WAV") == 0)) {
            if (count < max_files) {
                strncpy(file_list[count], fno.fname, 63);
                file_list[count][63] = '\0';
                count++;
            }
        }
    }

    f_closedir(&dir);
    return count;
}

/*======================================================================
 * storage_delete_file - 删除文件
 *======================================================================*/
esp_err_t storage_delete_file(const char *file_path)
{
    if (!s_mounted || file_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char full_path[MAX_FILE_PATH];
    if (file_path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", MOUNT_POINT, file_path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", MOUNT_POINT, file_path);
    }

    FRESULT res = f_unlink(full_path);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "Failed to delete file: %d", res);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/*======================================================================
 * storage_file_exists - 检查文件是否存在
 *======================================================================*/
bool storage_file_exists(const char *file_path)
{
    if (!s_mounted || file_path == NULL) {
        return false;
    }

    char full_path[MAX_FILE_PATH];
    if (file_path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", MOUNT_POINT, file_path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", MOUNT_POINT, file_path);
    }

    FILINFO fno;
    return (f_stat(full_path, &fno) == FR_OK && !(fno.fattrib & AM_DIR));
}

/*======================================================================
 * storage_get_file_size - 获取文件大小
 *======================================================================*/
uint32_t storage_get_file_size(const char *file_path)
{
    if (!s_mounted || file_path == NULL) {
        return 0;
    }

    char full_path[MAX_FILE_PATH];
    if (file_path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", MOUNT_POINT, file_path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", MOUNT_POINT, file_path);
    }

    FILINFO fno;
    if (f_stat(full_path, &fno) == FR_OK && !(fno.fattrib & AM_DIR)) {
        return (uint32_t)fno.fsize;
    }

    return 0;
}
