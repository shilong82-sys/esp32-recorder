/**
 * @file event_bus.h
 * @brief 全局事件总线 - 头文件
 *
 * 所有模块间通信均通过 event_bus 进行，实现解耦。
 * 采用发布/订阅模式，支持最多 32 个监听器。
 */

#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 事件类型枚举
 */
typedef enum {
    /* ---- 状态机事件 ---- */
    EVENT_STATE_CHANGED,        /*!< 设备状态变化 */

    /* ---- 按键事件 ---- */
    EVENT_BUTTON_PRESSED,       /*!< 按键按下 */
    EVENT_BUTTON_RELEASED,      /*!< 按键松开 */
    EVENT_BUTTON_CLICKED,       /*!< 单击（短按松开） */
    EVENT_BUTTON_DOUBLE_CLICKED,/*!< 双击 */
    EVENT_BUTTON_LONG_PRESSED,  /*!< 长按触发（首次到达阈值） */
    EVENT_BUTTON_HOLD,          /*!< 持续按住（HOLD 后每 500ms 重复） */

    /* ---- WiFi 事件 ---- */
    EVENT_WIFI_CONNECTED,      /*!< WiFi 已连接 */
    EVENT_WIFI_DISCONNECTED,   /*!< WiFi 断开 */

    /* ---- 存储事件 ---- */
    EVENT_STORAGE_READY,        /*!< TF 卡就绪 */
    EVENT_STORAGE_ERROR,        /*!< TF 卡错误 */

    /* ---- 电池事件 ---- */
    EVENT_BATTERY_LOW,          /*!< 电量低 */
    EVENT_BATTERY_CRITICAL,     /*!< 电量极低 */

    /* ---- 录音事件 ---- */
    EVENT_RECORDING_STARTED,    /*!< 录音开始 */
    EVENT_RECORDING_STOPPED,    /*!< 录音停止 */

    /* ---- 上传事件 ---- */
    EVENT_UPLOAD_STARTED,       /*!< 上传开始 */
    EVENT_UPLOAD_PROGRESS,      /*!< 上传进度更新 */
    EVENT_UPLOAD_DONE,          /*!< 上传完成 */
    EVENT_UPLOAD_FAILED,        /*!< 上传失败 */

    /* ---- 扩展预留 ---- */
    EVENT_TYPE_COUNT,
} event_type_t;

/**
 * @brief 事件回调函数类型
 * @param type      事件类型
 * @param data      事件数据（可为 NULL）
 * @param data_len  数据长度
 * @param user_data 用户参数（注册时传入）
 */
typedef void (*event_cb_t)(event_type_t type, const void *data, size_t data_len, void *user_data);

/**
 * @brief 事件句柄（用于注销）
 */
typedef int event_handler_t;

/**
 * @brief 事件数据结构（STATE_CHANGED 使用）
 */
typedef struct {
    int prev_state;
    int curr_state;
} event_state_data_t;

/**
 * @brief 按键事件数据结构
 */
typedef struct {
    int gpio_num;
} event_button_data_t;

/**
 * @brief 上传进度事件数据
 */
typedef struct {
    int progress_percent;       /*!< 进度百分比 (0~100) */
    uint32_t bytes_sent;        /*!< 已发送字节数 */
    uint32_t bytes_total;       /*!< 文件总字节数 */
} event_upload_progress_data_t;

/**
 * @brief 上传结果事件数据（EVENT_UPLOAD_DONE / EVENT_UPLOAD_FAILED 使用）
 */
typedef struct {
    char filename[64];          /*!< 上传文件名（不含路径） */
    bool success;               /*!< true=上传成功, false=上传失败 */
    int http_status;            /*!< HTTP 状态码（200=成功，其余=失败） */
} event_upload_result_data_t;

/* ---- APIs ---- */

/**
 * @brief 初始化事件总线
 * @note  通常在 app_main 中调用一次
 */
void event_bus_init(void);

/**
 * @brief 订阅事件
 * @param type      关注的事件类型
 * @param cb        回调函数
 * @param user_data 用户参数（回调时原样传入）
 * @return  订阅句柄（用于后续 unsubscribe），失败返回 -1
 */
event_handler_t event_bus_subscribe(event_type_t type, event_cb_t cb, void *user_data);

/**
 * @brief 取消订阅
 * @param handle event_bus_subscribe() 返回的句柄
 */
void event_bus_unsubscribe(event_handler_t handle);

/**
 * @brief 发布事件（同步分发：所有订阅者回调在 publish 返回前执行完毕）
 * @param type  事件类型
 * @param data  事件数据（可为 NULL）
 * @param len   数据长度
 */
void event_bus_publish(event_type_t type, const void *data, size_t len);

/**
 * @brief 获取事件类型对应的字符串名称（用于日志）
 * @param type 事件类型
 * @return     字符串，外部不负责释放
 */
const char* event_type_name(event_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BUS_H */
