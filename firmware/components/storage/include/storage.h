/**
 * @file storage.h
 * @brief TF 卡存储模块 - 头文件
 *
 * 功能：
 * - FATFS 挂载 / 卸载
 * - 文件读写（WAV / 日志 / 配置）
 * - 剩余空间查询
 * - 文件管理（列表、删除）
 */

#ifndef STORAGE_H
#define STORAGE_H

#include "esp_err.h"
#include "ff.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 挂载 TF 卡（SDMMC 或 SPI 模式）
 * @param mount_point 挂载点，e.g. "/sdcard"
 * @return esp_err_t
 */
esp_err_t storage_mount(const char *mount_point);

/**
 * @brief 卸载 TF 卡
 */
void storage_unmount(void);

/**
 * @brief 获取 TF 卡剩余空间（字节）
 * @param[out] out_total_kb 总空间（KB）
 * @param[out] out_free_kb  剩余空间（KB）
 * @return esp_err_t
 */
esp_err_t storage_get_space(uint32_t *out_total_kb, uint32_t *out_free_kb);

/**
 * @brief 列出目录下所有 WAV 文件
 * @param dir_path   目录路径
 * @param[out] file_list 文件名数组
 * @param max_files 最大文件数
 * @return 实际文件数
 */
int storage_list_wav_files(const char *dir_path, char file_list[][64], int max_files);

/**
 * @brief 删除文件
 * @param file_path 文件路径
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

#endif // STORAGE_H
