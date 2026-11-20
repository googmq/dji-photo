#ifndef REMOTE_WIRE_H
#define REMOTE_WIRE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define RW_MAX_FRAME 1023
/** 已验证协议帧的借用视图，有效期与输入缓冲一致。 */
typedef struct {
    uint8_t type, set, id;
    uint16_t seq;
    const uint8_t *payload;
    size_t length;
} rw_frame_t;
/** 分片接收缓冲，同一连接独占，断线时清零。 */
typedef struct {
    uint8_t bytes[RW_MAX_FRAME];
    size_t used;
} rw_stream_t;
/** @brief 消费一个完整帧。@param context 调用方上下文。@param bytes 帧数据。@param length
 * 长度。@return 无；数据只在回调期间有效，不抛异常。 */
typedef void (*rw_frame_fn)(void *context, const uint8_t *bytes, size_t length);
/** @brief 构造 DJI R SDK 帧。@param out 输出缓冲。@param capacity 容量。@param type 帧类型。@param
 * set 命令集。@param id 命令。@param seq 序号。@param payload 载荷，空载荷可为 NULL。@param length
 * 载荷长度。@return 帧长度，参数非法返回 0，无异常。 */
size_t rw_encode(uint8_t *out, size_t capacity, uint8_t type, uint8_t set, uint8_t id, uint16_t seq,
                 const uint8_t *payload, size_t length);
/** @brief 验证长度、版本及两级 CRC。@param bytes 输入。@param length 长度。@param out
 * 输出视图。@return 成功 true，畸形帧 false，无异常。 */
bool rw_decode(const uint8_t *bytes, size_t length, rw_frame_t *out);
/** @brief 拼接分片并拆分粘包。@param stream 当前连接缓冲。@param bytes 数据。@param length
 * 长度。@param fn 完整帧回调。@param context 回调参数。@return 无；跳过坏帧与无关协议，不抛异常。
 */
void rw_feed(rw_stream_t *stream, const uint8_t *bytes, size_t length, rw_frame_fn fn,
             void *context);
/** @brief 构造唤醒广播。@param out 至少 11 字节。@param mac 目标地址六字节。@return
 * 无；参数必须有效，不抛异常。 */
void rw_wake_advertisement(uint8_t out[11], const uint8_t mac[6]);
/** @brief 检测 DJI 厂商广播。@param ad 单个广播或响应数据。@param length 长度。@return 匹配
 * true，畸形或不匹配 false，无异常。 */
bool rw_is_camera_advertisement(const uint8_t *ad, size_t length);
#endif
