# DoitDuck IMU 小板

ESP32-C2 + LSM6DSV16XTR，固定在躯干上，作为设备 ID 200 挂在舵机总线上。协议和寄存器见下文「总线协议」。

所有代码在 [main/main.c](main/main.c)：

| 任务 | 优先级 | 内容 |
| --- | --- | --- |
| `bus_task` | 最高 | 解析总线帧，只应答发给 ID 200 的 PING / READ / WRITE |
| `sample_task` | 高 | 每 2 ms 读 LSM6DSV16X 的 FIFO（加速度、角速度 480 Hz，SFLP 零偏和重力 120 Hz），按传感器 XYZ 顺序解码、扣零偏、安装倾角旋转、10 ms 平均，执行校准 |
| `app_main` | 低 | 写 NVS、UART0 日志和按键命令 |

## 接线

以[原理图](../../hardware/imu_board.pdf)为准。

| ESP32-C2 | 原理图网络 | 用途 |
| --- | --- | --- |
| GPIO0 | DXL_TX_EN | SN74LVC2G125 1OE#，低电平发送，高电平释放总线 |
| GPIO1 | DXL_TX | UART1 TX，1 Mbps |
| GPIO2 | DXL_RX | UART1 RX，缓冲器接收常开 |
| GPIO3 | IMU_INT1 | 预留，当前轮询 FIFO，不使用 |
| GPIO4 | IMU_MISO | SPI MISO |
| GPIO5 | IMU_MOSI | SPI MOSI |
| GPIO6 | IMU_SCK | SPI 时钟，10 MHz，mode 3 |
| GPIO7 | IMU_CS | SPI 片选，低有效，R5 10 kΩ 上拉 |
| GPIO18 | LED | 测试灯，当前不使用 |
| GPIO19 / GPIO20 | U0RXD / U0TXD | UART0 下载、日志、校准命令，波特率同启动日志（26 MHz 晶振 74880） |
| GPIO9 | BOOT | 下载启动，引到烧录测试点 BOOT |

LSM6DSV16XTR 同时支持 SPI 和 I2C，本固件使用原理图标出的四线 SPI。UART1 通过 `UART_MODE_RS485_HALF_DUPLEX` 控制 RTS。该模式下 RTS 发送时为高、空闲为低（按 DE 高有效设计），与 1OE# 相反，所以用 `UART_SIGNAL_RTS_INV` 反相：发送时 GPIO0 拉低，最后一个停止位发送完成后拉高；物理接口仍是 TTL 单线，缓冲器替代旧版开漏直连方式。

电池（2S，8.4 V）接 CN2，经电源开关 U8 给本板和舵机总线（DXL_VDD）供电；板上 BL1117-33 将 8.4 V 转为 3.3 V，地与舵机总线共地。原理图没有电池分压采样，电池电压寄存器 `0x5C` 固定为 0（不可用）。

安装方向：小板不交换轴、不翻转单轴方向，XYZ 与传感器一致，安装轴映射由主控完成。保留现有协议的加速度和重力整体负号，以及零偏、安装倾角校准；倾角校准绕传感器轴执行。

## 构建和烧录

需要支持 ESP32-C2 的 ESP-IDF。板子用 26 MHz 晶振，`sdkconfig.defaults` 已按 26 MHz 配置，串口日志波特率 74880。

```sh
idf.py set-target esp32c2
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

板上没有下载接口和按键，用 3.3 V 电平的 USB 转 UART 下载器接烧录测试点 TX、RX、GND。进入下载模式：BOOT 测试点接 GND，再把 RESET 测试点短接一下 GND 复位。烧录后 esptool 自带的复位不能让小板正常启动（串口无日志、总线不应答），需断开 BOOT，再复位一次或重新上电。ESP32-C2 没有 USB Serial/JTAG 控制台，日志和命令均使用 UART0。

## 使用

串口日志每秒一行：加速度（mg）、角速度（°/s，已扣零偏）、状态位、电池电压。状态位 bit0 数据有效，bit1 零偏有效，bit2 安装倾角已校准，bit3 IMU 故障。

上电后静止 2 秒自动测一次零偏；之后用 SFLP 估计的零偏，每分钟变化超过 0.05 °/s 时存入 NVS。

校准可以在主控网页的「维护 → 标定向导」里做，也可以在串口监视器里按键：

| 按键 | 命令 | 要求 |
| --- | --- | --- |
| `1` | 测零偏 | 静止 2 秒，任意姿态 |
| `2` | 当前姿态设为水平 | 机身放平、静止 2 秒；倾角超过 15° 失败 |
| `3` | 清除零偏和安装倾角 | — |

校准结果保存在小板上，换主控不需要重新校准。

## 总线协议

帧格式和飞特 SCS 舵机相同，小端，1 Mbps，主控发起，小板只应答：

```text
指令：FF FF ID LEN INST PARAM… CHK      LEN = 参数个数 + 2
应答：FF FF ID LEN ERR  DATA…  CHK      LEN = 数据个数 + 2
CHK  = ~(ID + LEN + INST/ERR + Σ参数/数据) & 0xFF
```

| 指令 | 码 | 说明 |
| --- | --- | --- |
| PING | 0x01 | 应答无数据，检测在线 |
| READ | 0x02 | 参数：起始地址、长度（1～32） |
| WRITE | 0x03 | 只接受写 `0x60`（1 字节）和 `0x62`（4 字节） |

广播（ID 0xFE）、同步读（0x82）、同步写（0x83）是给舵机的，小板忽略。地址或长度不对时应答 ERR = 0x08。

| 地址 | 长度 | 内容 | 读写 |
| --- | --- | --- | --- |
| 0x00 | 2 | 型号 `0xD0 0x0D` | R |
| 0x02 | 1 | 固件版本 | R |
| 0x05 | 1 | ID，固定 200 | R |
| 0x40 | 6 | 加速度 x / y / z，int16，mg | R |
| 0x46 | 6 | 角速度 x / y / z，int16，0.1 °/s，已扣零偏 | R |
| 0x4C | 2 | 采样计数，uint16，每个新数据加 1 | R |
| 0x4E | 1 | 状态：bit0 数据有效，bit1 零偏有效，bit2 安装倾角已校准，bit3 IMU 故障 | R |
| 0x50 | 6 | SFLP 重力向量，int16，1/16384 g | R |
| 0x56 | 6 | 当前零偏，int16，0.01 °/s | R |
| 0x5C | 2 | 电池电压，uint16，mV；本板没有分压采样，固定为 0 | R |
| 0x60 | 1 | 校准命令：1 测零偏，2 当前姿态设为水平，3 清除；读为 0 | W |
| 0x61 | 1 | 校准结果：0 空闲，1 进行中，2 成功，3 失败（没有静止） | R |
| 0x62 | 4 | 安装横滚、俯仰，int16，0.01°，掉电保存 | R/W |

主控每 10 ms 读 `0x40` 起 15 字节，每秒读一次 `0x56` 起 16 字节，应答超时 8 ms。采样计数不变或状态 bit0 为 0 时，主控视为 IMU 数据无效，持续 100 ms 后卸力。
