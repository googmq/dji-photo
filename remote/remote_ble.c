#include "remote_ble.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "remote_wire.h"
#include <stdatomic.h>
#include <string.h>

#define TAG "remote_ble"
#define TX_CAPACITY 8

/** BLE 适配阶段，用于过滤异步操作完成事件。 */
typedef enum {
    BL_IDLE,        /**< 无活动连接和扫描。 */
    BL_SCAN_CONFIG, /**< 等待扫描参数设置成功。 */
    BL_SCANNING,    /**< 收集兼容相机广播。 */
    BL_OPENING,     /**< 正在建立物理连接。 */
    BL_DISCOVER,    /**< 协商 MTU、查找特征与启用通知。 */
    BL_READY,       /**< FFF4 通知和 FFF5 写入可用。 */
    BL_ADV_CONFIG,  /**< 配置 WKP 广播。 */
    BL_ADV_START,   /**< 等待广播实际启动。 */
    BL_ADV_RUN,     /**< 持续广播两秒。 */
    BL_ADV_STOP,    /**< 等待广播停止再连接。 */
    BL_CLOSING,     /**< 等待旧连接关闭。 */
    BL_SCAN_CANCEL, /**< 等待旧扫描停止。 */
    BL_OPEN_CANCEL  /**< 等待连接请求取消。 */
} bl_phase_t;

/** 已验证相机的持久化地址，version 防止错误读取未来格式。 */
typedef struct {
    uint8_t version, address[6], address_type;
} bl_binding_t;
/** 回调事件副本；通知内容独立保存，绝不保留 SDK 临时指针。 */
typedef struct {
    bool gap;
    int id;
    esp_gatt_if_t interface;
    uint32_t generation;
    uint64_t at_ms;
    union {
        esp_ble_gap_cb_param_t gap;
        esp_ble_gattc_cb_param_t gatt;
    } param;
    uint8_t bytes[512];
} bl_event_t;
/** 单任务拥有的 BLE 运行数据；跨任务仅队列和原子标记。 */
typedef struct {
    bl_phase_t phase;
    bl_phase_t observed_phase;
    uint64_t phase_since_ms;
    bool restarting;
    esp_gatt_if_t interface;
    bool registered, bound, connected, found, writing, failed, mtu_started;
    bl_binding_t binding;
    uint8_t peer[6], peer_type;
    int rssi;
    uint16_t conn, service_start, service_end, notify_handle, write_handle, descriptor, mtu;
    esp_gatt_write_type_t write_type;
    uint32_t generation;
    uint32_t requested_generation;
    rc_effect_kind_t requested;
    bool have_request;
    uint64_t adv_until_ms;
    rc_effect_t tx[TX_CAPACITY];
    size_t tx_head, tx_count;
    rw_stream_t stream;
} bl_context_t;
static bl_context_t bl;
static QueueHandle_t events;
static atomic_bool overflow;
static atomic_uint callback_generation;

/** @brief 将异步事件入队。@param e 已复制事件。@return 无；队列满置故障标记，不阻塞蓝牙线程。 */
static void enqueue(bl_event_t *e) {
    e->generation = atomic_load(&callback_generation);
    e->at_ms = (uint64_t)esp_timer_get_time() / 1000;
    if (xQueueSend(events, e, 0) != pdTRUE)
        atomic_store(&overflow, true);
}
/** @brief GAP 回调只复制事件。@param event 类型。@param param SDK 数据。@return
 * 无；入队失败交主循环处理。 */
static void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    bl_event_t e = {.gap = true, .id = event};
    e.param.gap = *param;
    enqueue(&e);
}
/** @brief GATT 回调复制通知内容。@param event 类型。@param interface 客户端接口。@param param SDK
 * 数据。@return 无；超长通知视为传输故障。 */
static void gatt_callback(esp_gattc_cb_event_t event, esp_gatt_if_t interface,
                          esp_ble_gattc_cb_param_t *param) {
    bl_event_t e = {.id = event, .interface = interface};
    e.param.gatt = *param;
    if (event == ESP_GATTC_NOTIFY_EVT) {
        if (param->notify.value_len > sizeof e.bytes) {
            atomic_store(&overflow, true);
            return;
        }
        memcpy(e.bytes, param->notify.value, param->notify.value_len);
    }
    enqueue(&e);
}
/** @brief 检查 SDK 调用返回值。@param result 错误码。@return 成功
 * true，失败记录待处理故障；无异常。 */
static bool check(esp_err_t result) {
    if (result == ESP_OK)
        return true;
    ESP_LOGE(TAG, "BLE operation failed: %s", esp_err_to_name(result));
    bl.failed = true;
    return false;
}
/** @brief 在旧操作结束后启动请求。@param 无。@return 无；异步失败由 poll 处理。 */
static void begin_request(void) {
    if (!bl.have_request || !bl.registered || bl.phase != BL_IDLE)
        return;
    bl.have_request = false;
    bl.generation = bl.requested_generation;
    bl.failed = false;
    bl.tx_count = 0;
    bl.writing = false;
    bl.stream.used = 0;
    atomic_store(&callback_generation, bl.generation);
    if (bl.requested == RC_EFFECT_WAKE) {
        if (!bl.bound) {
            bl.failed = true;
            return;
        }
        uint8_t adv[11];
        rw_wake_advertisement(adv, bl.binding.address);
        bl.phase = BL_ADV_CONFIG;
        if (!check(esp_ble_gap_config_adv_data_raw(adv, sizeof adv)))
            bl.phase = BL_IDLE;
        return;
    }
    bl.found = false;
    bl.rssi = -128;
    bl.phase = BL_SCAN_CONFIG;
    esp_ble_scan_params_t params = {.scan_type = BLE_SCAN_TYPE_ACTIVE,
                                    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                                    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
                                    .scan_interval = 0x50,
                                    .scan_window = 0x30,
                                    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE};
    if (!check(esp_ble_gap_set_scan_params(&params)))
        bl.phase = BL_IDLE;
}
/** @brief 取消旧操作，为下个请求建立异步屏障。@param 无。@return 无；完成事件到达前不启动新连接。
 */
static void cancel_current(void) {
    bl.tx_count = 0;
    bl.stream.used = 0;
    if (bl.phase == BL_CLOSING || bl.phase == BL_SCAN_CANCEL || bl.phase == BL_OPEN_CANCEL ||
        bl.phase == BL_ADV_STOP)
        return;
    if (bl.connected) {
        bl.phase = BL_CLOSING;
        if (!check(esp_ble_gattc_close(bl.interface, bl.conn)))
            bl.phase = BL_READY;
    } else if (bl.phase == BL_OPENING) {
        bl.phase = BL_OPEN_CANCEL;
        esp_ble_gattc_cancel_open_params_t params = {.gattc_if = bl.interface};
        memcpy(params.remote_bda, bl.peer, 6);
        if (!check(esp_ble_gattc_cancel_open(&params)))
            bl.phase = BL_OPENING;
    } else if (bl.phase == BL_SCANNING) {
        bl.phase = BL_SCAN_CANCEL;
        if (!check(esp_ble_gap_stop_scanning()))
            bl.phase = BL_SCANNING;
    } else if (bl.phase == BL_ADV_RUN) {
        bl.phase = BL_ADV_STOP;
        if (!check(esp_ble_gap_stop_advertising()))
            bl.phase = BL_ADV_RUN;
    } else if (bl.phase == BL_SCAN_CONFIG || bl.phase == BL_ADV_CONFIG ||
               bl.phase == BL_ADV_START) {
        /* 配置调用尚未完成；完成处理器根据 have_request/请求类型决定取消。 */
    } else {
        bl.phase = BL_IDLE;
        begin_request();
    }
}
/** @brief 保存当前授权相机。@param 无。@return 是否保存成功，失败不上报绑定成功。 */
static bool save_binding(void) {
    bl_binding_t value = {.version = 1, .address_type = bl.peer_type};
    memcpy(value.address, bl.peer, 6);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("dji_remote", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "camera", &value, sizeof value);
        if (err == ESP_OK)
            err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot save binding: %s", esp_err_to_name(err));
        return false;
    }
    bl.binding = value;
    bl.bound = true;
    return true;
}
bool remote_ble_effect(void *context, const rc_effect_t *e) {
    (void)context;
    switch (e->kind) {
    case RC_EFFECT_CONNECT:
    case RC_EFFECT_WAKE:
        bl.requested_generation = e->generation;
        bl.requested = e->kind;
        bl.have_request = true;
        cancel_current();
        return true;
    case RC_EFFECT_DISCONNECT:
        bl.have_request = false;
        bl.requested = RC_EFFECT_DISCONNECT;
        cancel_current();
        return true;
    case RC_EFFECT_BIND:
        if (bl.phase != BL_READY)
            return false;
        if (bl.bound && memcmp(bl.peer, bl.binding.address, 6) == 0 &&
            bl.peer_type == bl.binding.address_type)
            return true;
        return save_binding();
    case RC_EFFECT_SEND:
        if (bl.phase != BL_READY || e->generation != bl.generation || e->length > bl.mtu - 3U ||
            bl.tx_count == TX_CAPACITY)
            return false;
        bl.tx[(bl.tx_head + bl.tx_count) % TX_CAPACITY] = *e;
        bl.tx_count++;
        return true;
    }
    return false;
}
/** @brief 扫描结束后连接候选相机。@param 无。@return 无；未找到设备报告错误。 */
static void open_candidate(void) {
    if (bl.phase != BL_SCANNING)
        return;
    if (!bl.found) {
        bl.phase = BL_IDLE;
        bl.failed = true;
        return;
    }
    bl.phase = BL_OPENING;
    bl.mtu = 23;
    bl.mtu_started = false;
    bl.service_start = bl.service_end = bl.notify_handle = bl.write_handle = bl.descriptor = 0;
    ESP_LOGI(TAG, "Connecting %02x:%02x:%02x:%02x:%02x:%02x (type %u)", bl.peer[0], bl.peer[1],
             bl.peer[2], bl.peer[3], bl.peer[4], bl.peer[5], bl.peer_type);
    if (!check(esp_ble_gattc_open(bl.interface, bl.peer, bl.peer_type, true)))
        bl.phase = BL_IDLE;
}
/** @brief 推进 GAP 异步操作。@param e 队列事件。@param core 控制核心。@param now 当前毫秒。@return
 * 无；错误标记在 bl.failed。 */
static void handle_gap(const bl_event_t *e, rc_core_t *core, uint64_t now) {
    const esp_ble_gap_cb_param_t *p = &e->param.gap;
    switch (e->id) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (bl.phase != BL_SCAN_CONFIG)
            break;
        if (p->scan_param_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            bl.phase = BL_IDLE;
            bl.failed = true;
            break;
        }
        if (bl.have_request || bl.requested == RC_EFFECT_DISCONNECT) {
            bl.phase = BL_IDLE;
            begin_request();
            break;
        }
        bl.phase = BL_SCANNING;
        if (!check(esp_ble_gap_start_scanning(4)))
            bl.phase = BL_IDLE;
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (p->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            bl.phase = BL_IDLE;
            bl.failed = true;
        }
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (bl.phase != BL_SCANNING)
            break;
        if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            open_candidate();
            break;
        }
        if (p->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT)
            break;
        if (bl.bound) {
            if (memcmp(p->scan_rst.bda, bl.binding.address, 6) != 0 ||
                p->scan_rst.ble_addr_type != bl.binding.address_type)
                break;
        } else if (!rw_is_camera_advertisement(p->scan_rst.ble_adv, p->scan_rst.adv_data_len) &&
                   !rw_is_camera_advertisement(p->scan_rst.ble_adv + p->scan_rst.adv_data_len,
                                               p->scan_rst.scan_rsp_len))
            break;
        if (p->scan_rst.rssi > bl.rssi && (bl.bound || p->scan_rst.rssi >= -80)) {
            bl.found = true;
            bl.rssi = p->scan_rst.rssi;
            memcpy(bl.peer, p->scan_rst.bda, 6);
            bl.peer_type = p->scan_rst.ble_addr_type;
        }
        break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (bl.phase == BL_SCAN_CANCEL) {
            if (p->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                bl.phase = BL_SCANNING;
                bl.failed = true;
                break;
            }
            bl.phase = BL_IDLE;
            begin_request();
        }
        break;
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        if (bl.phase != BL_ADV_CONFIG)
            break;
        if (p->adv_data_raw_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            bl.phase = BL_IDLE;
            bl.failed = true;
            break;
        }
        if (bl.have_request || bl.requested == RC_EFFECT_DISCONNECT) {
            bl.phase = BL_IDLE;
            begin_request();
            break;
        }
        {
            esp_ble_adv_params_t params = {.adv_int_min = 0x20,
                                           .adv_int_max = 0x40,
                                           .adv_type = ADV_TYPE_IND,
                                           .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                                           .channel_map = ADV_CHNL_ALL,
                                           .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY};
            bl.phase = BL_ADV_START;
            if (!check(esp_ble_gap_start_advertising(&params)))
                bl.phase = BL_IDLE;
        }
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (bl.phase != BL_ADV_START)
            break;
        if (p->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            bl.phase = BL_IDLE;
            bl.failed = true;
            break;
        }
        bl.phase = BL_ADV_RUN;
        bl.adv_until_ms = now + 2000;
        if (bl.have_request || bl.requested == RC_EFFECT_DISCONNECT)
            cancel_current();
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (bl.phase != BL_ADV_STOP)
            break;
        if (p->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            bl.phase = BL_ADV_RUN;
            bl.failed = true;
            break;
        }
        bl.phase = BL_IDLE;
        if (bl.have_request)
            begin_request();
        else if (bl.requested == RC_EFFECT_WAKE)
            rc_wake_complete(core, bl.generation, now);
        break;
    default:
        break;
    }
}
/** @brief 获取特征及启动通知注册。@param 无。@return 无；缺少特征或属性时失败。 */
static void discover_characteristics(void) {
    esp_gattc_char_elem_t notify, write;
    uint16_t n = 1;
    esp_bt_uuid_t uuid = {.len = ESP_UUID_LEN_16, .uuid.uuid16 = 0xfff4};
    if (!bl.service_start ||
        esp_ble_gattc_get_char_by_uuid(bl.interface, bl.conn, bl.service_start, bl.service_end,
                                       uuid, &notify, &n) != ESP_GATT_OK ||
        n != 1 || !(notify.properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)) {
        bl.failed = true;
        return;
    }
    n = 1;
    uuid.uuid.uuid16 = 0xfff5;
    if (esp_ble_gattc_get_char_by_uuid(bl.interface, bl.conn, bl.service_start, bl.service_end,
                                       uuid, &write, &n) != ESP_GATT_OK ||
        n != 1 ||
        !(write.properties & (ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR))) {
        bl.failed = true;
        return;
    }
    bl.notify_handle = notify.char_handle;
    bl.write_handle = write.char_handle;
    bl.write_type = (write.properties & ESP_GATT_CHAR_PROP_BIT_WRITE) ? ESP_GATT_WRITE_TYPE_RSP
                                                                      : ESP_GATT_WRITE_TYPE_NO_RSP;
    check(esp_ble_gattc_register_for_notify(bl.interface, bl.peer, bl.notify_handle));
}
/** 帧回调的当前会话及接收时间。 */
typedef struct {
    rc_core_t *core;
    uint32_t generation;
    uint64_t now;
} bl_frame_context_t;
/** @brief 将完整帧交给真实核心。@param context 会话信息。@param bytes 帧。@param length
 * 长度。@return 无；错误帧由核心忽略。 */
static void on_frame(void *context, const uint8_t *bytes, size_t length) {
    bl_frame_context_t *f = context;
    rc_receive(f->core, f->generation, bytes, length, f->now);
}
/** @brief 推进 GATT 建连和收发。@param e 事件。@param core 核心。@param now 当前毫秒。@return
 * 无；旧连接事件忽略。 */
static void handle_gatt(const bl_event_t *e, rc_core_t *core, uint64_t now) {
    const esp_ble_gattc_cb_param_t *p = &e->param.gatt;
    if (e->id == ESP_GATTC_REG_EVT) {
        if (p->reg.status != ESP_GATT_OK) {
            bl.failed = true;
            return;
        }
        bl.interface = e->interface;
        bl.registered = true;
        begin_request();
        return;
    }
    if (e->id == ESP_GATTC_CLOSE_EVT && bl.phase == BL_CLOSING && p->close.conn_id == bl.conn &&
        p->close.status != ESP_GATT_OK) {
        /* 失败完成事件不会再带来断开通知，恢复可取消状态让用户能够重试。 */
        bl.phase = BL_READY;
        bl.failed = true;
        return;
    }
    if (e->id == ESP_GATTC_DISCONNECT_EVT) {
        if (!bl.connected || p->disconnect.conn_id != bl.conn)
            return;
        uint32_t generation = e->generation;
        bool intentional = bl.phase == BL_CLOSING;
        bl.connected = false;
        bl.phase = BL_IDLE;
        bl.writing = false;
        bl.tx_count = 0;
        bl.stream.used = 0;
        if (intentional)
            begin_request();
        else
            rc_link_lost(core, generation, now);
        return;
    }
    if (e->id == ESP_GATTC_CANCEL_OPEN_EVT && bl.phase == BL_OPEN_CANCEL) {
        bl.phase = BL_IDLE;
        if (p->cancel_open.status != ESP_GATT_OK)
            bl.failed = true;
        else
            begin_request();
        return;
    }
    if (e->generation != bl.generation)
        return;
    switch (e->id) {
    case ESP_GATTC_CONNECT_EVT:
        /* 取消与成功建连可能交错；先回收物理连接，不能按新请求的编号丢掉旧连接。 */
        bl.connected = true;
        bl.conn = p->connect.conn_id;
        if (bl.phase != BL_OPENING) {
            bl.phase = BL_CLOSING;
            if (!check(esp_ble_gattc_close(bl.interface, bl.conn)))
                bl.phase = BL_READY;
            break;
        }
        bl.phase = BL_DISCOVER;
        break;
    case ESP_GATTC_OPEN_EVT:
        if (p->open.status != ESP_GATT_OK) {
            if (bl.phase == BL_OPENING) {
                bl.phase = BL_IDLE;
                bl.failed = true;
            }
            break;
        }
        if (bl.phase != BL_DISCOVER && bl.phase != BL_OPENING) {
            if (!bl.connected) {
                bl.connected = true;
                bl.conn = p->open.conn_id;
                cancel_current();
            }
            break;
        }
        bl.connected = true;
        bl.conn = p->open.conn_id;
        bl.phase = BL_DISCOVER;
        if (!bl.mtu_started) {
            bl.mtu_started = true;
            check(esp_ble_gattc_send_mtu_req(bl.interface, bl.conn));
        }
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        if (bl.phase != BL_DISCOVER || p->cfg_mtu.conn_id != bl.conn)
            break;
        if (p->cfg_mtu.status != ESP_GATT_OK || p->cfg_mtu.mtu < 54) {
            bl.failed = true;
            break;
        }
        bl.mtu = p->cfg_mtu.mtu;
        check(esp_ble_gattc_search_service(bl.interface, bl.conn, NULL));
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        if (bl.phase == BL_DISCOVER && p->search_res.conn_id == bl.conn &&
            p->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            p->search_res.srvc_id.uuid.uuid.uuid16 == 0xfff0) {
            bl.service_start = p->search_res.start_handle;
            bl.service_end = p->search_res.end_handle;
        }
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (bl.phase != BL_DISCOVER || p->search_cmpl.conn_id != bl.conn)
            break;
        if (p->search_cmpl.status != ESP_GATT_OK) {
            bl.failed = true;
            break;
        }
        discover_characteristics();
        break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        if (bl.phase != BL_DISCOVER || p->reg_for_notify.handle != bl.notify_handle)
            break;
        if (p->reg_for_notify.status != ESP_GATT_OK) {
            bl.failed = true;
            break;
        }
        {
            uint16_t n = 1;
            esp_gattc_descr_elem_t d;
            esp_bt_uuid_t uuid = {.len = ESP_UUID_LEN_16, .uuid.uuid16 = 0x2902};
            if (esp_ble_gattc_get_descr_by_char_handle(bl.interface, bl.conn, bl.notify_handle,
                                                       uuid, &d, &n) != ESP_GATT_OK ||
                n != 1) {
                bl.failed = true;
                break;
            }
            uint8_t enabled[2] = {1, 0};
            bl.descriptor = d.handle;
            check(esp_ble_gattc_write_char_descr(bl.interface, bl.conn, d.handle, 2, enabled,
                                                 ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE));
        }
        break;
    case ESP_GATTC_WRITE_DESCR_EVT:
        if (bl.phase != BL_DISCOVER || p->write.conn_id != bl.conn ||
            p->write.handle != bl.descriptor)
            break;
        if (p->write.status != ESP_GATT_OK) {
            bl.failed = true;
            break;
        }
        bl.phase = BL_READY;
        rc_link_ready(core, e->generation, now);
        break;
    case ESP_GATTC_WRITE_CHAR_EVT:
        if (bl.phase != BL_READY || p->write.conn_id != bl.conn)
            break;
        bl.writing = false;
        if (p->write.status != ESP_GATT_OK)
            bl.failed = true;
        break;
    case ESP_GATTC_NOTIFY_EVT:
        if (bl.phase != BL_READY || p->notify.conn_id != bl.conn ||
            p->notify.handle != bl.notify_handle)
            break;
        {
            if (now > e->at_ms + 2000) {
                bl.failed = true;
                break;
            }
            bl_frame_context_t f = {core, e->generation, now};
            rw_feed(&bl.stream, e->bytes, p->notify.value_len, on_frame, &f);
        }
        break;
    default:
        break;
    }
}
void remote_ble_poll(rc_core_t *core, uint64_t now) {
    if (bl.restarting)
        return;
    if (atomic_exchange(&overflow, false)) {
        ESP_LOGE(TAG, "BLE event queue overflow; resetting session");
        bl.failed = true;
    }
    bl_event_t e;
    for (unsigned i = 0; i < 16 && xQueueReceive(events, &e, 0) == pdTRUE; i++) {
        if (e.gap)
            handle_gap(&e, core, now);
        else
            handle_gatt(&e, core, now);
    }
    if (bl.failed) {
        bl.failed = false;
        rc_io_failed(core, bl.have_request ? bl.requested_generation : bl.generation, now);
        /* 若取消也立即失败，保留可再次取消的活动阶段；避免主循环无限重试。 */
        bl.failed = false;
    }
    if (bl.phase == BL_ADV_RUN && now >= bl.adv_until_ms) {
        bl.phase = BL_ADV_STOP;
        if (!check(esp_ble_gap_stop_advertising()))
            bl.phase = BL_ADV_RUN;
    }
    if (bl.observed_phase != bl.phase) {
        bl.observed_phase = bl.phase;
        bl.phase_since_ms = now;
    }
    /* SDK 完成事件若丢失，单纯超时取消仍可能永远等待同一事件。
     * 只在传输过渡阶段卡住 30 秒时重启 MCU；不会重发任何拍摄命令。 */
    if ((!bl.registered || (bl.phase != BL_IDLE && bl.phase != BL_READY)) &&
        now - bl.phase_since_ms >= 30000) {
        bl.restarting = true;
        ESP_LOGE(TAG, "BLE phase %d stalled for 30s; restarting controller", bl.phase);
        esp_restart();
        return;
    }
    if (core->state == RC_ASLEEP || core->state == RC_WAKING)
        bl.tx_count = 0;
    if (bl.phase == BL_READY && !bl.writing && bl.tx_count) {
        rc_effect_t *tx = &bl.tx[bl.tx_head];
        bl.writing = true;
        check(esp_ble_gattc_write_char(bl.interface, bl.conn, bl.write_handle, tx->length, tx->data,
                                       bl.write_type, ESP_GATT_AUTH_REQ_NONE));
        bl.tx_head = (bl.tx_head + 1) % TX_CAPACITY;
        bl.tx_count--;
    }
}
esp_err_t remote_ble_init(bool *bound, uint8_t mac[6]) {
    bl.phase_since_ms = (uint64_t)esp_timer_get_time() / 1000;
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
        return err;
    nvs_handle_t handle;
    err = nvs_open("dji_remote", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = sizeof bl.binding;
        err = nvs_get_blob(handle, "camera", &bl.binding, &size);
        nvs_close(handle);
        bl.bound = err == ESP_OK && size == sizeof bl.binding && bl.binding.version == 1 &&
                   bl.binding.address_type <= 3;
    } else if (err != ESP_ERR_NVS_NOT_FOUND)
        return err;
    *bound = bl.bound;
    err = esp_read_mac(mac, ESP_MAC_BT);
    if (err != ESP_OK)
        return err;
    events = xQueueCreate(32, sizeof(bl_event_t));
    if (!events)
        return ESP_ERR_NO_MEM;
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((err = esp_bt_controller_init(&cfg)) != ESP_OK)
        return err;
    if ((err = esp_bt_controller_enable(ESP_BT_MODE_BLE)) != ESP_OK)
        return err;
    if ((err = esp_bluedroid_init()) != ESP_OK)
        return err;
    if ((err = esp_bluedroid_enable()) != ESP_OK)
        return err;
    if ((err = esp_ble_gap_register_callback(gap_callback)) != ESP_OK)
        return err;
    if ((err = esp_ble_gattc_register_callback(gatt_callback)) != ESP_OK)
        return err;
    if ((err = esp_ble_gatt_set_local_mtu(247)) != ESP_OK)
        return err;
    return esp_ble_gattc_app_register(0);
}
esp_err_t remote_ble_forget(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("dji_remote", NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return err;
    err = nvs_erase_key(handle, "camera");
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
