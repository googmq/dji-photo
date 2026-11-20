#include "remote_core.h"
#include "remote_wire.h"
#include <string.h>

#define STATUS_MAX_AGE_MS 2000
#define COMMAND_TIMEOUT_MS 5000

/** @brief 撤销所有可执行状态并关闭链路。@param c 核心。@param error 原因。@return
 * 无；关闭失败仍保持离线，不抛异常。 */
static void fail(rc_core_t *c, rc_error_t error) {
    c->state = RC_OFFLINE;
    c->ready_since_ms = c->clock_ms;
    c->error = error;
    c->status.valid = false;
    rc_effect_t e = {.kind = RC_EFFECT_DISCONNECT, .generation = c->generation};
    (void)c->emit(c->context, &e);
}
/** @brief 执行动作并统一处理拒绝受理。@param c 核心。@param e 动作。@return 成功 true，失败离线
 * false；无异常。 */
static bool emit(rc_core_t *c, const rc_effect_t *e) {
    if (c->emit(c->context, e))
        return true;
    fail(c, RC_ERROR_IO);
    return false;
}
/** @brief 发起平台操作。@param c 核心。@param kind 操作类型。@return 受理结果；失败撤销就绪。 */
static bool action(rc_core_t *c, rc_effect_kind_t kind) {
    rc_effect_t e = {.kind = kind, .generation = c->generation};
    return emit(c, &e);
}
/** @brief 发送协议帧，不在此处等待应答。@param c 核心。@param type 帧类型。@param set 集。@param id
 * 命令。@param seq 序号。@param data 载荷。@param length 长度。@return 是否受理；失败离线。 */
static bool send_frame(rc_core_t *c, uint8_t type, uint8_t set, uint8_t id, uint16_t seq,
                       const uint8_t *data, size_t length) {
    rc_effect_t e = {.kind = RC_EFFECT_SEND, .generation = c->generation};
    e.length = rw_encode(e.data, sizeof e.data, type, set, id, seq, data, length);
    if (!e.length) {
        fail(c, RC_ERROR_IO);
        return false;
    }
    return emit(c, &e);
}
/** @brief 开始一个有应答命令。@param c 核心。@param set 集。@param id 命令。@param data
 * 载荷。@param length 长度。@param now 毫秒。@return 受理结果；无重试。 */
static bool command(rc_core_t *c, uint8_t set, uint8_t id, const uint8_t *data, size_t length,
                    uint64_t now) {
    c->clock_ms = now;
    c->pending_seq = ++c->sequence;
    c->pending_set = set;
    c->pending_id = id;
    c->acknowledged = false;
    c->operation_ms = now;
    c->deadline_ms = now + COMMAND_TIMEOUT_MS;
    return send_frame(c, 2, set, id, c->pending_seq, data, length);
}
/** @brief 开始新的连接会话。@param c 核心。@param now 毫秒。@return 无；平台失败离线。 */
static void connect(rc_core_t *c, uint64_t now) {
    c->generation++;
    c->state = RC_CONNECTING;
    c->status.valid = false;
    c->error = RC_ERROR_NONE;
    c->deadline_ms = now + 20000;
    action(c, RC_EFFECT_CONNECT);
}
/** @brief 进入就绪且消除过渡期间手势。@param c 核心。@param now 毫秒。@return 无；无异常。 */
static void set_ready(rc_core_t *c, uint64_t now) {
    c->state = RC_READY;
    c->ready_since_ms = now;
}
/** @brief 发送休眠命令。@param c 核心。@param now 毫秒。@return 无；应答之前保持过渡状态。 */
static void sleep_camera(rc_core_t *c, uint64_t now) {
    const uint8_t power = 3;
    c->state = RC_SLEEP_PENDING;
    command(c, 0, 0x1a, &power, 1, now);
}
/** @brief 尝试完成等待状态。@param c 核心。@param now 毫秒。@return
 * 无；只有命令后的有效状态才能确认结果。 */
static void settle(rc_core_t *c, uint64_t now) {
    if (!c->status.valid || c->status.received_ms <= c->operation_ms || !c->acknowledged)
        return;
    if (c->state == RC_CAPTURING)
        set_ready(c, now);
    else if (c->state == RC_WAIT_IDLE && c->status.countdown_seconds == 0 &&
             (c->status.activity == 0 || c->status.activity == 1))
        sleep_camera(c, now);
}
void rc_init(rc_core_t *c, const uint8_t mac[6], bool bound, uint16_t code, rc_emit_fn fn,
             void *context) {
    memset(c, 0, sizeof *c);
    memcpy(c->local_mac, mac, 6);
    c->bound = bound;
    c->pairing_code = code;
    c->emit = fn;
    c->context = context;
}
void rc_start(rc_core_t *c, uint64_t now) {
    c->clock_ms = now;
    if (c->state == RC_OFFLINE)
        connect(c, now);
}
void rc_link_ready(rc_core_t *c, uint32_t generation, uint64_t now) {
    c->clock_ms = now;
    if (generation != c->generation || c->state != RC_CONNECTING)
        return;
    uint8_t p[33] = {0};
    memcpy(p, c->local_mac + 2, 4);
    p[4] = 6;
    memcpy(p + 5, c->local_mac, 6);
    p[26] = c->bound ? 0 : 1;
    p[27] = (uint8_t)c->pairing_code;
    p[28] = (uint8_t)(c->pairing_code >> 8);
    c->state = RC_HANDSHAKE;
    if (command(c, 0, 0x19, p, sizeof p, now))
        c->deadline_ms = now + 30000;
}
void rc_receive(rc_core_t *c, uint32_t generation, const uint8_t *bytes, size_t length,
                uint64_t now) {
    c->clock_ms = now;
    rw_frame_t f;
    if (generation != c->generation || !rw_decode(bytes, length, &f))
        return;
    if (c->state == RC_ASLEEP || c->state == RC_WAKING || c->state == RC_OFFLINE ||
        c->state == RC_CONNECTING)
        return;
    bool ack = (f.type & 0x20) != 0;
    if (c->state == RC_HANDSHAKE) {
        if (f.set != 0 || f.id != 0x19)
            return;
        if (ack) {
            if (f.seq == c->pending_seq && f.length >= 5 && f.payload[4] != 0)
                fail(c, RC_ERROR_REJECTED);
            return;
        }
        if (f.length < 33 || f.payload[26] != 2)
            return;
        if (f.payload[27] != 0 || f.payload[28] != 0) {
            fail(c, RC_ERROR_REJECTED);
            return;
        }
        memcpy(c->camera_id, f.payload, 4);
        uint8_t response[9] = {0};
        memcpy(response, c->local_mac + 2, 4);
        c->state = RC_SYNCING;
        c->deadline_ms = now + 5000;
        if (!send_frame(c, 0x20, 0, 0x19, f.seq, response, sizeof response))
            return;
        if (!action(c, RC_EFFECT_BIND))
            return;
        c->bound = true;
        const uint8_t subscription[6] = {3, 20, 0, 0, 0, 0};
        send_frame(c, 0, 0x1d, 5, ++c->sequence, subscription, sizeof subscription);
        return;
    }
    if (!ack && f.set == 0x1d && f.id == 2 && f.length >= 29) {
        c->status = (rc_status_t){.mode = f.payload[0],
                                  .activity = f.payload[1],
                                  .power = f.payload[28],
                                  .countdown_seconds =
                                      (uint16_t)(f.payload[9] | (uint16_t)f.payload[10] << 8),
                                  .received_ms = now,
                                  .valid = true};
        if (c->status.power == 3) {
            c->state = RC_ASLEEP;
            c->ready_since_ms = now;
            return;
        }
        if (c->status.power != 0) {
            fail(c, RC_ERROR_STALE);
            return;
        }
        if (c->state == RC_SYNCING)
            set_ready(c, now);
        else
            settle(c, now);
        return;
    }
    if (ack && !c->acknowledged && f.seq == c->pending_seq && f.set == c->pending_set &&
        f.id == c->pending_id && f.length >= 1 &&
        (c->state == RC_CAPTURING || c->state == RC_SLEEP_PENDING || c->state == RC_WAIT_IDLE)) {
        if (f.payload[0] != 0) {
            fail(c, RC_ERROR_REJECTED);
            return;
        }
        c->acknowledged = true;
        if (c->state == RC_SLEEP_PENDING) {
            c->state = RC_ASLEEP;
            c->ready_since_ms = now;
            c->status.power = 3;
        } else
            settle(c, now);
    }
}
bool rc_button(rc_core_t *c, bool long_press, uint64_t pressed_ms, uint64_t now) {
    c->clock_ms = now;
    if ((c->state == RC_ASLEEP || c->state == RC_OFFLINE) && !long_press) {
        if (pressed_ms < c->ready_since_ms)
            return false;
        if (!c->bound)
            connect(c, now);
        else {
            c->generation++;
            c->state = RC_WAKING;
            c->status.valid = false;
            c->deadline_ms = now + 6000;
            action(c, RC_EFFECT_WAKE);
        }
        return true;
    }
    if (c->state != RC_READY || pressed_ms < c->ready_since_ms)
        return false;
    if (!c->status.valid || now - c->status.received_ms > STATUS_MAX_AGE_MS) {
        fail(c, RC_ERROR_STALE);
        return false;
    }
    if (long_press) {
        if (c->status.activity == 3 || (c->status.mode == 5 && c->status.countdown_seconds > 0)) {
            c->state = RC_WAIT_IDLE;
            c->operation_ms = now;
            c->acknowledged = true;
            c->deadline_ms = now + 15000;
            if (c->status.mode == 1) {
                uint8_t p[9] = {0};
                memcpy(p, c->camera_id, 4);
                p[4] = 1;
                command(c, 0x1d, 3, p, sizeof p, now);
            } else if (c->status.mode != 5) {
                fail(c, RC_ERROR_UNSUPPORTED);
                return false;
            }
        } else if (c->status.activity == 0 || c->status.activity == 1 || c->status.activity == 2 ||
                   c->status.activity == 5)
            sleep_camera(c, now);
        else
            return false;
        return true;
    }
    if (c->status.mode != 1 && c->status.mode != 5) {
        c->error = RC_ERROR_UNSUPPORTED;
        return false;
    }
    if (c->status.activity != 0 && c->status.activity != 1 && c->status.activity != 3 &&
        c->status.activity != 5)
        return false;
    if (c->status.mode == 5 && (c->status.activity == 3 || c->status.countdown_seconds > 0))
        return false;
    const uint8_t shutter[4] = {1, 1, 0, 0};
    c->state = RC_CAPTURING;
    return command(c, 0, 0x11, shutter, sizeof shutter, now);
}
void rc_tick(rc_core_t *c, uint64_t now) {
    c->clock_ms = now;
    if (c->state == RC_READY) {
        if (now - c->status.received_ms > STATUS_MAX_AGE_MS)
            fail(c, RC_ERROR_STALE);
    } else if (c->state != RC_OFFLINE && c->state != RC_ASLEEP && now >= c->deadline_ms)
        fail(c, RC_ERROR_TIMEOUT);
}
void rc_link_lost(rc_core_t *c, uint32_t generation, uint64_t now) {
    c->clock_ms = now;
    if (generation != c->generation)
        return;
    c->status.valid = false;
    if (c->state == RC_ASLEEP || c->state == RC_WAKING || c->state == RC_OFFLINE)
        return;
    if (c->state == RC_CONNECTING || c->state == RC_HANDSHAKE || c->state == RC_SYNCING)
        fail(c, RC_ERROR_IO);
    else
        connect(c, now);
}
void rc_wake_complete(rc_core_t *c, uint32_t generation, uint64_t now) {
    c->clock_ms = now;
    if (generation == c->generation && c->state == RC_WAKING)
        connect(c, now);
}
void rc_io_failed(rc_core_t *c, uint32_t generation, uint64_t now) {
    c->clock_ms = now;
    if (generation == c->generation)
        fail(c, RC_ERROR_IO);
}
