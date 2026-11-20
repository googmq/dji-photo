#ifndef REMOTE_BUTTON_H
#define REMOTE_BUTTON_H
#include <stdbool.h>
#include <stdint.h>
/** 手势分类，长按与松手短按互斥。 */
typedef enum {
    RB_NONE,
    /**< 无新手势。 */ RB_SHORT,
    /**< 松手短按。 */ RB_LONG /**< 达到阈值的长按。 */
} rb_event_t;
/** 去抖状态；上电按住时先等待松手，避免启动误操作。 */
typedef struct {
    bool raw, stable, armed, long_sent;
    uint64_t changed_ms, pressed_ms;
} rb_button_t;
/** @brief 扫描按钮。@param b 零初始化状态。@param down 当前是否低电平按下。@param now
 * 单调毫秒。@return 单次手势事件；无异常。 */
rb_event_t rb_poll(rb_button_t *b, bool down, uint64_t now);
#endif
