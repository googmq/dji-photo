#ifndef FAKE_IDF_H
#define FAKE_IDF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;
typedef int esp_gatt_if_t;
typedef int esp_gatt_write_type_t;
typedef int esp_gap_ble_cb_event_t;
typedef int esp_gattc_cb_event_t;
typedef int nvs_handle_t;
typedef uint8_t esp_bd_addr_t[6];
typedef void *QueueHandle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_NVS_NOT_FOUND 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_GATT_OK 0
#define ESP_BT_STATUS_SUCCESS 0
#define ESP_UUID_LEN_16 2
#define ESP_GATT_CHAR_PROP_BIT_NOTIFY 16
#define ESP_GATT_CHAR_PROP_BIT_WRITE 8
#define ESP_GATT_CHAR_PROP_BIT_WRITE_NR 4
#define ESP_GATT_WRITE_TYPE_RSP 1
#define ESP_GATT_WRITE_TYPE_NO_RSP 2
#define ESP_GATT_AUTH_REQ_NONE 0
#define BLE_SCAN_TYPE_ACTIVE 1
#define BLE_ADDR_TYPE_PUBLIC 0
#define BLE_SCAN_FILTER_ALLOW_ALL 0
#define BLE_SCAN_DUPLICATE_DISABLE 0
#define ADV_TYPE_IND 0
#define ADV_CHNL_ALL 7
#define ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY 0
#define ESP_GAP_SEARCH_INQ_RES_EVT 1
#define ESP_GAP_SEARCH_INQ_CMPL_EVT 2
#define ESP_BT_MODE_BLE 1
#define ESP_MAC_BT 1
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define pdTRUE 1
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() {0}
#define ESP_LOGE(tag, ...)                                                                         \
    do {                                                                                           \
        (void)(tag);                                                                               \
        if (0)                                                                                     \
            printf(__VA_ARGS__);                                                                   \
    } while (0)
#define ESP_LOGI ESP_LOGE
#define ESP_LOGW ESP_LOGE

/** 测试替身事件标识，与实际 SDK 通过同名常量交互，不验证 ABI。 */
enum {
    ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT = 1, /**< 扫描参数配置完成。 */
    ESP_GAP_BLE_SCAN_START_COMPLETE_EVT,         /**< 扫描启动完成。 */
    ESP_GAP_BLE_SCAN_RESULT_EVT,                 /**< 扫描结果或自然结束。 */
    ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT,          /**< 主动停止扫描完成。 */
    ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT,   /**< 原始广播数据配置完成。 */
    ESP_GAP_BLE_ADV_START_COMPLETE_EVT,          /**< 广播启动完成。 */
    ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT            /**< 广播停止完成。 */
};
/** 测试替身事件标识，与实际 SDK 通过同名常量交互，不验证 ABI。 */
enum {
    ESP_GATTC_REG_EVT = 1,        /**< GATT 应用注册完成。 */
    ESP_GATTC_DISCONNECT_EVT,     /**< 物理连接断开。 */
    ESP_GATTC_CANCEL_OPEN_EVT,    /**< 取消建连请求完成。 */
    ESP_GATTC_CONNECT_EVT,        /**< 物理连接建立。 */
    ESP_GATTC_OPEN_EVT,           /**< GATT 虚拟连接打开完成。 */
    ESP_GATTC_CFG_MTU_EVT,        /**< MTU 协商完成。 */
    ESP_GATTC_SEARCH_RES_EVT,     /**< 发现服务。 */
    ESP_GATTC_SEARCH_CMPL_EVT,    /**< 服务发现结束。 */
    ESP_GATTC_REG_FOR_NOTIFY_EVT, /**< 通知注册完成。 */
    ESP_GATTC_WRITE_DESCR_EVT,    /**< CCCD 写入完成。 */
    ESP_GATTC_WRITE_CHAR_EVT,     /**< 特征写入完成。 */
    ESP_GATTC_NOTIFY_EVT,         /**< 收到相机通知。 */
    ESP_GATTC_CLOSE_EVT           /**< 关闭虚拟连接请求完成。 */
};
typedef struct {
    int len;
    union {
        uint16_t uuid16;
    } uuid;
} esp_bt_uuid_t;
typedef struct {
    int status;
} fake_status_t;
typedef union {
    fake_status_t scan_param_cmpl, scan_start_cmpl, scan_stop_cmpl, adv_data_raw_cmpl,
        adv_start_cmpl, adv_stop_cmpl;
    struct {
        int search_evt;
        uint8_t bda[6], ble_addr_type, ble_adv[62];
        size_t adv_data_len, scan_rsp_len;
        int rssi;
    } scan_rst;
} esp_ble_gap_cb_param_t;
typedef union {
    fake_status_t reg, cancel_open;
    struct {
        uint16_t conn_id;
        int status;
    } disconnect, connect, close;
    struct {
        uint16_t conn_id, mtu;
        int status;
    } open, cfg_mtu;
    struct {
        uint16_t conn_id, start_handle, end_handle;
        struct {
            esp_bt_uuid_t uuid;
        } srvc_id;
    } search_res;
    struct {
        uint16_t conn_id;
        int status;
    } search_cmpl;
    struct {
        uint16_t handle;
        int status;
    } reg_for_notify;
    struct {
        uint16_t conn_id, handle;
        int status;
    } write;
    struct {
        uint16_t conn_id, handle, value_len;
        uint8_t *value;
    } notify;
} esp_ble_gattc_cb_param_t;
typedef struct {
    int scan_type, own_addr_type, scan_filter_policy, scan_interval, scan_window, scan_duplicate;
} esp_ble_scan_params_t;
typedef struct {
    int adv_int_min, adv_int_max, adv_type, own_addr_type, channel_map, adv_filter_policy;
} esp_ble_adv_params_t;
typedef struct {
    int gattc_if;
    uint8_t remote_bda[6];
} esp_ble_gattc_cancel_open_params_t;
typedef struct {
    uint16_t char_handle;
    int properties;
} esp_gattc_char_elem_t;
typedef struct {
    uint16_t handle;
} esp_gattc_descr_elem_t;
typedef struct {
    int unused;
} esp_bt_controller_config_t;
typedef void (*fake_gap_fn)(esp_gap_ble_cb_event_t, esp_ble_gap_cb_param_t *);
typedef void (*fake_gatt_fn)(esp_gattc_cb_event_t, esp_gatt_if_t, esp_ble_gattc_cb_param_t *);

extern fake_gap_fn fake_gap;
extern fake_gatt_fn fake_gatt;
extern uint64_t fake_now;
extern int fake_adv_config_calls;
extern int fake_restart_calls;
void esp_restart(void);
extern int fake_scan_config_calls, fake_open_calls, fake_adv_start_calls, fake_adv_stop_calls,
    fake_close_calls;
extern int fake_immediate_error;
extern unsigned fake_write_count;
extern uint8_t fake_writes[64][64];
extern size_t fake_write_lengths[64];
extern uint8_t fake_open_address[6], fake_adv[11];
extern int fake_open_type;
QueueHandle_t xQueueCreate(size_t capacity, size_t item_size);
int xQueueSend(QueueHandle_t q, const void *item, int timeout);
int xQueueReceive(QueueHandle_t q, void *item, int timeout);
void fake_cleanup(void);
int64_t esp_timer_get_time(void);
const char *esp_err_to_name(int err);
esp_err_t esp_ble_gap_set_scan_params(const esp_ble_scan_params_t *p);
esp_err_t esp_ble_gap_start_scanning(int seconds);
esp_err_t esp_ble_gap_stop_scanning(void);
esp_err_t esp_ble_gap_config_adv_data_raw(uint8_t *data, size_t length);
esp_err_t esp_ble_gap_start_advertising(const esp_ble_adv_params_t *p);
esp_err_t esp_ble_gap_stop_advertising(void);
esp_err_t esp_ble_gattc_open(int interface, uint8_t *address, int type, bool direct);
esp_err_t esp_ble_gattc_close(int interface, int conn);
esp_err_t esp_ble_gattc_cancel_open(const esp_ble_gattc_cancel_open_params_t *p);
esp_err_t esp_ble_gattc_send_mtu_req(int interface, int conn);
esp_err_t esp_ble_gattc_search_service(int interface, int conn, void *filter);
esp_err_t esp_ble_gattc_register_for_notify(int interface, uint8_t *address, int handle);
esp_err_t esp_ble_gattc_get_char_by_uuid(int interface, int conn, int start, int end,
                                         esp_bt_uuid_t uuid, esp_gattc_char_elem_t *result,
                                         uint16_t *count);
esp_err_t esp_ble_gattc_get_descr_by_char_handle(int interface, int conn, int handle,
                                                 esp_bt_uuid_t uuid, esp_gattc_descr_elem_t *result,
                                                 uint16_t *count);
esp_err_t esp_ble_gattc_write_char_descr(int interface, int conn, int handle, size_t length,
                                         uint8_t *data, int type, int auth);
esp_err_t esp_ble_gattc_write_char(int interface, int conn, int handle, size_t length,
                                   uint8_t *data, int type, int auth);
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle);
esp_err_t nvs_get_blob(int handle, const char *key, void *value, size_t *length);
esp_err_t nvs_set_blob(int handle, const char *key, const void *value, size_t length);
esp_err_t nvs_erase_key(int handle, const char *key);
esp_err_t nvs_commit(int handle);
void nvs_close(int handle);
esp_err_t nvs_flash_init(void);
esp_err_t esp_read_mac(uint8_t *mac, int kind);
esp_err_t esp_bt_controller_init(const esp_bt_controller_config_t *cfg);
esp_err_t esp_bt_controller_enable(int mode);
esp_err_t esp_bluedroid_init(void);
esp_err_t esp_bluedroid_enable(void);
esp_err_t esp_ble_gap_register_callback(fake_gap_fn fn);
esp_err_t esp_ble_gattc_register_callback(fake_gatt_fn fn);
esp_err_t esp_ble_gatt_set_local_mtu(int mtu);
esp_err_t esp_ble_gattc_app_register(int id);
#endif
