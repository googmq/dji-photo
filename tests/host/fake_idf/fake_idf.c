#include "fake_idf.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
fake_gap_fn fake_gap;
fake_gatt_fn fake_gatt;
uint64_t fake_now;
int fake_adv_config_calls;
int fake_scan_config_calls, fake_open_calls, fake_adv_start_calls, fake_adv_stop_calls,
    fake_close_calls;
int fake_immediate_error;
unsigned fake_write_count;
uint8_t fake_writes[64][64], fake_open_address[6], fake_adv[11];
size_t fake_write_lengths[64];
int fake_open_type;
/** 模拟队列，只替代 FreeRTOS 调度边界，保存真实适配器事件内容。 */
typedef struct {
    size_t capacity, size, head, count;
    uint8_t data[];
} fake_queue_t;
static fake_queue_t *queue;
static uint8_t saved[8];
static bool saved_valid;
/** @brief 分配队列。@param capacity 容量。@param size 元素大小。@return 队列或断言失败。 */
QueueHandle_t xQueueCreate(size_t capacity, size_t size) {
    queue = calloc(1, sizeof *queue + capacity * size);
    assert(queue);
    queue->capacity = capacity;
    queue->size = size;
    return queue;
}
/** @brief 复制入队。@param q 队列。@param item 数据。@param timeout 未使用。@return 满时 0，否则
 * 1。 */
int xQueueSend(QueueHandle_t q, const void *item, int timeout) {
    (void)timeout;
    fake_queue_t *f = q;
    if (f->count == f->capacity)
        return 0;
    memcpy(f->data + ((f->head + f->count) % f->capacity) * f->size, item, f->size);
    f->count++;
    return 1;
}
/** @brief 复制出队。@param q 队列。@param item 输出。@param timeout 未使用。@return 空时 0，否则
 * 1。 */
int xQueueReceive(QueueHandle_t q, void *item, int timeout) {
    (void)timeout;
    fake_queue_t *f = q;
    if (!f->count)
        return 0;
    memcpy(item, f->data + f->head * f->size, f->size);
    f->head = (f->head + 1) % f->capacity;
    f->count--;
    return 1;
}
/** @brief 释放测试资源。@param 无。@return 无。 */
void fake_cleanup(void) {
    free(queue);
    queue = NULL;
}
/** @brief 获取测试时钟。@param 无。@return 微秒。 */
int64_t esp_timer_get_time(void) {
    return (int64_t)fake_now * 1000;
}
/** @brief 提供测试错误名。@param err 错误码。@return 固定字符串。 */
const char *esp_err_to_name(int err) {
    (void)err;
    return "fake error";
}
/** @brief 消费一次注入错误。@param 无。@return 错误码，随后恢复成功。 */
static int result(void) {
    int err = fake_immediate_error;
    fake_immediate_error = 0;
    return err;
}
/* 以下替身只记录 SDK 边界或返回夹具；真正的协议和适配逻辑来自 remote 目录。 */
/**
 * @brief 记录扫描配置请求，支持注入即时失败。
 * @param p 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gap_set_scan_params(const esp_ble_scan_params_t *p) {
    (void)p;
    fake_scan_config_calls++;
    return result();
}
/**
 * @brief 验证扫描持续时间并返回可注入结果。
 * @param seconds 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gap_start_scanning(int seconds) {
    assert(seconds == 4);
    return result();
}
/**
 * @brief 模拟提交停止扫描请求，完成事件由用例另行注入。
 * @param 无。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gap_stop_scanning(void) {
    return result();
}
/**
 * @brief 保存真实适配器生成的唤醒广播用于字节校验。
 * @param data 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param length 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gap_config_adv_data_raw(uint8_t *data, size_t length) {
    fake_adv_config_calls++;
    assert(length == 11);
    memcpy(fake_adv, data, 11);
    return result();
}
/**
 * @brief 记录广播开始请求，避免把受理当成已经广播。
 * @param p 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gap_start_advertising(const esp_ble_adv_params_t *p) {
    (void)p;
    fake_adv_start_calls++;
    return result();
}
/**
 * @brief 记录停止广播请求，用于验证两秒时序。
 * @param 无。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gap_stop_advertising(void) {
    fake_adv_stop_calls++;
    return result();
}
/**
 * @brief 记录目标地址与地址类型，连接结果由用例注入。
 * @param interface 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param address 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param type 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param direct 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gattc_open(int interface, uint8_t *address, int type, bool direct) {
    (void)interface;
    assert(direct);
    fake_open_calls++;
    memcpy(fake_open_address, address, 6);
    fake_open_type = type;
    return result();
}
/**
 * @brief 记录关闭旧连接请求，完成结果由用例注入。
 * @param interface 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param conn 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gattc_close(int interface, int conn) {
    (void)interface;
    (void)conn;
    fake_close_calls++;
    return result();
}
/**
 * @brief 模拟取消请求受理，允许注入取消与成功建连竞态。
 * @param p 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gattc_cancel_open(const esp_ble_gattc_cancel_open_params_t *p) {
    (void)p;
    return result();
}
/**
 * @brief 模拟 MTU 请求受理，协商结果由用例注入。
 * @param interface 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param conn 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gattc_send_mtu_req(int interface, int conn) {
    (void)interface;
    (void)conn;
    return result();
}
/**
 * @brief 模拟服务发现受理，特征数据由夹具提供。
 * @param interface 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param conn 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param filter 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gattc_search_service(int interface, int conn, void *filter) {
    (void)interface;
    (void)conn;
    (void)filter;
    return result();
}
/**
 * @brief 校验通知句柄并模拟注册受理。
 * @param interface 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param address 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param handle 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gattc_register_for_notify(int interface, uint8_t *address, int handle) {
    (void)interface;
    (void)address;
    assert(handle == 10);
    return result();
}
/**
 * @brief 提供固定通知及写入特征，供真实适配器发现。
 * @param interface 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param conn 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param start 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param end 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param uuid 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param r 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param count 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gattc_get_char_by_uuid(int interface, int conn, int start, int end,
                                         esp_bt_uuid_t uuid, esp_gattc_char_elem_t *r,
                                         uint16_t *count) {
    (void)interface;
    (void)conn;
    (void)start;
    (void)end;
    *count = 1;
    r->char_handle = uuid.uuid.uuid16 == 0xfff4 ? 10 : 20;
    r->properties = ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_WRITE;
    return result();
}
/**
 * @brief 提供 CCCD 描述符并校验 UUID。
 * @param interface 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param conn 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param handle 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param uuid 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param r 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param count 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gattc_get_descr_by_char_handle(int interface, int conn, int handle,
                                                 esp_bt_uuid_t uuid, esp_gattc_descr_elem_t *r,
                                                 uint16_t *count) {
    (void)interface;
    (void)conn;
    (void)handle;
    assert(uuid.uuid.uuid16 == 0x2902);
    *count = 1;
    r->handle = 11;
    return result();
}
/**
 * @brief 验证真实适配器启用通知的字节与句柄。
 * @param interface 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param conn 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param handle 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param length 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param data 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param type 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param auth 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gattc_write_char_descr(int interface, int conn, int handle, size_t length,
                                         uint8_t *data, int type, int auth) {
    (void)interface;
    (void)conn;
    (void)type;
    (void)auth;
    assert(handle == 11 && length == 2 && data[0] == 1);
    return result();
}
/**
 * @brief 记录真实协议帧，供用例验证命令与时序。
 * @param interface 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param conn 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param handle 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param length 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param data 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param type 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param auth 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gattc_write_char(int interface, int conn, int handle, size_t length,
                                   uint8_t *data, int type, int auth) {
    (void)interface;
    (void)conn;
    (void)type;
    (void)auth;
    assert(handle == 20 && length <= 64 && fake_write_count < 64);
    memcpy(fake_writes[fake_write_count], data, length);
    fake_write_lengths[fake_write_count++] = length;
    return result();
}
/**
 * @brief 提供测试 NVS 句柄，不访问用户机器存储。
 * @param name 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param mode 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param handle 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    (void)name;
    (void)mode;
    *handle = 1;
    return ESP_OK;
}
/**
 * @brief 返回已持久化夹具，未绑定时返回未找到。
 * @param handle 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param key 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param value 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param length 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t nvs_get_blob(int handle, const char *key, void *value, size_t *length) {
    (void)handle;
    (void)key;
    if (!saved_valid)
        return ESP_ERR_NVS_NOT_FOUND;
    assert(*length >= sizeof saved);
    memcpy(value, saved, sizeof saved);
    *length = sizeof saved;
    return ESP_OK;
}
/**
 * @brief 保存真实绑定序列化结果到内存。
 * @param handle 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param key 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param value 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param length 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t nvs_set_blob(int handle, const char *key, const void *value, size_t length) {
    (void)handle;
    (void)key;
    assert(length == sizeof saved);
    memcpy(saved, value, length);
    saved_valid = true;
    return ESP_OK;
}
/**
 * @brief 删除测试绑定记录。
 * @param handle 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param key 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t nvs_erase_key(int handle, const char *key) {
    (void)handle;
    (void)key;
    saved_valid = false;
    return ESP_OK;
}
/**
 * @brief 确认内存绑定提交，不写真实闪存。
 * @param handle 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t nvs_commit(int handle) {
    (void)handle;
    return ESP_OK;
}
/**
 * @brief 结束测试句柄生命周期，无外部副作用。
 * @param handle 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return 无；夹具不合法时断言失败，无异常。
 */
void nvs_close(int handle) {
    (void)handle;
}
/**
 * @brief 模拟可用的 NVS 初始化。
 * @param 无。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t nvs_flash_init(void) {
    return ESP_OK;
}
/**
 * @brief 提供稳定的本机 BLE 地址用于握手。
 * @param mac 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @param kind 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_read_mac(uint8_t *mac, int kind) {
    (void)kind;
    memcpy(mac, "\1\2\3\4\5\6", 6);
    return ESP_OK;
}
/**
 * @brief 模拟蓝牙控制器初始化。
 * @param cfg 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_bt_controller_init(const esp_bt_controller_config_t *cfg) {
    (void)cfg;
    return ESP_OK;
}
/**
 * @brief 模拟 BLE 控制器启用。
 * @param mode 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_bt_controller_enable(int mode) {
    (void)mode;
    return ESP_OK;
}
/**
 * @brief 模拟 Bluedroid 初始化。
 * @param 无。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_bluedroid_init(void) {
    return ESP_OK;
}
/**
 * @brief 模拟 Bluedroid 启用。
 * @param 无。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_bluedroid_enable(void) {
    return ESP_OK;
}
/**
 * @brief 保存生产 GAP 回调供测试驱动真实适配器。
 * @param fn 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gap_register_callback(fake_gap_fn fn) {
    fake_gap = fn;
    return ESP_OK;
}
/**
 * @brief 保存生产 GATT 回调供测试驱动真实适配器。
 * @param fn 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gattc_register_callback(fake_gatt_fn fn) {
    fake_gatt = fn;
    return ESP_OK;
}
/**
 * @brief 模拟本地 MTU 配置。
 * @param mtu 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gatt_set_local_mtu(int mtu) {
    (void)mtu;
    return ESP_OK;
}
/**
 * @brief 模拟应用注册受理，注册完成事件由用例注入。
 * @param id 对应 ESP-IDF 接口参数；仅用于校验或内存夹具。
 * @return ESP_OK 或测试注入的错误；夹具不合法时断言失败，无异常。
 */
esp_err_t esp_ble_gattc_app_register(int id) {
    (void)id;
    return ESP_OK;
}

int fake_restart_calls;
/** @brief 记录丢失 SDK 完成事件时的受控重启，不重启测试机器。@param 无。@return 无；无异常。 */
void esp_restart(void) {
    fake_restart_calls++;
}
