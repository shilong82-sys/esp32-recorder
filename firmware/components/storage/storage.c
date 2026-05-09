/**
 * @file storage.c
 * @brief TF 卡存储模块 - 源文件
 *
 * 功能：
 * - FATFS 挂载 / 卸载（SDMMC 模式）
 * - 文件读写（WAV / 日志 / 配置）
 * - 剩余空间查询
 * - 文件管理（列表、删除）
 */

#include "storage.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "storage";

#define MOUNT_POINT "/sdcard"
#define MAX_FILE_PATH 128

static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;

/**
 * @brief 挂载 TF 卡（SDMMC 1-bit 模式，兼容性强）
 */
esp_err_t storage_mount(const char *mount_point)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "Storage already mounted");
        return ESP_OK;
    }

    const char *mount = mount_point ? mount_point : MOUNT_POINT;

    // SDMMC 主机配置
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // FATFS 挂载配置
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,   // 挂载失败时格式化
        .max_files = 5,                   // 最多同时打开 5 个文件
        .allocation_unit_size = 16 * 1024
    };

    ESP_LOGI(TAG, "Mounting SD card at %s...", mount);

    // 使用默认槽位配置（自动使用默认引脚）
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount, &host, NULL, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted successfully");

    // 打印卡信息
    sdmmc_card_print_info(stdout, s_card);

    return ESP_OK;
}

/**
 * @brief 卸载 TF 卡
 */
void storage_unmount(void)
{
    if (!s_mounted) {
        return;
    }

    const char *mount = MOUNT_POINT;
    esp_vfs_fat_sdcard_unmount(mount, s_card);
    s_card = NULL;
    s_mounted = false;

    ESP_LOGI(TAG, "SD card unmounted");
}

/**
 * @brief 获取 TF 卡空间信息
 */
esp_err_t storage_get_space(uint32_t *out_total_kb, uint32_t *out_free_kb)
{
    if (!s_mounted) {
        return ESP_FAIL;
    }

    FATFS *fs;
    DWORD fre_clust, tot_sect;
    FRESULT res;

    // 获取文件系统信息
    res = f_getfree(MOUNT_POINT, &fre_clust, &fs);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "f_getfree failed: %d", res);
        return ESP_FAIL;
    }

    // 计算总扇区和可用扇区
    tot_sect = (fs->n_fatent - 2) * fs->csize;
    DWORD fre_sect = fre_clust * fs->csize;

    // 转换为 KB
    uint32_t sector_size = fs->ssize ? fs->ssize : 512;
    if (out_total_kb) {
        *out_total_kb = (tot_sect * sector_size) / 1024;
    }
    if (out_free_kb) {
        *out_free_kb = (fre_sect * sector_size) / 1024;
    }

    ESP_LOGI(TAG, "Total: %lu KB, Free: %lu KB", *out_total_kb, *out_free_kb);

    return ESP_OK;
}

/**
 * @brief 列出目录下所有 WAV 文件
 */
int storage_list_wav_files(const char *dir_path, char file_list[][64], int max_files)
{
    if (!s_mounted || file_list == NULL || max_files <= 0) {
        return 0;
    }

    FF_DIR dir;
    FILINFO fno;
    char path[MAX_FILE_PATH];
    int count = 0;

    // 构建完整路径
    if (dir_path && strlen(dir_path) > 0) {
        snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, dir_path);
    } else {
        snprintf(path, sizeof(path), "%s/", MOUNT_POINT);
    }

    ESP_LOGD(TAG, "Listing WAV files in: %s", path);

    // 打开目录
    FRESULT res = f_opendir(&dir, path);
    if (res != FR_OK) {
        ESP_LOGW(TAG, "Failed to open directory: %d", res);
        return 0;
    }

    // 遍历目录
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        // 跳过目录
        if (fno.fattrib & AM_DIR) {
            continue;
        }

        // 检查是否为 WAV 文件
        const char *ext = strrchr(fno.fname, '.');
        if (ext && (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".WAV") == 0)) {
            if (count < max_files) {
                strncpy(file_list[count], fno.fname, 63);
                file_list[count][63] = '\0';
                ESP_LOGD(TAG, "Found: %s", file_list[count]);
                count++;
            }
        }
    }

    f_closedir(&dir);
    ESP_LOGI(TAG, "Found %d WAV files", count);

    return count;
}

/**
 * @brief 删除文件
 */
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

    ESP_LOGI(TAG, "Deleting file: %s", full_path);

    FRESULT res = f_unlink(full_path);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "Failed to delete file: %d", res);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "File deleted successfully");
    return ESP_OK;
}

/**
 * @brief 检查文件是否存在
 */
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
    FRESULT res = f_stat(full_path, &fno);
    return (res == FR_OK && !(fno.fattrib & AM_DIR));
}

/**
 * @brief 获取文件大小
 */
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
    FRESULT res = f_stat(full_path, &fno);
    if (res == FR_OK && !(fno.fattrib & AM_DIR)) {
        return (uint32_t)fno.fsize;
    }

    return 0;
}
