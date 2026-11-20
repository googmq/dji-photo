#ifndef REMOTE_CORE_H
#define REMOTE_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 遥控器业务状态，只有控制任务可以修改。 */
typedef enum {
    RC_OFFLINE,       /**< 尚未连接或上次操作结果未知。 */
    RC_CONNECTING,    /**< 正在建立 BLE 通道及通知订阅。 */
    RC_HANDSHAKE,     /**< 等待相机授权 DJI 协议连接。 */
    RC_SYNCING,       /**< 已授权，等待当前连接的相机状态。 */
    RC_READY,         /**< 相机清醒且状态有效，可接收按键。 */
    RC_CAPTURING,     /**< 已发送一次快门，等待应答和后续状态。 */
    RC_WAIT_IDLE,     /**< 长按后等待拍照完成或录像停止。 */
    RC_SLEEP_PENDING, /**< 已发休眠命令，等待确认。 */
    RC_ASLEEP,        /**< 已确认休眠，禁止普通 GATT 发送。 */
    RC_WAKING         /**< 正在广播唤醒，只恢复连接、不补发拍摄。 */
} rc_state_t;

/** 核心向平台请求的副作用，回调不能重入核心。 */
typedef enum {
    RC_EFFECT_CONNECT,    /**< 扫描并连接当前绑定相机，未绑定时寻找兼容相机。 */
    RC_EFFECT_DISCONNECT, /**< 取消连接或关闭当前链路。 */
    RC_EFFECT_WAKE,       /**< 发送目标相机的 WKP 广播。 */
    RC_EFFECT_SEND,       /**< 向 FFF5 写入完整 DJI 帧。 */
    RC_EFFECT_BIND        /**< 授权成功后持久化当前相机身份。 */
} rc_effect_kind_t;

/** 最近一次失败原因；状态机不会根据失败自动重发快门。 */
typedef enum {
    RC_ERROR_NONE,        /**< 尚无错误。 */
    RC_ERROR_IO,          /**< 传输、持久化或事件队列失败。 */
    RC_ERROR_TIMEOUT,     /**< 操作超时，真实相机结果可能未知。 */
    RC_ERROR_REJECTED,    /**< 相机明确拒绝连接或命令。 */
    RC_ERROR_UNSUPPORTED, /**< 当前拍摄模式不在第一版支持范围。 */
    RC_ERROR_STALE        /**< 未获得新鲜的相机状态。 */
} rc_error_t;

/** 发送给平台的一次动作；data 仅在回调期间有效，平台须复制。 */
typedef struct {
    rc_effect_kind_t kind;
    uint32_t generation;
    uint8_t data[64];
    size_t length;
} rc_effect_t;

/** @brief 执行副作用。@param context 平台上下文。@param effect 动作。@return 已受理为 true，失败为
 * false；不抛异常，不得重入核心。 */
typedef bool (*rc_emit_fn)(void *context, const rc_effect_t *effect);

/** 相机数据快照；screen-off 与 power=3 的休眠含义不同。 */
typedef struct {
    uint8_t mode;
    uint8_t activity;
    uint8_t power;
    uint16_t countdown_seconds; /**< 当前倒计时剩余秒数，不是配置的倒计时时长。 */
    uint64_t received_ms;
    bool valid;
} rc_status_t;

/** 单线程控制上下文，期限使用平台提供的单调毫秒时间。 */
typedef struct {
    rc_state_t state;
    rc_error_t error;
    rc_status_t status;
    uint32_t generation;
    uint16_t sequence;
    uint16_t pending_seq;
    uint8_t pending_set;
    uint8_t pending_id;
    uint8_t local_mac[6];
    uint8_t camera_id[4];
    uint16_t pairing_code;
    bool bound;
    bool acknowledged;
    uint64_t deadline_ms;
    uint64_t ready_since_ms; /**< 最近进入可接受手势状态的时间。 */
    uint64_t clock_ms;       /**< 最近一次调用的单调时间，供传输失败路径使用。 */
    uint64_t operation_ms;
    rc_emit_fn emit;
    void *context;
} rc_core_t;

/** @brief 初始化控制核心。@param c 输出上下文。@param mac 本机 BLE 地址。@param bound
 * 是否已有绑定。@param code 配对码。@param emit 动作回调。@param context 回调参数。@return
 * 无；参数必须非空，不抛异常。 */
void rc_init(rc_core_t *c, const uint8_t mac[6], bool bound, uint16_t code, rc_emit_fn emit,
             void *context);
/** @brief 启动连接。@param c 核心。@param now 单调毫秒。@return 无；错误记录在 c->error。 */
void rc_start(rc_core_t *c, uint64_t now);
/** @brief CCCD 写成功后开始协议握手。@param c 核心。@param generation 连接编号。@param now
 * 单调毫秒。@return 无；旧编号忽略。 */
void rc_link_ready(rc_core_t *c, uint32_t generation, uint64_t now);
/** @brief 接收一个完整协议帧。@param c 核心。@param generation 连接编号。@param bytes 帧。@param
 * length 字节数。@param now 单调毫秒。@return 无；非法帧丢弃。 */
void rc_receive(rc_core_t *c, uint32_t generation, const uint8_t *bytes, size_t length,
                uint64_t now);
/** @brief 处理手势。@param c 核心。@param long_press 是否长按。@param pressed_ms
 * 消抖后按下时间。@param now 当前毫秒。@return 是否受理；忙碌事件不缓存，无异常。 */
bool rc_button(rc_core_t *c, bool long_press, uint64_t pressed_ms, uint64_t now);
/** @brief 推进超时。@param c 核心。@param now 单调毫秒。@return 无；超时撤销就绪且不重发拍摄。 */
void rc_tick(rc_core_t *c, uint64_t now);
/** @brief 处理链路断开。@param c 核心。@param generation 连接编号。@param now 单调毫秒。@return
 * 无；清醒时尝试一次完整重连。 */
void rc_link_lost(rc_core_t *c, uint32_t generation, uint64_t now);
/** @brief 广播已停止后开始重连。@param c 核心。@param generation 唤醒编号。@param now
 * 单调毫秒。@return 无；不生成快门。 */
void rc_wake_complete(rc_core_t *c, uint32_t generation, uint64_t now);
/** @brief 平台故障时撤销就绪。@param c 核心。@param generation 操作编号。@param now
 * 单调毫秒。@return 无；不自动重发。
 */
void rc_io_failed(rc_core_t *c, uint32_t generation, uint64_t now);
#endif
