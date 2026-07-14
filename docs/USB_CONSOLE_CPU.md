# 接电脑 USB 后 CPU 飙高发烫 — 根因调研

现象：真机刷入固件后，用数据线连电脑（USB JTAG/serial debug unit 口）静置，一段时间后
CPU 占用飙高、整机发烫；插普通充电头（仅供电）正常，不插线也正常。

## 结论

根因不在应用代码，而在 **ESP-IDF 的 USB-Serial-JTAG（USJ）控制台写路径**。全仓库没有任何
一处代码显式判断"是否连着电脑"——电源侧只有无线充线圈 I2C 探测
（`components/metalio_hal/src/power/power.cc`）和 BQ27220 电流符号判充电
（`src/power/bq27220_gauge.cc`），与 USB 数据链路无关。唯一随"接没接电脑"改变行为的，
是每条 `ESP_LOGx` 底下的 console 驱动；而本固件有稳定日志源：`mhal::sysmon` 每秒 4 行
INFO（`components/metalio_hal/src/sysmon.cc:35-83`，行内含时间戳、ANSI 色码、中文
UTF-8，单行远超 USJ 64 字节 TX FIFO），网络栈等还有偶发日志叠加。

## 机制链条（ESP-IDF v5.5.4 源码逐行核对）

1. **console 配置走默认值。** `sdkconfig.defaults` 无任何 `CONFIG_ESP_CONSOLE_*`，P4
   默认为 primary = UART0、**secondary = USB_SERIAL_JTAG**（`esp_system/Kconfig` 中
   `ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG` 默认开启）。每行日志同时写 UART0 与 USJ
   （`esp_vfs_console/vfs_console.c` `console_write`）。
2. **USJ 写入口按"是否枚举"分叉。** `esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c`
   `usb_serial_jtag_write()` 第一句：`if (!usb_serial_jtag_is_connected()) return -1;`。
   连接判定基于 **USB SOF 包**（`usb_serial_jtag_connection_monitor.c`：主机每 1ms 发一次
   SOF，tick hook 检测；无 SOF 若干 tick 判离线）——插电脑即使不开串口终端也判"已连接"，
   充电头无数据链路判"未连接"。
3. **已连接时逐字节发送带忙等窗口。** `usb_serial_jtag_tx_char_no_driver()`
   （usb_serial_jtag_vfs.c:148-171）：TX FIFO 满后自旋，直到距上次成功写入超过
   `TX_FLUSH_TIMEOUT_US`（50ms）才丢弃。只要主机侧偶有抽取（哪怕很慢），
   `last_tx_ts` 被刷新，50ms 忙等窗口反复重置——打日志的任务（sysmon 优先级 5）
   持续自旋烧 CPU。

三场景对照：

| 场景 | SOF / is_connected | 日志写 USJ 的行为 | 表现 |
|------|-------------------|------------------|------|
| 充电头 / 不插线 | 无 / false | 立即 `return -1`，零开销 | 正常 |
| 接电脑，不开终端 | 有 / true | FIFO 满 → 忙等窗口反复触发 | CPU 飙高、发烫 |
| 接电脑，开 `idf.py monitor` | 有 / true | 主机正常抽取，不忙等 | 正常（观察者效应） |

放大因素：项目未开 `CONFIG_PM_ENABLE` / tickless idle，CPU 常驻满频，忙等直接转化为发热。

## 待真机确认的细节

严格按源码推演，若主机端**完全**不抽数据，理论上只自旋一次 50ms 后即快速丢弃；持续高
CPU 意味着电脑侧存在慢速抽取（某进程碰过该串口）。验证时注意**观察者效应**：接上
monitor 后主机开始正常抽数据，忙等即消失，"一调试就正常"正是此类问题的典型特征。

对照实验：`main.cc` 的 `mhal::Init()` 后临时加 `esp_log_level_set("*", ESP_LOG_NONE);`
重刷，插电脑静置——CPU 不再飙升即坐实根因。CPU 占用从设备屏幕（设置页系统信息）读取，
避免依赖串口观察。

## 修复方向（确认根因后按需选）

- **治本**：sdkconfig 明确 console 行为。`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` 让日志不写
  USJ（代价：monitor 口无日志）；或 primary 切 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` 并安装
  USJ 驱动（`usb_serial_jtag_driver_install` + `usb_serial_jtag_vfs_use_driver`），中断 +
  环形缓冲，写满阻塞让出 CPU 而非自旋（驱动模式下主机不读的行为需真机验证后再定）。
- **止血**：生产固件默认日志级别降到 WARN（`esp_log_level_set`），或拉长/降级 sysmon 打印
  周期，降低忙等触发频率。
- **独立优化**：评估开启 `CONFIG_PM_ENABLE` + tickless，降低整机温度基线。
