# 验证记录与实机检查表

验证范围：ESP32-C6 / ESP-IDF 6.1.1。2026-11-17 完成控制器初始验证，2026-11-19 迁移默认配置到 XIAO ESP32C6；不连接硬件、不烧录、不提交 Git。

## 无设备验证

运行 `./scripts/test.sh`，所有测试开启 `-Wall -Wextra -Werror`、AddressSanitizer 和 UndefinedBehaviorSanitizer。

| 需求/异常 | 权威测试 | 覆盖内容 |
| --- | --- | --- |
| 照片/视频短按及再次短按停录 | `test_capture` | 真正输出 0011 拍录键，遵循当前模式，过渡中不重复发 |
| 长按录像后休眠 | `test_sleep_after_recording` | 先 1D03 停录，ACK 后仍等待新空闲状态，再 001A；使用握手返回设备 ID |
| 只唤醒，第二次才拍摄 | `test_wake_only` | WKP 完成→新连接→握手→状态同步；旧手势不补拍 |
| 相机自行休眠与熄屏区分 | `test_wake_only` / `test_failures` | power=3 休眠，screen-off 且 power=0 仍可拍摄 |
| 长按互斥与消抖 | `test_button` | 弹跳、上电按住、长按松手、阈值附近释放消抖 |
| 拍照、倒计时与预录制 | `test_wait_photo_and_late_ack` / `test_photo_countdown` / `test_prerecord_sleep` | 忙碌不重复拍，结束才休眠；预录制可休眠，不当实际录像停录 |
| 旧状态和迟到 ACK | `test_session_and_ack_isolation` | 旧连接编号、错误序号、错误命令、状态先于 ACK 的顺序 |
| 拒绝、超时及过渡手势 | `test_pairing_and_sleep_failures` / `test_transition_gesture` / `test_failures` | 不误报休眠，不重发拍摄，不把过渡期间手势延迟执行 |
| 协议兼容性 | `test_wire` | 官方 SNAPSHOT 样例逐字节比对；大小端、长度、CRC、WKP 地址逆序 |
| 协议输入安全 | `test_wire` | 分片、粘包、坏 CRC、短帧、随机输入 3000×1024 字节；断开回调清空缓冲不会下溢 |
| 实际 BLE 适配流程 | `test_ble flow` | 扫描地址类型、MTU、特征发现、CCCD 后才握手、写队列、授权绑定、两秒广播、重新授权、仅下次按键拍摄 |
| 适配层失败恢复 | `test_ble scan_error/immediate_error/adv_config_error/adv_start_error/adv_immediate_error/close_error` | 失败完成或 API 即时拒绝后仍可恢复，不永久等待不存在的事件 |
| 取消竞态 | `test_ble cancel_race` | 取消中发起新请求，旧连接成功与取消失败交错，先关闭旧连接才能开始新请求 |
| SDK 丢失完成事件 | `test_ble lost_callback` | 传输阶段卡住 30 秒后受控重启，不产生快门 |
| 队列溢出 | `test_ble queue_overflow` | 丢失事件即撤销就绪，不继续按不完整状态发送 |

测试替身只替代 ESP-IDF 的 BLE、NVS、时钟和队列边界，不替代生产状态机或协议实现。它们不模拟射频、真实相机固件或文件系统落盘。

上游 `test/frame_check` 已由源码重新编译并执行 `--test`。该工具包含无效帧示例，且对无效帧也返回 0，因此仅记为工具冒烟检查，**不将其退出码计作“协议全部通过”**。有效帧兼容性由本项目独立断言测试证明。

## 构建

在 ESP-IDF 环境执行 `./scripts/build.sh`，应生成 `build/dji_camera_bluetooth_control.bin`，并通过应用分区大小检查。

- 生产源范围由 `main/CMakeLists.txt` 明确指定，仅 `remote/`、入口及官方 CRC。
- 依赖版本见 `dependencies.lock`；工程配置由 `sdkconfig.defaults` 在 `build/sdkconfig` 生成。
- 编译通过只证明工具链、API、链接和镜像生成，不证明真实相机连接或拍摄已成功。
- 最终镜像尺寸和 SHA-256 记录在下方“最终结果”。

## 独立审查与修复

独立审查识别并修复了：接收流被回调清空导致下溢、BLE 失败阶段卡住、取消与新连接代次混淆、预录制不能休眠、休眠过渡手势被延迟执行。
相关回归测试先复现失败，修复后运行整套测试。另补充迟到快门应答、拍照倒计时、关闭失败和按钮阈值边界测试。

## 用户实机验证

记录开发板型号、相机型号、固件版本、距离与完整串口日志。先用一台相机完成，再分别测试 Action 4 / 5 Pro / 6。

- [ ] XIAO 通过 USB-C 正常启动，USB 控制台能输出日志并接收命令；松开 BOOT 上电后按键可用。
- [ ] 首次连接显示配对码，在相机确认后日志为 `state=4 error=0`；重启开发板后只连接绑定相机。
- [ ] 相机照片模式短按：照片实际写入存储卡；连拍及倒计时遵循相机设置。
- [ ] 相机视频模式短按开始，再按停止；视频可以正常回放。
- [ ] 在相机端切换照片/视频后遥控行为随之变化。
- [ ] 空闲长按休眠；释放按钮不触发拍摄或重新唤醒。
- [ ] 录像中长按先结束并保存视频，再休眠；拍照/倒计时中长按等待完成。
- [ ] 相机自身进入休眠后，第一次短按只唤醒；日志恢复 `state=4 error=0` 后再按才拍摄。
- [ ] 唤醒、休眠和连接过程中反复按键，不发生就绪后突然拍摄。
- [ ] 仅熄屏但未休眠时仍能拍摄。
- [ ] 相机拒绝配对、无卡、卡满、超时和断线均不误报成功、不自动重复拍摄。
- [ ] 断线后恢复完整协议连接；若失败，短按恢复的那次不拍摄。
- [ ] 休眠 1 分钟和接近 30 分钟分别测试；超过官方唤醒窗口时可手动开机恢复。
- [ ] 输入 `forget` 清除绑定；更换相机后重新配对。

实机结果暂未填写，不把测试替身结果当作以上项目已完成。

## 初始验证结果（2026-11-17，DevKitC 8 MB）

- `./scripts/test.sh`：通过；11 个核心测试函数、协议样例/分片/随机输入套件、10 个 BLE 适配场景。日志：`build-host/test-results.log`。
- `./scripts/build.sh`：ESP-IDF 6.1.1 构建与 size 成功，未出现项目编译警告。日志：`build/verification-build.log`。
- Clang 静态分析：`remote_core.c`、`remote_wire.c`、`remote_ble.c` 无诊断；BLE 分析使用同一测试 SDK 边界头。
- `git diff --check`、构建/测试脚本语法检查通过。
- 应用 bin 实际长度：**858704 字节**（约 838.6 KiB）；1 MiB 应用分区剩余约 18%。
- 应用 bin SHA-256：`badb13c8b49b8ef4d4aa42688ebad8271f250cf588103e26903207f6bfca03f3`。
- 烧录地址：bootloader `0x0`、partition-table `0x8000`、application `0x10000`；以 `build/flash_args` 为准。
- 未进行实机连接、烧录、拍摄验证或 Git 提交。当前实现保存在本地 `feat/ble-camera-remote` 分支工作区。

## XIAO 配置验证结果（2026-11-19）

- 默认目标改为 XIAO ESP32C6：4 MB Flash、GPIO9 BOOT、关闭可寻址 RGB、USB Serial/JTAG 主控制台、无次控制台。
- BLE 初始化前将 GPIO3、GPIO14 配置为低电平输出，选择板载天线；支持通过 `REMOTE_XIAO_ESP32C6` 关闭此板级初始化。
- 已同步迁移本机 `build/sdkconfig`，并核对生成的 `build/config/sdkconfig.json` 与 `build/flasher_args.json`；实际烧录参数为 `--flash-size 4MB`。
- `./scripts/test.sh`：全部通过；日志 `build-host/xiao-test-results.log`。覆盖按键、控制核心、协议与 BLE 适配，不模拟天线电路或 USB 实物。
- `./scripts/build.sh`：ESP-IDF 6.1.1 编译、链接、镜像大小检查及 size 成功；日志 `build/xiao-verification-build.log`。
- 应用 bin 长度：**834128 字节**；SHA-256：`f47c741f83baf669c955b7965c8d5f5c521b04ec680b016d172b39b75e08ef73`。
- `git diff --check` 通过。未烧录、未验证实机 USB 输入、天线通信或相机拍摄，未提交 Git。
