#include "remote_app.h"

/**
 * @brief 启动 ESP32-C6 单键相机遥控器。
 * @param 无。
 * @return 正常不返回；初始化错误由运行层输出诊断并交 ESP-IDF 处理。
 */
void app_main(void) {
    remote_app_run();
}
