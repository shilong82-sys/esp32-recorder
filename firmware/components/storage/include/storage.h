/**
 * @file storage.h
 * @brief TF 卡存储模块 - 头文件
 *
 * 功能：
 * - FATFS 挂载 / 卸载（VFS 封装）
 * - 文件读写（WAV / 日志 / 配置）
 * - 剩余空间查询
 * - 文件管理（列表、删除）
 *
 * ===== 文件系统策略（强制） =====
 *
 * 本项目统一使用 ESP-IDF VFS/POSIX API：
 *   mkdir(), opendir(), readdir(), stat(), unlink(), fopen()
 *
 * 禁止直接使用 FatFs-native API（f_mkdir, f_opendir, f_stat, f_unlink 等）。
 * esp_vfs_fat_sdspi_mount() 已将 FatFs 封装进 VFS。
 *
 * 原因：
 * - drive mapping 不稳定（取决于 SPI Flash FAT 注册顺序）
 * - FR_INVALID_NAME 问题
 * - 与 VFS mount point 状态不一致
 *
 * 见 docs/storage-path-policy.md。
 */

#ifndef STORAGE_H
#define STORAGE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

/*======================================================================
 * Storage Path Type — 唯一合法的目录标识符
 *
 * 全项目禁止硬编码目录名字符串。
 * 所有路径构造必须通过 storage_build_vfs_path()。
 *======================================================================*/

typedef enum {
    STORAGE_PATH_RECORDINGS,    /**< /sdcard/recordings   — 录音文件 */
    STORAGE_PATH_UPLOADED,      /**< /sdcard/uploaded     — 已上传文件 */
    STORAGE_PATH_UPLOAD_QUEUE,  /**< /sdcard/upload_queue — 待上传队列 */
    STORAGE_PATH_TEMP,          /**< /sdcard/temp         — 临时文件 */
    STORAGE_PATH_LOGS,          /**< /sdcard/logs         — 日志文件 */
    STORAGE_PATH_COUNT,         /**< 枚举计数，不是有效路径类型 */
} storage_path_type_t;

/**
 * @brief 挂载 TF 卡（SDMMC 或 SPI 模式）
 * @param mount_point 挂载点，传 NULL 使用默认 "/sdcard"
 * @return esp_err_t
 */
esp_err_t storage_mount(const char *mount_point);

/**
 * @brief 卸载 TF 卡
 */
void storage_unmount(void);

/**
 * @brief 测试 SD 卡读写（创建 test.txt 并验证）
 * @return ESP_OK=成功，其他=失败
 */
esp_err_t storage_test_rw(void);

/**
 * @brief 获取 TF 卡剩余空间（字节）
 * @param[out] out_total_kb 总空间（KB）
 * @param[out] out_free_kb  剩余空间（KB）
 * @return esp_err_t
 */
esp_err_t storage_get_space(uint32_t *out_total_kb, uint32_t *out_free_kb);

/**
 * @brief 列出目录下所有 WAV 文件
 * @param dir_path 目录名（如 "recordings"），或完整 VFS 路径
 * @param[out] file_list 文件名数组
 * @param max_files 最大文件数
 * @return 实际文件数
 */
int storage_list_wav_files(const char *dir_path, char file_list[][64], int max_files);

/**
 * @brief 删除文件
 * @param file_path 相对路径，如 "recordings/test.wav"
 * @return esp_err_t
 */
esp_err_t storage_delete_file(const char *file_path);

/**
 * @brief 检查文件是否存在
 * @param file_path 文件路径
 * @return true=存在, false=不存在
 */
bool storage_file_exists(const char *file_path);

/**
 * @brief 获取文件大小
 * @param file_path 文件路径
 * @return 文件大小（字节），0 表示不存在
 */
uint32_t storage_get_file_size(const char *file_path);

/**
 * @brief 确保所有必需的子目录存在（recordings, uploaded, upload_queue, temp, logs）
 * 在 storage_mount() 成功后自动调用，也可手动调用。
 * 使用 POSIX mkdir()，errno == EEXIST 视为成功。
 * @return ESP_OK = 全部成功，ESP_FAIL = 部分失败
 */
esp_err_t storage_ensure_directories(void);

/**
 * @brief 验证所有必需的子目录是否存在（启动自检）
 * 只验证，不创建。
 * 使用 POSIX opendir() 验证，打印 [OK] / [MISSING] 格式日志。
 */
void storage_validate_layout(void);

/*======================================================================
 * 路径构造 API（强制，禁止硬编码路径字符串）
 *
 * 示例：
 *
 *   // VFS 路径（用于 fopen/mkdir/stat）
 *   char path[128];
 *   storage_build_vfs_path(path, sizeof(path),
 *                          STORAGE_PATH_RECORDINGS,
 *                          "REC_SESSION_0001.wav");
 *   // → "/sdcard/recordings/REC_SESSION_0001.wav"
 *
 *   // 仅目录路径
 *   storage_build_vfs_path(buf, 128, STORAGE_PATH_TEMP, NULL)
 *     → "/sdcard/temp"
 *======================================================================*/

/**
 * @brief 构造 VFS 路径（用于 fopen/mkdir/stat/opendir）
 *
 * @param[out] out        输出缓冲区
 * @param out_size        缓冲区大小
 * @param type            目录类型（STORAGE_PATH_* 枚举）
 * @param filename        文件名（可为 NULL，表示仅返回目录路径）
 * @return ESP_OK=成功，ESP_ERR_INVALID_SIZE=缓冲区不足
 *
 * 示例：
 *   storage_build_vfs_path(buf, 128, STORAGE_PATH_LOGS, "app.log")
 *     → "/sdcard/logs/app.log"
 *   storage_build_vfs_path(buf, 128, STORAGE_PATH_TEMP, NULL)
 *     → "/sdcard/temp"
 */
esp_err_t storage_build_vfs_path(char *out, size_t out_size,
                                 storage_path_type_t type,
                                 const char *filename);

/**
 * @brief 将路径类型转为目录名字符串（用于日志输出）
 *
 * @param type 目录类型
 * @return 目录名字符串（不含路径），如 "recordings"、"logs"
 *         非法 type 返回 "???"
 *
 * 示例：
 *   storage_path_type_to_string(STORAGE_PATH_LOGS)  → "logs"
 */
const char* storage_path_type_to_string(storage_path_type_t type);

/**
 * @brief 获取 VFS mount point（"/sdcard"）
 * @return mount point 字符串
 */
const char* storage_get_vfs_mount(void);

#endif // STORAGE_H
