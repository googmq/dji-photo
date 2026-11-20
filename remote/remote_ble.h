#ifndef REMOTE_BLE_H
#define REMOTE_BLE_H
#include "esp_err.h"
#include "remote_core.h"
/** @brief 初始化 BLE/NVS 及事件队列。@param bound 输出是否有绑定。@param mac 输出本机地址。@return
 * ESP_OK 或初始化错误；不自动擦除 NVS。 */
esp_err_t remote_ble_init(bool *bound, uint8_t mac[6]);
/** @brief 核心动作的平台实现。@param context 未使用。@param effect 动作。@return
 * 是否受理，异步失败由 poll 上报；不能重入核心。 */
bool remote_ble_effect(void *context, const rc_effect_t *effect);
/** @brief 处理已入队的 BLE 事件。@param core 核心。@param now 单调毫秒。@return
 * 无；错误使核心离线，最多处理固定数量事件。 */
void remote_ble_poll(rc_core_t *core, uint64_t now);
/** @brief 清除本工程绑定记录。@param 无。@return ESP_OK 或 NVS 错误；调用方随后重启。 */
esp_err_t remote_ble_forget(void);
#endif
