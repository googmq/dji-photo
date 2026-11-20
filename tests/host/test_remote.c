#include "remote_button.h"
#include "remote_core.h"
#include "remote_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static rc_core_t core;
static rc_effect_t effects[128];
static size_t count;
/** @brief 捕获实际核心输出。@param ctx 未使用。@param e 动作。@return true；数组满则断言失败。 */
static bool capture(void *ctx, const rc_effect_t *e) {
    (void)ctx;
    assert(count < 128);
    effects[count++] = *e;
    return true;
}
/** @brief 重置测试夹具。@param 无。@return 无；初始化真实核心，不模拟状态机。 */
static void reset(void) {
    const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    count = 0;
    rc_init(&core, mac, false, 1234, capture, NULL);
}
/** @brief 向真实核心注入相机协议帧。@param type 帧类型。@param set 命令集。@param id 命令。@param
 * seq 序号。@param p 载荷。@param n 长度。@param now 时间。@return 无；编码失败断言。 */
static void receive(uint8_t type, uint8_t set, uint8_t id, uint16_t seq, const uint8_t *p, size_t n,
                    uint64_t now) {
    uint8_t frame[128];
    size_t len = rw_encode(frame, sizeof frame, type, set, id, seq, p, n);
    assert(len);
    rc_receive(&core, core.generation, frame, len, now);
}
/** @brief 注入完整状态通知。@param mode 模式。@param activity 拍摄状态。@param power 电源。@param
 * now 时间。@return 无。 */
static void status(uint8_t mode, uint8_t activity, uint8_t power, uint64_t now) {
    uint8_t p[38] = {0};
    p[0] = mode;
    p[1] = activity;
    p[28] = power;
    receive(0, 0x1d, 2, 80, p, sizeof p, now);
}
/** @brief 模拟授权及首个状态。@param mode 初始模式。@return 无；必须经历真正握手。 */
static void ready(uint8_t mode) {
    reset();
    rc_start(&core, 0);
    assert(effects[0].kind == RC_EFFECT_CONNECT);
    rc_link_ready(&core, core.generation, 100);
    rw_frame_t f;
    assert(rw_decode(effects[count - 1].data, effects[count - 1].length, &f));
    assert(f.set == 0 && f.id == 0x19 && f.length == 33 && f.payload[26] == 1);
    uint8_t p[33] = {0x44, 0xff, 0, 0};
    p[26] = 2;
    receive(2, 0, 0x19, 99, p, sizeof p, 200);
    assert(core.state == RC_SYNCING);
    status(mode, 1, 0, 300);
    assert(core.state == RC_READY);
    count = 0;
}
/** @brief 断言最后发送的命令。@param set 集。@param id 命令。@return 已解码视图；不符则断言失败。
 */
static rw_frame_t last_command(uint8_t set, uint8_t id) {
    rw_frame_t f;
    assert(count);
    assert(effects[count - 1].kind == RC_EFFECT_SEND);
    assert(rw_decode(effects[count - 1].data, effects[count - 1].length, &f));
    assert(f.set == set && f.id == id);
    return f;
}
/** @brief 确认当前在途命令。@param code 应答返回码。@param now 时间。@return 无。 */
static void ack(uint8_t code, uint64_t now) {
    uint8_t p[5] = {code};
    receive(0x20, core.pending_set, core.pending_id, core.pending_seq, p, sizeof p, now);
}
/** @brief 验证模式跟随与快门切换。@param 无。@return 无；失败断言。 */
static void test_capture(void) {
    ready(5);
    assert(rc_button(&core, false, 400, 450));
    rw_frame_t f = last_command(0, 0x11);
    assert(f.length == 4 && memcmp(f.payload, "\x01\x01\x00\x00", 4) == 0);
    assert(!rc_button(&core, false, 460, 500));
    ack(0, 510);
    status(5, 1, 0, 600);
    assert(core.state == RC_READY);
    ready(1);
    assert(rc_button(&core, false, 400, 450));
    last_command(0, 0x11);
    ack(0, 500);
    status(1, 3, 0, 600);
    assert(core.state == RC_READY);
    assert(rc_button(&core, false, 700, 750));
    last_command(0, 0x11);
}
/** @brief 验证停录得到新空闲状态后才发送休眠。@param 无。@return 无；失败断言。 */
static void test_sleep_after_recording(void) {
    ready(1);
    status(1, 3, 0, 400);
    assert(rc_button(&core, true, 500, 2000));
    rw_frame_t f = last_command(0x1d, 3);
    assert(f.payload[4] == 1);
    assert(memcmp(f.payload, "\x44\xff\0\0", 4) == 0);
    size_t n = count;
    ack(0, 2100);
    assert(count == n);
    status(1, 1, 0, 2200);
    f = last_command(0, 0x1a);
    assert(f.length == 1 && f.payload[0] == 3);
    ack(0, 2300);
    assert(core.state == RC_ASLEEP);
    n = count;
    status(1, 1, 0, 2400);
    rc_tick(&core, 9000);
    assert(core.state == RC_ASLEEP && count == n);
}
/** @brief 验证唤醒不补拍且旧手势不能延迟拍摄。@param 无。@return 无；失败断言。 */
static void test_wake_only(void) {
    ready(5);
    status(5, 0, 3, 400);
    assert(core.state == RC_ASLEEP);
    assert(rc_button(&core, false, 500, 550));
    assert(effects[count - 1].kind == RC_EFFECT_WAKE);
    assert(!rc_button(&core, false, 560, 600));
    uint32_t old = core.generation;
    rc_wake_complete(&core, old, 2600);
    assert(effects[count - 1].kind == RC_EFFECT_CONNECT);
    rc_link_ready(&core, core.generation, 2800);
    uint8_t p[33] = {0x44, 0xff};
    p[26] = 2;
    receive(2, 0, 0x19, 101, p, sizeof p, 2900);
    status(5, 1, 0, 3000);
    assert(core.state == RC_READY);
    count = 0;
    assert(!rc_button(&core, false, 2950, 3100));
    assert(count == 0);
    assert(rc_button(&core, false, 3200, 3300));
    last_command(0, 0x11);
}
/** @brief 验证坏状态、拒绝和超时不会重复拍摄。@param 无。@return 无；失败断言。 */
static void test_failures(void) {
    ready(5);
    assert(!rc_button(&core, false, 4000, 4100));
    assert(core.state != RC_READY);
    ready(5);
    assert(rc_button(&core, false, 400, 450));
    size_t n = count;
    rc_tick(&core, 6000);
    assert(core.state == RC_OFFLINE);
    assert(count == n + 1 && effects[count - 1].kind == RC_EFFECT_DISCONNECT);
    ready(5);
    rc_button(&core, false, 400, 450);
    ack(2, 500);
    assert(core.error == RC_ERROR_REJECTED && core.state != RC_READY);
    ready(5);
    status(5, 0, 0, 400);
    assert(rc_button(&core, false, 500, 550));
    last_command(0, 0x11);
    ready(0x28);
    assert(!rc_button(&core, false, 400, 450));
    assert(count == 0);
}
/** @brief 验证按键消抖、长按互斥和上电按住。@param 无。@return 无；失败断言。 */
static void test_button(void) {
    rb_button_t b = {0};
    assert(rb_poll(&b, false, 0) == RB_NONE);
    assert(rb_poll(&b, true, 100) == RB_NONE);
    assert(rb_poll(&b, false, 110) == RB_NONE);
    assert(rb_poll(&b, true, 120) == RB_NONE);
    assert(rb_poll(&b, true, 150) == RB_NONE);
    assert(rb_poll(&b, true, 1650) == RB_LONG);
    assert(rb_poll(&b, true, 1700) == RB_NONE);
    assert(rb_poll(&b, false, 1800) == RB_NONE);
    assert(rb_poll(&b, false, 1830) == RB_NONE);
    assert(rb_poll(&b, true, 2000) == RB_NONE);
    assert(rb_poll(&b, true, 2030) == RB_NONE);
    assert(rb_poll(&b, false, 2100) == RB_NONE);
    assert(rb_poll(&b, false, 2130) == RB_SHORT);
    memset(&b, 0, sizeof b);
    rb_poll(&b, false, 0);
    rb_poll(&b, true, 100);
    rb_poll(&b, true, 130);
    /* 松开去抖不能把实际不足 1.5 秒的按住时间延长为长按。 */
    assert(rb_poll(&b, false, 1620) == RB_NONE);
    assert(rb_poll(&b, false, 1640) == RB_NONE);
    assert(rb_poll(&b, false, 1650) == RB_SHORT);
    memset(&b, 0, sizeof b);
    rb_poll(&b, true, 0);
    assert(rb_poll(&b, true, 3000) == RB_NONE);
}
/** @brief 验证旧连接、错误序号和错误命令应答不能改变当前操作。@param 无。@return 无；失败断言。 */
static void test_session_and_ack_isolation(void) {
    ready(5);
    uint32_t old = core.generation;
    rc_link_lost(&core, old, 400);
    assert(core.state == RC_CONNECTING);
    uint8_t p[38] = {5, 1};
    uint8_t frame[80];
    size_t n = rw_encode(frame, sizeof frame, 0, 0x1d, 2, 88, p, sizeof p);
    rc_receive(&core, old, frame, n, 500);
    assert(core.state == RC_CONNECTING && !core.status.valid);
    rc_link_ready(&core, old, 600);
    assert(core.state == RC_CONNECTING);
    ready(5);
    rc_button(&core, false, 400, 450);
    uint8_t ok[5] = {0};
    receive(0x20, 0, 0x11, core.pending_seq + 1, ok, 5, 500);
    status(5, 1, 0, 550);
    assert(core.state == RC_CAPTURING);
    receive(0x20, 0, 0x1a, core.pending_seq, ok, 5, 560);
    assert(core.state == RC_CAPTURING);
    ack(0, 600);
    assert(core.state == RC_READY);
}
/** @brief 验证相机拒绝配对、过期和睡眠应答失败不误报成功。@param 无。@return 无；失败断言。 */
static void test_pairing_and_sleep_failures(void) {
    reset();
    rc_start(&core, 0);
    rc_link_ready(&core, core.generation, 100);
    uint8_t p[33] = {0x44, 0xff};
    p[26] = 2;
    p[27] = 1;
    receive(2, 0, 0x19, 99, p, sizeof p, 200);
    assert(core.state == RC_OFFLINE && !core.bound && core.error == RC_ERROR_REJECTED);
    ready(5);
    assert(rc_button(&core, true, 400, 1900));
    last_command(0, 0x1a);
    ack(2, 2000);
    assert(core.state == RC_OFFLINE && core.error == RC_ERROR_REJECTED);
    ready(5);
    assert(rc_button(&core, true, 400, 1900));
    size_t n = count;
    status(5, 1, 0, 1950);
    assert(core.state == RC_SLEEP_PENDING);
    rc_tick(&core, 7000);
    assert(core.state == RC_OFFLINE && count == n + 1);
    ready(5);
    status(5, 0, 3, 400);
    rc_button(&core, false, 500, 550);
    n = count;
    rc_tick(&core, 7000);
    assert(core.state == RC_OFFLINE && count == n + 1);
}
/** @brief 拍照中长按等待结束，不能受前一次快门的迟到应答干扰。@param 无。@return 无；失败断言。 */
static void test_wait_photo_and_late_ack(void) {
    ready(5);
    rc_button(&core, false, 400, 450);
    ack(0, 500);
    status(5, 3, 0, 550);
    assert(core.state == RC_READY);
    assert(!rc_button(&core, false, 600, 650));
    status(5, 3, 0, 1700);
    assert(rc_button(&core, true, 700, 2200));
    assert(core.state == RC_WAIT_IDLE);
    /* 前一次快门的迟到重复应答不属于这次等待拍照结束操作。 */
    ack(2, 2300);
    assert(core.state == RC_WAIT_IDLE);
    status(5, 1, 0, 2400);
    last_command(0, 0x1a);
}
/** @brief 预录制不是实际录像，长按直接请求休眠。@param 无。@return 无；失败断言。 */
static void test_prerecord_sleep(void) {
    ready(1);
    status(1, 5, 0, 1500);
    assert(rc_button(&core, true, 400, 1900));
    last_command(0, 0x1a);
}
/** @brief 过渡期间按下不能在休眠确认或连接失败后变成新操作。@param 无。@return 无；失败断言。 */
static void test_transition_gesture(void) {
    ready(5);
    rc_button(&core, true, 400, 1900);
    ack(0, 2000);
    size_t n = count;
    assert(!rc_button(&core, false, 1950, 2050));
    assert(count == n);
    assert(rc_button(&core, false, 2100, 2150));
    reset();
    rc_start(&core, 0);
    rc_tick(&core, 21000);
    n = count;
    assert(!rc_button(&core, false, 20000, 22000));
    assert(count == n);
}
/** @brief 拍照倒计时尚未结束时既不能重复拍照，也不能提前休眠。@param 无。@return 无；失败断言。 */
static void test_photo_countdown(void) {
    ready(5);
    uint8_t p[38] = {5, 1};
    p[9] = 3;
    receive(0, 0x1d, 2, 80, p, sizeof p, 400);
    assert(!rc_button(&core, false, 500, 550));
    assert(count == 0);
    assert(rc_button(&core, true, 500, 2000));
    assert(core.state == RC_WAIT_IDLE && count == 0);
    p[9] = 1;
    receive(0, 0x1d, 2, 81, p, sizeof p, 2100);
    assert(core.state == RC_WAIT_IDLE && count == 0);
    status(5, 1, 0, 2500);
    last_command(0, 0x1a);
}
/** @brief 执行行为测试。@param 无。@return 成功 0，断言失败终止。 */
int main(void) {
    test_photo_countdown();
    test_prerecord_sleep();
    test_transition_gesture();
    test_wait_photo_and_late_ack();
    test_session_and_ack_isolation();
    test_pairing_and_sleep_failures();
    test_button();
    test_capture();
    test_sleep_after_recording();
    test_wake_only();
    test_failures();
    puts("remote behavioral tests passed");
    return 0;
}
