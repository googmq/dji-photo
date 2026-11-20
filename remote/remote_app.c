#include "remote_app.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "remote_ble.h"
#include "remote_button.h"
#include "remote_core.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define TAG "remote"
static atomic_bool forget_requested;
static led_strip_handle_t led;

/** @brief 读取串口调试命令。@param arg 未使用。@return 任务不返回；读不到字符时让出
 * CPU，不占用控制任务。 */
static void console_task(void *arg) {
    (void)arg;
    char line[32];
    size_t used = 0;
    for (;;) {
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (ch == '\n' || ch == '\r') {
            line[used] = 0;
            if (strcmp(line, "forget") == 0)
                atomic_store(&forget_requested, true);
            used = 0;
        } else if (used < sizeof line - 1)
            line[used++] = (char)ch;
        else
            used = 0;
    }
}
/**
 * @brief 初始化 XIAO 板载天线、低电平按键和可选的可寻址 RGB 灯。
 * @param 无。
 * @return 无。
 * @note 不抛出异常；GPIO 或灯初始化失败时由 ESP_ERROR_CHECK 输出错误并终止运行。
 *       天线必须在 BLE 初始化前选定；XIAO 的普通用户灯不能按可寻址 RGB 驱动。
 */
static void init_hardware(void) {
#if CONFIG_REMOTE_XIAO_ESP32C6
    /* GPIO3 低电平使能射频开关，GPIO14 低电平选择板载天线。
     * 先预置输出锁存值，再打开输出，避免初始化时短暂选择外接天线。 */
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_3, 0));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_14, 0));
    gpio_config_t rf_cfg = {.pin_bit_mask = (1ULL << GPIO_NUM_3) | (1ULL << GPIO_NUM_14),
                            .mode = GPIO_MODE_OUTPUT,
                            .pull_up_en = GPIO_PULLUP_DISABLE,
                            .pull_down_en = GPIO_PULLDOWN_DISABLE,
                            .intr_type = GPIO_INTR_DISABLE};
    ESP_ERROR_CHECK(gpio_config(&rf_cfg));
#endif
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << CONFIG_REMOTE_BUTTON_GPIO,
                         .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE,
                         .pull_down_en = GPIO_PULLDOWN_DISABLE,
                         .intr_type = GPIO_INTR_DISABLE};
    ESP_ERROR_CHECK(gpio_config(&cfg));
#if CONFIG_REMOTE_LED_ENABLED
    led_strip_config_t strip = {.strip_gpio_num = CONFIG_REMOTE_LED_GPIO, .max_leds = 1};
    led_strip_rmt_config_t rmt = {.resolution_hz = 10000000, .flags.with_dma = false};
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip, &rmt, &led));
#endif
}
/** @brief 更新状态指示灯。@param c 核心只读快照。@param now 毫秒。@return
 * 无；灯故障打印日志，不影响相机命令。 */
static void update_led(const rc_core_t *c, uint64_t now) {
    if (!led)
        return;
    uint8_t r = 0, g = 0, b = 0;
    if (c->state == RC_READY) {
        if (c->status.mode == 1 && c->status.activity == 3)
            r = (now / 500) % 2 ? 12 : 0;
        else
            g = 10;
    } else if (c->state == RC_ASLEEP)
        b = 2;
    else if (c->state == RC_OFFLINE)
        r = 10;
    else
        b = (now / 250) % 2 ? 12 : 0;
    static uint32_t previous = UINT32_MAX;
    uint32_t color = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
    if (color == previous)
        return;
    esp_err_t err = led_strip_set_pixel(led, 0, r, g, b);
    if (err == ESP_OK)
        err = led_strip_refresh(led);
    if (err == ESP_OK)
        previous = color;
    else
        ESP_LOGW(TAG, "LED error: %s", esp_err_to_name(err));
}
void remote_app_run(void) {
    init_hardware();
    bool bound;
    uint8_t mac[6];
    ESP_ERROR_CHECK(remote_ble_init(&bound, mac));
    rc_core_t core;
    uint16_t code = (uint16_t)(esp_random() % 10000);
    rc_init(&core, mac, bound, code, remote_ble_effect, NULL);
    ESP_LOGI(TAG, "Pairing code: %04u; bound=%d. Short: shutter/wake only. Long: sleep.", code,
             bound);
    ESP_LOGI(TAG,
             "Type 'forget' then Enter to clear binding while idle; physical tests are required.");
    if (xTaskCreate(console_task, "remote_console", 3072, NULL, 1, NULL) != pdPASS)
        ESP_LOGW(TAG, "Console task unavailable");
    rc_start(&core, (uint64_t)esp_timer_get_time() / 1000);
    rb_button_t button = {0};
    int old_state = -1, old_error = -1, old_mode = -1, old_activity = -1;
    for (;;) {
        remote_ble_poll(&core, (uint64_t)esp_timer_get_time() / 1000);
        uint64_t now = (uint64_t)esp_timer_get_time() / 1000;
        rc_tick(&core, now);
        rb_event_t event = rb_poll(&button, gpio_get_level(CONFIG_REMOTE_BUTTON_GPIO) == 0, now);
        if (event != RB_NONE && !rc_button(&core, event == RB_LONG, button.pressed_ms, now))
            ESP_LOGI(TAG, "Button ignored: busy, old gesture, unsupported mode or stale status");
        if (atomic_exchange(&forget_requested, false)) {
            if (core.state == RC_OFFLINE || core.state == RC_ASLEEP ||
                (core.state == RC_READY && core.status.activity != 3 &&
                 core.status.activity != 5)) {
                esp_err_t err = remote_ble_forget();
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Binding cleared; restarting");
                    esp_restart();
                } else
                    ESP_LOGE(TAG, "Cannot clear binding: %s", esp_err_to_name(err));
            } else
                ESP_LOGW(TAG, "Cannot forget while busy/recording");
        }
        if (old_state != (int)core.state || old_error != (int)core.error ||
            old_mode != core.status.mode || old_activity != core.status.activity) {
            ESP_LOGI(TAG, "state=%d error=%d mode=0x%02x activity=0x%02x power=%u session=%lu",
                     core.state, core.error, core.status.mode, core.status.activity,
                     core.status.power, (unsigned long)core.generation);
            old_state = core.state;
            old_error = core.error;
            old_mode = core.status.mode;
            old_activity = core.status.activity;
        }
        update_led(&core, now);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
