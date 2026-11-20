# Camera Remote Implementation Plan

> 执行方式：使用 superpowers:executing-plans 在当前会话执行；用户已授权实现与无设备验证。按用户约定不提交。

**Goal:** 实现已确认的单键拍摄、停录、休眠和二次按键拍摄的唤醒流程。

**Architecture:** 纯 C 状态机输出传输动作，ESP-IDF 适配执行 BLE 扫描、连接、广播、发送和 NVS。按键与协议独立测试。

**Tech Stack:** ESP32-C6、ESP-IDF 6.1.1、Bluedroid、FreeRTOS、C11、Clang 主机测试。

**Spec:** ../specs/2026-11-17-camera-remote-design.md

## Global Constraints

- 不提交，不使用实际设备；新增/修改函数、结构和枚举以中文说明参数、返回和失败处理。
- 默认 BOOT GPIO9，低电平按下；RGB GPIO8。1500 ms 长按、30 ms 消抖。
- SDK 来源 dji-sdk/Osmo-GPS-Controller-Demo @ 92fe23e。
- 不更改用户已安装的 ESP-IDF；工程配置适配 6.1.1。

## Review Focus

- 唤醒期间按键不得延迟变为拍摄，旧连接通知必须隔离。
- 休眠应答与末尾状态通知竞态不得重新开放发送。
- 录像停止应答并不代表已停止，需等待新的空闲状态。
- 超时、队列满、拒绝配对和异常帧不得误报就绪或重复发快门。
- BLE CCCD 写成功后才开始握手；广播配置和开始完成后才计 2 秒。

### Task 1: 核心与协议

Files: remote/remote_core.[ch], remote/remote_wire.[ch], remote/remote_button.[ch], tests/host/*。
Interfaces: rc_init/rc_start/rc_link_ready/rc_receive/rc_button/rc_tick/rc_link_lost；通过 rc_effect_t 输出动作。
- [x] 写主机行为测试并先观察失败：photo/video、停录后休眠、只唤醒、外部休眠、状态陈旧、拒绝与超时、长按互斥。
- [x] 实现固定内存协议及异步核心；官方 CRC 与已知帧交叉验证；测试分片、粘包和坏 CRC。
- [x] make -C tests/host test 通过，开启 ASan/UBSan。

### Task 2: ESP-IDF 集成

Files: remote/remote_ble.[ch], remote/remote_app.[ch], main/app_main.c, main/CMakeLists.txt, main/Kconfig.projbuild, sdkconfig.defaults。
Consumes: Task 1 的事件接口与动作；Produces: 上电自动配对、NVS 绑定、按键控制、LED、串口清除绑定。
- [x] 增加真实控制核心到模拟相机的协议闭环测试，先观察失败再补齐对应流程。
- [x] 实现非阻塞 BLE 状态推进、CCCD 就绪、地址类型、连接编号、异步广播和传输错误恢复。
- [x] 适配本机工具链并执行 idf.py build、idf.py size。

### Task 3: 交付和复核

Files: README.md, docs/VALIDATION.md, scripts/build.sh, scripts/test.sh。
- [x] 写运行说明、可复现构建命令、实机检查表及已知限制。
- [x] 执行全套主机测试、官方帧测试、固件编译，记录结果。
- [x] 独立代码审查并修复关键问题；复验受影响测试和固件。

## 执行记录

- 初始环境：工作目录为空；现有 ESP-IDF 6.1.1 可用。clone 官方仓库后创建 feat/ble-camera-remote，无提交。
- Ruling: 使用专用异步适配替换生产入口中的示例任务，保留官方源码与 CRC。原因是同步重连与分散状态不适合严格的唤醒/休眠契约；代价是新增 BLE 适配需实机验证。

- Task 1: complete — 主机测试先对占位核心观察长按失败，再实现通过；增加 ACK、倒计时、手势和已知帧回归。
- Task 2: complete — ESP-IDF 6.1.1 构建；实际 BLE 适配器在 SDK 替身边界运行闭环及异常用例。
- Task 3: complete — 操作说明、验证清单、脚本已提供；独立审查的 1 个 P1 与 4 个 P2 均有复现测试并修复。
- Ruling: 当前仅支持已安装的 ESP-IDF 6.1.x，led_strip 锁定 3.1.1；取消建连接口使用 6.1 参数结构，未宣称兼容 5.5。
- Ruling: 传输阶段丢失 SDK 完成事件超过 30 秒时重启 MCU，避免取消流程永久卡住；不补拍，代价是重建连接。
- Final: fixed 流缓冲回调下溢 — ASan negative-size-param RED→GREEN。
- Final: fixed 异步失败等待、取消代次竞态 — 真实适配器 scan_error / immediate_error / cancel_race / adv_* / close_error RED→GREEN。
- Final: fixed 预录制休眠和过渡手势 — test_prerecord_sleep / test_transition_gesture RED→GREEN。
- Final: fixed 迟到快门 ACK、释放去抖阈值、照片倒计时 — 对应主机回归 RED→GREEN。
- 无待处理的重要审查项；实机验证按用户要求留待后续，不计为本次已完成项。
