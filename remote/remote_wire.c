#include "remote_wire.h"
#include "custom_crc16.h"
#include "custom_crc32.h"
#include <string.h>

/** @brief 读取小端整数。@param p 至少四字节。@return 数值；无异常。 */
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/** @brief 写入小端整数。@param p 四字节输出。@param n 数值。@return 无；无异常。 */
static void write32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(n >> (i * 8));
}
size_t rw_encode(uint8_t *out, size_t capacity, uint8_t type, uint8_t set, uint8_t id, uint16_t seq,
                 const uint8_t *payload, size_t length) {
    if (!out || (length && !payload) || length > RW_MAX_FRAME - 18 || capacity < length + 18)
        return 0;
    size_t n = length + 18;
    memset(out, 0, 14);
    out[0] = 0xaa;
    out[1] = (uint8_t)n;
    out[2] = (uint8_t)(n >> 8);
    out[3] = type;
    out[8] = (uint8_t)seq;
    out[9] = (uint8_t)(seq >> 8);
    uint16_t crc = calculate_crc16(out, 10);
    out[10] = (uint8_t)crc;
    out[11] = (uint8_t)(crc >> 8);
    out[12] = set;
    out[13] = id;
    if (length)
        memcpy(out + 14, payload, length);
    write32(out + n - 4, calculate_crc32(out, n - 4));
    return n;
}
bool rw_decode(const uint8_t *bytes, size_t length, rw_frame_t *out) {
    if (!bytes || !out || length < 18 || length > RW_MAX_FRAME || bytes[0] != 0xaa ||
        bytes[4] != 0 || (bytes[2] & 0xfc))
        return false;
    if (((size_t)bytes[1] | (size_t)bytes[2] << 8) != length ||
        calculate_crc16(bytes, 10) != ((uint16_t)bytes[10] | (uint16_t)bytes[11] << 8) ||
        calculate_crc32(bytes, length - 4) != read32(bytes + length - 4))
        return false;
    *out = (rw_frame_t){.type = bytes[3],
                        .set = bytes[12],
                        .id = bytes[13],
                        .seq = (uint16_t)(bytes[8] | bytes[9] << 8),
                        .payload = bytes + 14,
                        .length = length - 18};
    return true;
}
void rw_feed(rw_stream_t *s, const uint8_t *bytes, size_t length, rw_frame_fn fn, void *context) {
    if (!s || !bytes || !fn)
        return;
    for (size_t i = 0; i < length; i++) {
        if (s->used == sizeof s->bytes)
            s->used = 0;
        s->bytes[s->used++] = bytes[i];
        while (s->used) {
            if (s->bytes[0] != 0xaa) {
                memmove(s->bytes, s->bytes + 1, --s->used);
                continue;
            }
            if (s->used < 12)
                break;
            size_t n = (size_t)s->bytes[1] | (size_t)s->bytes[2] << 8;
            if (n < 18 || n > RW_MAX_FRAME ||
                calculate_crc16(s->bytes, 10) !=
                    ((uint16_t)s->bytes[10] | (uint16_t)s->bytes[11] << 8)) {
                memmove(s->bytes, s->bytes + 1, --s->used);
                continue;
            }
            if (s->used < n)
                break;
            rw_frame_t f;
            if (rw_decode(s->bytes, n, &f)) {
                /* 回调可能断开连接并清空 stream，先消费再交付独立副本。 */
                uint8_t complete[RW_MAX_FRAME];
                memcpy(complete, s->bytes, n);
                s->used -= n;
                memmove(s->bytes, s->bytes + n, s->used);
                fn(context, complete, n);
            } else {
                memmove(s->bytes, s->bytes + 1, --s->used);
            }
        }
    }
}
void rw_wake_advertisement(uint8_t out[11], const uint8_t mac[6]) {
    memcpy(out,
           "\x0a\xff"
           "WKP",
           5);
    for (unsigned i = 0; i < 6; i++)
        out[5 + i] = mac[5 - i];
}
bool rw_is_camera_advertisement(const uint8_t *ad, size_t length) {
    if (!ad)
        return false;
    for (size_t i = 0; i < length;) {
        size_t n = ad[i];
        if (n == 0 || n > length - i - 1)
            return false;
        if (n >= 6 && ad[i + 1] == 0xff && ad[i + 2] == 0xaa && ad[i + 3] == 8 && ad[i + 6] == 0xfa)
            return true;
        i += n + 1;
    }
    return false;
}
