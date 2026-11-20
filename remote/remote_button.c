#include "remote_button.h"

rb_event_t rb_poll(rb_button_t *b, bool down, uint64_t now) {
    if (down != b->raw) {
        b->raw = down;
        b->changed_ms = now;
    }
    if (!b->armed) {
        if (!down && now - b->changed_ms >= 30)
            b->armed = true;
        /* 首次采样为松开时即可开始；上电按住必须先稳定松开。 */
        if (!down && now == 0)
            b->armed = true;
        return RB_NONE;
    }
    if (now - b->changed_ms >= 30 && b->stable != b->raw) {
        b->stable = b->raw;
        if (b->stable) {
            b->pressed_ms = now;
            b->long_sent = false;
        } else if (!b->long_sent) {
            if (b->changed_ms - b->pressed_ms >= 1500) {
                b->long_sent = true;
                return RB_LONG;
            }
            return RB_SHORT;
        }
    }
    if (b->stable && b->raw && !b->long_sent && now - b->pressed_ms >= 1500) {
        b->long_sent = true;
        return RB_LONG;
    }
    return RB_NONE;
}
