#include "custom_crc16.h"
#include "custom_crc32.h"
#include "remote_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int frames;
/** @brief 验证分片后的完整帧。@param ctx 未使用。@param data 帧。@param n 长度。@return
 * 无；非法帧断言失败。 */
static void got_frame(void *ctx, const uint8_t *data, size_t n) {
    (void)ctx;
    rw_frame_t f;
    assert(rw_decode(data, n, &f));
    frames++;
}
/** @brief 模拟相机拒绝握手导致适配层清空连接缓冲。@param ctx 当前流。@param bytes 帧。@param length
 * 长度。@return 无；断开连接允许同步清空流。 */
static void reset_stream(void *ctx, const uint8_t *bytes, size_t length) {
    rw_frame_t frame;
    assert(rw_decode(bytes, length, &frame));
    ((rw_stream_t *)ctx)->used = 0;
}
/** @brief 运行真实 CRC、官方样例及异常帧测试。@param 无。@return 成功 0；失败断言。 */
int main(void) {
    /* 官方 key_logic.c 中的 SNAPSHOT 样例，独立于本项目编码器。 */
    const uint8_t golden[] = {0xaa, 0x16, 0,    1, 0, 0, 0, 0,    0,    0x12, 0x8c,
                              0x23, 0,    0x11, 3, 1, 0, 0, 0x26, 0x3b, 0x43, 0xcc};
    rw_stream_t reset_buffer = {0};
    rw_feed(&reset_buffer, golden, sizeof golden, reset_stream, &reset_buffer);
    assert(reset_buffer.used == 0);
    rw_frame_t f;
    assert(rw_decode(golden, sizeof golden, &f));
    assert(f.seq == 0x1200 && f.id == 0x11 && f.length == 4);
    uint8_t out[1024];
    size_t n = rw_encode(out, sizeof out, 1, 0, 0x11, 0x1200, (const uint8_t[]){3, 1, 0, 0}, 4);
    assert(n == sizeof golden && memcmp(out, golden, n) == 0);
    rw_stream_t stream = {0};
    frames = 0;
    rw_feed(&stream, golden, 3, got_frame, NULL);
    assert(frames == 0);
    rw_feed(&stream, golden + 3, sizeof golden - 3, got_frame, NULL);
    assert(frames == 1);
    uint8_t joined[44];
    memcpy(joined, golden, 22);
    memcpy(joined + 22, golden, 22);
    rw_feed(&stream, joined, 44, got_frame, NULL);
    assert(frames == 3);
    out[15] ^= 1;
    assert(!rw_decode(out, n, &f));
    rw_feed(&stream, out, n, got_frame, NULL);
    rw_feed(&stream, golden, 22, got_frame, NULL);
    assert(frames == 4);
    for (size_t i = 0; i < sizeof golden; i++)
        assert(!rw_decode(golden, i, &f));
    assert(!rw_encode(out, 20, 0, 0, 0, 0, golden, 22));
    assert(!rw_encode(out, sizeof out, 0, 0, 0, 0, NULL, 1));
    const uint8_t junk[] = {0x55, 0xff, 0xaa, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0};
    rw_feed(&stream, junk, sizeof junk, got_frame, NULL);
    rw_feed(&stream, golden, 22, got_frame, NULL);
    assert(frames == 5);
    const uint8_t mac[] = {1, 2, 3, 4, 5, 6};
    rw_wake_advertisement(out, mac);
    assert(memcmp(out, (uint8_t[]){10, 255, 'W', 'K', 'P', 6, 5, 4, 3, 2, 1}, 11) == 0);
    assert(rw_is_camera_advertisement((uint8_t[]){6, 255, 0xaa, 8, 0, 0, 0xfa}, 7));
    assert(!rw_is_camera_advertisement((uint8_t[]){6, 255, 0xaa, 8, 0, 0}, 6));
    /* 确定性随机输入配合 ASan/UBSan 验证边界处理与有界缓冲。 */
    uint32_t seed = 7;
    for (unsigned j = 0; j < 3000; j++) {
        for (unsigned i = 0; i < sizeof out; i++) {
            seed = seed * 1664525 + 1013904223;
            out[i] = (uint8_t)(seed >> 24);
        }
        rw_feed(&stream, out, sizeof out, got_frame, NULL);
        rw_decode(out, j % 1024, &f);
        rw_is_camera_advertisement(out, j % 1024);
    }
    puts("wire golden/fragmentation/malformed/fuzz tests passed");
    return 0;
}
