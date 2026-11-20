#include "fake_idf.h"
#include "remote_ble.h"
#include "remote_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static rc_core_t core;
static const uint8_t peer[6] = {11, 22, 33, 44, 55, 66};
/** @brief 推进真实适配器。@param 无。@return 无；每次推进测试单调时钟。 */
static void pump(void) {
    fake_now += 10;
    remote_ble_poll(&core, fake_now);
}
/** @brief 注入 GAP 完成事件。@param id 事件。@param status 结果。@return 无。 */
static void gap(int id, int status) {
    esp_ble_gap_cb_param_t p = {0};
    p.scan_param_cmpl.status = status;
    fake_gap(id, &p);
    pump();
}
/** @brief 注入 GATT 事件。@param id 类型。@param p 数据。@return 无。 */
static void gatt(int id, esp_ble_gattc_cb_param_t p) {
    fake_gatt(id, 1, &p);
    pump();
}
/** @brief 初始化真实适配器及核心。@param 无。@return 无；失败断言。 */
static void init(void) {
    bool bound;
    uint8_t mac[6];
    assert(remote_ble_init(&bound, mac) == ESP_OK);
    rc_init(&core, mac, bound, 1234, remote_ble_effect, NULL);
    rc_start(&core, 0);
    gatt(ESP_GATTC_REG_EVT, (esp_ble_gattc_cb_param_t){.reg.status = 0});
}
/** @brief 完成扫描，验证发现目标的地址和地址类型。@param 无。@return 无；失败断言。 */
static void scan(void) {
    gap(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, 0);
    esp_ble_gap_cb_param_t p = {0};
    p.scan_rst.search_evt = ESP_GAP_SEARCH_INQ_RES_EVT;
    memcpy(p.scan_rst.bda, peer, 6);
    p.scan_rst.ble_addr_type = 1;
    p.scan_rst.rssi = -20;
    memcpy(p.scan_rst.ble_adv, (uint8_t[]){6, 255, 0xaa, 8, 0, 0, 0xfa}, 7);
    p.scan_rst.adv_data_len = 7;
    fake_gap(ESP_GAP_BLE_SCAN_RESULT_EVT, &p);
    pump();
    p.scan_rst.search_evt = ESP_GAP_SEARCH_INQ_CMPL_EVT;
    fake_gap(ESP_GAP_BLE_SCAN_RESULT_EVT, &p);
    pump();
    assert(memcmp(fake_open_address, peer, 6) == 0 && fake_open_type == 1);
}
/** @brief 模拟连接与通知订阅完成。@param 无。@return 无；CCCD 之前不得握手。 */
static void link(void) {
    unsigned n = fake_write_count;
    gatt(ESP_GATTC_CONNECT_EVT, (esp_ble_gattc_cb_param_t){.connect.conn_id = 7});
    gatt(ESP_GATTC_OPEN_EVT,
         (esp_ble_gattc_cb_param_t){.open = {.conn_id = 7, .mtu = 23, .status = 0}});
    gatt(ESP_GATTC_CFG_MTU_EVT,
         (esp_ble_gattc_cb_param_t){.cfg_mtu = {.conn_id = 7, .mtu = 247, .status = 0}});
    esp_ble_gattc_cb_param_t p = {0};
    p.search_res.conn_id = 7;
    p.search_res.start_handle = 1;
    p.search_res.end_handle = 30;
    p.search_res.srvc_id.uuid.len = 2;
    p.search_res.srvc_id.uuid.uuid.uuid16 = 0xfff0;
    gatt(ESP_GATTC_SEARCH_RES_EVT, p);
    gatt(ESP_GATTC_SEARCH_CMPL_EVT,
         (esp_ble_gattc_cb_param_t){.search_cmpl = {.conn_id = 7, .status = 0}});
    gatt(ESP_GATTC_REG_FOR_NOTIFY_EVT,
         (esp_ble_gattc_cb_param_t){.reg_for_notify = {.handle = 10, .status = 0}});
    assert(fake_write_count == n);
    gatt(ESP_GATTC_WRITE_DESCR_EVT,
         (esp_ble_gattc_cb_param_t){.write = {.conn_id = 7, .handle = 11, .status = 0}});
    assert(fake_write_count == n + 1);
}
/** @brief 通知实际帧进入接收队列。@param type 帧类型。@param set 集。@param id 命令。@param seq
 * 序号。@param p 载荷。@param length 长度。@return 无。 */
static void notify(uint8_t type, uint8_t set, uint8_t id, uint16_t seq, const uint8_t *p,
                   size_t length) {
    uint8_t bytes[128];
    size_t n = rw_encode(bytes, sizeof bytes, type, set, id, seq, p, length);
    esp_ble_gattc_cb_param_t e = {0};
    e.notify.conn_id = 7;
    e.notify.handle = 10;
    e.notify.value = bytes;
    e.notify.value_len = n;
    gatt(ESP_GATTC_NOTIFY_EVT, e);
}
/** @brief 完成一笔 GATT 写操作。@param 无。@return 无。 */
static void written(void) {
    gatt(ESP_GATTC_WRITE_CHAR_EVT,
         (esp_ble_gattc_cb_param_t){.write = {.conn_id = 7, .handle = 20, .status = 0}});
}
/** @brief 完成真实握手、订阅和状态同步。@param 无。@return 无；验证顺序与首次模式。 */
static void authorize(void) {
    uint8_t auth[33] = {0x44, 0xff};
    auth[26] = 2;
    written();
    notify(2, 0, 0x19, 80, auth, 33);
    written();
    written();
    uint8_t status[38] = {5, 1};
    notify(0, 0x1d, 2, 81, status, 38);
    assert(core.state == RC_READY && core.bound);
}
/** @brief 执行端到端唤醒流程。@param 无。@return 无；校验仅唤醒且满两秒后重连。 */
static void flow(void) {
    init();
    scan();
    link();
    authorize();
    uint8_t status[38] = {5, 0};
    status[28] = 3;
    notify(0, 0x1d, 2, 82, status, 38);
    assert(core.state == RC_ASLEEP);
    unsigned n = fake_write_count;
    assert(rc_button(&core, false, fake_now + 1, fake_now + 10));
    assert(fake_close_calls == 1);
    gatt(ESP_GATTC_DISCONNECT_EVT, (esp_ble_gattc_cb_param_t){.disconnect.conn_id = 7});
    assert(fake_adv_start_calls == 0);
    gap(ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT, 0);
    assert(fake_adv_start_calls == 1);
    gap(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, 0);
    uint64_t start = fake_now;
    fake_now = start + 1990;
    remote_ble_poll(&core, fake_now);
    assert(fake_adv_stop_calls == 0);
    fake_now = start + 2000;
    remote_ble_poll(&core, fake_now);
    assert(fake_adv_stop_calls == 1 && fake_open_calls == 1);
    gap(ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT, 0);
    assert(core.state == RC_CONNECTING);
    assert(fake_write_count == n);
    assert(memcmp(fake_adv, (uint8_t[]){10, 255, 'W', 'K', 'P', 66, 55, 44, 33, 22, 11}, 11) == 0);
    scan();
    link();
    authorize();
    rw_frame_t f;
    for (unsigned i = n; i < fake_write_count; i++) {
        assert(rw_decode(fake_writes[i], fake_write_lengths[i], &f));
        assert(!(f.set == 0 && f.id == 0x11));
    }
    assert(rc_button(&core, false, fake_now + 1, fake_now + 10));
    pump();
    assert(
        rw_decode(fake_writes[fake_write_count - 1], fake_write_lengths[fake_write_count - 1], &f));
    assert(f.id == 0x11);
}
/** @brief 扫描配置失败后再次短按必须能够重试。@param immediate 是否 SDK 即时拒绝。@return
 * 无；永久卡阶段将断言失败。 */
static void scan_failure(bool immediate) {
    if (immediate)
        fake_immediate_error = ESP_FAIL;
    init();
    if (!immediate)
        gap(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, 1);
    pump();
    assert(core.state == RC_OFFLINE);
    int calls = fake_scan_config_calls;
    assert(rc_button(&core, false, fake_now + 1, fake_now + 10));
    pump();
    assert(fake_scan_config_calls == calls + 1);
}
/** @brief 取消中开始新代次，旧成功连接必须先关闭。@param 无。@return
 * 无；漏关旧连接或过早扫描会失败。 */
static void cancel_race(void) {
    init();
    scan();
    rc_io_failed(&core, core.generation, fake_now);
    assert(core.state == RC_OFFLINE);
    assert(rc_button(&core, false, fake_now + 1, fake_now + 10));
    int calls = fake_scan_config_calls;
    gatt(ESP_GATTC_CONNECT_EVT, (esp_ble_gattc_cb_param_t){.connect.conn_id = 7});
    assert(fake_close_calls == 1);
    gatt(ESP_GATTC_CANCEL_OPEN_EVT, (esp_ble_gattc_cb_param_t){.cancel_open.status = 1});
    assert(fake_scan_config_calls == calls);
    gatt(ESP_GATTC_DISCONNECT_EVT, (esp_ble_gattc_cb_param_t){.disconnect.conn_id = 7});
    assert(fake_scan_config_calls == calls + 1);
}
/** @brief 广播失败后可重新唤醒。@param stage 0 为配置失败，1 为开始失败，2 为立即拒绝。@return
 * 无；适配器卡死则失败。 */
static void adv_failure(int stage) {
    init();
    scan();
    link();
    authorize();
    uint8_t status[38] = {5, 0};
    status[28] = 3;
    notify(0, 0x1d, 2, 82, status, 38);
    assert(rc_button(&core, false, fake_now + 1, fake_now + 10));
    if (stage == 2)
        fake_immediate_error = ESP_FAIL;
    gatt(ESP_GATTC_DISCONNECT_EVT, (esp_ble_gattc_cb_param_t){.disconnect.conn_id = 7});
    if (stage == 0)
        gap(ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT, 1);
    if (stage == 1) {
        gap(ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT, 0);
        gap(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, 1);
    }
    assert(core.state == RC_OFFLINE);
    int calls = fake_adv_config_calls;
    assert(rc_button(&core, false, fake_now + 1, fake_now + 10));
    pump();
    assert(fake_adv_config_calls == calls + 1);
}
/** @brief 事件队列丢失时不能继续使用不完整状态。@param 无。@return 无；故障后仍可拍摄则失败。 */
static void queue_overflow(void) {
    init();
    scan();
    link();
    authorize();
    unsigned sent = fake_write_count;
    esp_ble_gap_cb_param_t p = {0};
    for (unsigned i = 0; i < 40; i++)
        fake_gap(999, &p);
    pump();
    assert(core.state == RC_OFFLINE);
    assert(fake_write_count == sent);
}
/** @brief 关闭链路失败完成事件不能永久卡住后续唤醒。@param 无。@return 无；必须允许重新关闭和恢复。
 */
static void close_failure(void) {
    init();
    scan();
    link();
    authorize();
    uint8_t status[38] = {5, 0};
    status[28] = 3;
    notify(0, 0x1d, 2, 82, status, 38);
    assert(rc_button(&core, false, fake_now + 1, fake_now + 10));
    assert(fake_close_calls == 1);
    gatt(ESP_GATTC_CLOSE_EVT, (esp_ble_gattc_cb_param_t){.close = {.conn_id = 7, .status = 1}});
    assert(core.state == RC_OFFLINE && fake_close_calls == 2);
    gatt(ESP_GATTC_DISCONNECT_EVT, (esp_ble_gattc_cb_param_t){.disconnect.conn_id = 7});
    int calls = fake_adv_config_calls;
    assert(rc_button(&core, false, fake_now + 1, fake_now + 10));
    pump();
    assert(fake_adv_config_calls == calls + 1);
}
/** @brief SDK 永远不回完成事件时有最终恢复出口。@param 无。@return 无；30
 * 秒后重启控制器，绝不发送快门。 */
static void lost_callback(void) {
    init();
    unsigned n = fake_write_count;
    fake_now = 40000;
    rc_tick(&core, fake_now);
    remote_ble_poll(&core, fake_now);
    assert(fake_restart_calls == 1 && fake_write_count == n);
}
/** @brief 选择独立进程中的适配测试场景。@param argc 参数数。@param argv 场景名。@return 成功
 * 0，失败断言。 */
int main(int argc, char **argv) {
    assert(argc == 2);
    if (strcmp(argv[1], "flow") == 0)
        flow();
    else if (strcmp(argv[1], "scan_error") == 0)
        scan_failure(false);
    else if (strcmp(argv[1], "immediate_error") == 0)
        scan_failure(true);
    else if (strcmp(argv[1], "cancel_race") == 0)
        cancel_race();
    else if (strcmp(argv[1], "adv_config_error") == 0)
        adv_failure(0);
    else if (strcmp(argv[1], "adv_start_error") == 0)
        adv_failure(1);
    else if (strcmp(argv[1], "adv_immediate_error") == 0)
        adv_failure(2);
    else if (strcmp(argv[1], "queue_overflow") == 0)
        queue_overflow();
    else if (strcmp(argv[1], "close_error") == 0)
        close_failure();
    else if (strcmp(argv[1], "lost_callback") == 0)
        lost_callback();
    else
        assert(false);
    fake_cleanup();
    printf("BLE adapter %s passed\n", argv[1]);
    return 0;
}
