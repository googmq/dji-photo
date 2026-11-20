#ifndef REMOTE_APP_H
#define REMOTE_APP_H
/** @brief 启动单键遥控器主循环。@param 无。@return 正常不返回；初始化错误由 ESP_ERROR_CHECK
 * 输出并重启。 */
void remote_app_run(void);
#endif
