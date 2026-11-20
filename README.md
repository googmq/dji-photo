# ESP32-C6 单键蓝牙相机遥控器

基于 [DJI Osmo-GPS-Controller-Demo](https://github.com/dji-sdk/Osmo-GPS-Controller-Demo) 的 DJI R SDK 协议，实现 Osmo Action 4 / 5 Pro / 6 的单键拍摄、休眠和唤醒。目标平台为 **ESP32-C6 + ESP-IDF 6.1.1**。

当前完成的是主机自动测试和固件编译，**尚未连接相机实测**。官方声明的型号支持不等于本项目已完成各型号兼容性验证。

## 按钮行为

| 相机状态 | 短按 | 长按约 1.5 秒 |
| --- | --- | --- |
| 普通照片模式，空闲 | 拍照，保留相机连拍/倒计时设置 | 休眠 |
| 普通视频模式，未录像 | 开始录像 | 休眠 |
| 正在录像 | 停止录像 | 先停录，收到结束状态后休眠 |
| 正在拍照或倒计时 | 忽略重复按键 | 等待结束后休眠，超时则提示失败 |
| 视频预录制 | 触发拍录键，由相机开始正式录像 | 直接请求休眠 |
| 已休眠 | **只唤醒**，就绪后再按才拍摄 | 不操作 |
| 正在连接、确认命令或唤醒 | 忽略，不缓存拍摄 | 忽略 |
| 离线/结果未知 | 未绑定则连接；已绑定则尝试广播唤醒并重连，本次不拍摄 | 不操作 |

长按释放不会产生第二次短按。相机端切换视频/照片模式后，遥控器跟随新状态，**不会主动改模式**。第一版拍摄仅开放普通视频 `0x01` 与照片 `0x05`；慢动作、延时摄影、直播等模式不触发快门。

## 硬件与环境

默认目标板为 Seeed Studio XIAO ESP32C6（4 MB Flash），直接使用板载硬件：

- BOOT 按键：GPIO9，低电平按下，启用上拉。
- 板载天线：在 BLE 初始化前将 GPIO3、GPIO14 配置为低电平输出；这两个引脚保留给射频开关。
- RGB 功能默认关闭：XIAO 的 GPIO15 用户灯是普通单色灯，不能使用当前可寻址 RGB 驱动；通过 USB 日志查看状态。
- USB-C 供电、烧录和主控制台（USB Serial/JTAG）；可输入 `forget` 清除绑定。首次测试无需外接按钮、电池、GPS 或蓝牙模块。

使用其他板卡时核对引脚与 Flash 容量。GPIO9 是启动配置引脚，正常启动时不要一直按住 BOOT。

若切回 DevKitC-1-N8，在 `idf.py menuconfig` 中将 Flash 改为 8 MB，关闭 `Camera Remote` 下的 XIAO 天线初始化，按需启用 GPIO8 RGB，并根据使用的 USB 接口选择主控制台。XIAO 引脚依据见 [官方入门文档](https://wiki.seeedstudio.com/cn/xiao_esp32c6_getting_started/)。

本工程使用 ESP-IDF `>=6.1.0,<6.2.0`，已验证 6.1.1。组件锁定文件记录 `espressif/led_strip` 3.1.1。官方 Demo 原先的 ESP-IDF 5.5 配置不能直接用于当前入口。

## 编译与主机测试

在已加载 ESP-IDF 的终端执行：

```sh
# 本机已有的环境入口；其他机器请加载自己的 ESP-IDF export.sh。
source ~/esp/activate-esp-idf.sh

./scripts/test.sh
./scripts/build.sh
```

主机测试需要 Clang 和 Make，不需要连接开发板或加载 ESP-IDF；默认启用 AddressSanitizer、UndefinedBehaviorSanitizer 及 `-Werror`。

ESP-IDF 配置从 `sdkconfig.defaults` 生成到 **`build/sdkconfig`**，不再把旧版本生成配置作为源码维护。`idf.py menuconfig` 修改同一构建目录中的配置；如换芯片/容量，请检查配置后重编。

`sdkconfig.defaults` 不会覆盖已有 `build/sdkconfig`。旧的 8 MB 构建迁移到 XIAO 时，需要在 menuconfig 中同步选择 4 MB Flash、XIAO 天线初始化、关闭 RGB，并将主控制台设为 USB Serial/JTAG（次控制台设为 None），随后重新编译。

构建产物：

- `build/dji_camera_bluetooth_control.bin`：应用固件。
- `build/dji_camera_bluetooth_control.elf`：调试符号。
- `build/bootloader/bootloader.bin`、`build/partition_table/partition-table.bin`：启动及分区镜像。
- `build/flash_args`：本次构建的完整烧录参数。

后续接好开发板，把下例端口替换为实际端口，再自行烧录：

```sh
idf.py -p /dev/cu.YOUR_PORT flash monitor
```

请通过 `idf.py flash` 烧录完整工程，不要把应用 bin 当作从地址 0 开始的整片镜像。

## 首次配对及日常操作

1. 相机装好存储卡、开启无线连接，升级到当前可用固件；首次测试时附近只开一台目标相机。
2. 启动开发板，自动扫描信号强度不低于 -80 dBm 的兼容设备，选择信号最强者。
3. 串口打印四位配对码，在相机屏幕上核对并允许连接。
4. USB 日志出现 `state=4 error=0` 代表协议连接和状态同步完成，可以短按 BOOT 拍摄。
5. 授权成功后地址及地址类型写入 NVS，之后只搜索绑定设备。失败后可短按重试，本次重试不拍摄。
6. 更换相机时，在串口输入 `forget` 并回车。仅离线、休眠或空闲时允许清除绑定并重启；拍摄和过渡期间拒绝操作。

恢复连接会重新协商 MTU、发现 FFF0 服务、启用 FFF4 的 CCCD、完成 0019 握手和 1D05 状态订阅。订阅频率为 2 Hz，超过 2 秒没有有效状态就撤销就绪，避免按旧状态误操作。

以下灯光说明仅适用于另外启用可寻址 RGB 的板卡；默认 XIAO 配置不输出这些灯光，请查看 USB 日志。

| 灯光 | 含义 |
| --- | --- |
| 绿色常亮 | 已就绪 |
| 红色闪烁 | 普通视频正在录像 |
| 蓝色闪烁 | 连接、命令确认、休眠过渡或唤醒中 |
| 暗蓝常亮 | 已确认相机休眠 |
| 红色常亮 | 离线或操作失败，查看串口错误 |

日志 `state` 的数值按 `remote/remote_core.h` 顺序：0 离线、1 连接、2 握手、3 同步、4 就绪、5 快门确认、6 等待拍摄结束、7 休眠确认、8 已休眠、9 唤醒。
`error`：0 无、1 传输/队列失败、2 超时、3 相机拒绝、4 模式不支持、5 状态过期。

## 实现结构

- `remote_core`：纯 C 异步状态机、命令序号匹配、休眠发送限制、手势时序保护。
- `remote_button`：30 ms 消抖、1500 ms 长按与短按互斥，上电按住先等待松开。
- `remote_wire`：固定内存协议帧、CRC 校验、分片/粘包、WKP 广播和厂商广播识别。
- `remote_ble`：Bluedroid 适配，回调仅投递事件，控制任务拥有连接和发送状态；保存绑定到 NVS。
- `remote_app`：启动、GPIO、LED、串口和主循环。
- `tests/host`：直接运行生产控制核心、协议与 BLE 适配代码，只用测试替身替换 ESP-IDF 硬件边界。

原始官方 `ble/`、`logic/`、`data/`、`protocol/` 和 GPS 示例保留供参考，**不编入当前固件**。生产代码复用官方 `utils/crc/` 实现，并用官方已知帧验证协议字节兼容性。上游版本：`92fe23e5a749f189593f980a26a105c3bb66aa1c`；原始说明见 [README_CN.md](README_CN.md)。

## 已知限制

- 官方广播唤醒要求近期成功连接过，且相机休眠不超过约 **30 分钟**；不保证任意关机时长可唤醒。
- 模式、相机休眠后 BLE 是否保持、唤醒断线时序及各型号固件差异仍需实机验证。
- 相机休眠后不发送 GATT 数据；唤醒只发 WKP 广播，不发送 SNAPSHOT 快照键。
- 若 SDK 的异步完成事件丢失、传输阶段卡住超过 30 秒，遥控器会自动重启恢复；不会补发快门。
- 超时不会自动重发快门。失败并不证明相机没有执行，先检查相机实际状态，再重试。
- 第一次配对需要相机处于可发现状态；没有绑定地址时无法定向唤醒一台已经休眠的相机。
- 绑定使用已授权地址和地址类型；若相机清除记录或改变地址，需要重新配对。
- 相机照片仍保存在相机存储卡中；不传输照片、不控制曝光参数、不实现 ESP32 自身低功耗休眠。

完整验证记录与实机检查表见 [docs/VALIDATION.md](docs/VALIDATION.md)。
