# DoitDuck

基于 [Microduck-kissqy](https://github.com/kissqy/Microduck-kissqy) 的双足机器鸭。Microduck-kissqy 是 [Microduck](https://github.com/pollen-robotics/microduck) 的飞特 HD1910 舵机版本（原版使用 Dynamixel XL330）。DoitDuck 沿用它的机械结构、舵机和动作模型格式，主控换成四博智联 Cove-610（Hi3516CV610），增加了网页控制、语音对话和视觉功能。

- 15 个总线舵机和总线 IMU，100 Hz 反馈，50 Hz 运行强化学习策略
- 行走、摔倒后自动起身、捡东西、翻跟头
- 浏览器控制：双摇杆、技能、摄像头画面、状态、标定向导、Wi-Fi 设置，中英文
- 唤醒词「你好小鸭」，接入小智大模型，通过 MCP 控制动作
- 手势识别、ToF 测距（可选）、合成叫声

文档：[快速开始](docs/quickstart.md) · [物料清单](hardware/BOM.md) · [训练动作模型](docs/training.md)

## 与 Microduck / Microduck-kissqy 的区别

| | 沿用 | DoitDuck |
| --- | --- | --- |
| 机械 | Microduck-kissqy 的 HD1910 版骨架和 15 个 HD1910 舵机 | — |
| 动作模型 | Microduck 格式：MLP 61 → 512 → 256 → 128 → 14，ONNX + `manifest.json`（schema 2） | 转成 float32 权重，在主控上推理 |
| 叫声 | Microduck `sounds` 的合成算法 | 移植到 Python，16 kHz，支持主动叫和上传录音 |
| 主控和电路 | — | Cove-610、IMU 小板（ESP32-C2 + LSM6DSV16X，兼电源板）、CV610 连接板 |
| 交互 | — | 网页、热点和蓝牙配网、语音、小智、手势、ToF |

## 硬件

| 部件 | 说明 |
| --- | --- |
| 主控 | Cove-610（Hi3516CV610），舵机总线用 UART1（U-Boot 环境变量 `sdio1_pins=none`） |
| CV610 连接板 | 舵机总线收发（半双工 1 Mbps，发送使能 GPIO1_0）、3.3 V 降压、ToF 接口。[原理图](hardware/cv610_board.pdf) |
| IMU 小板 | ESP32-C2 + LSM6DSV16X，总线 ID 200，带电池接口、电源开关和 3 个舵机接口。[固件](firmware/imu_board) · [原理图](hardware/imu_board.pdf) |
| 可选 | 摄像头；麦克风和扬声器；ToF VL53L5CX（I2C1，SDA GPIO1_5，SCL GPIO1_4） |

| 舵机 ID | 关节 |
| --- | --- |
| 10–14 | 右腿：髋偏航、髋横滚、髋俯仰、膝、踝 |
| 20–24 | 左腿：同上 |
| 30–33 | 颈俯仰、头俯仰、头偏航、头横滚 |
| 34 | 嘴 |

新舵机用 [tools/servo_tool.py](tools/servo_tool.py) 设置（Windows，USB 转舵机总线，1 Mbps；打包：`pyinstaller -F -w tools/servo_tool.py`）。支持批量改 ID、转到零位（2048）、把当前位置设为零位、拖动控制位置。同 ID 的多个舵机回复会冲突，扫描只能显示「至少 N 个」。

## 编译

PC 版，不需要板子，用于调试网页和指令：

```sh
cmake -S firmware/main_board/pc -B build/pc && cmake --build build/pc
cd build/pc && ./doitduck          # http://localhost:9527
```

Cove-610，需要 V610-App-SDK，在 V610-Apps 根目录执行：

```sh
source <V610-App-SDK>/export.sh
sh tools/build.sh projects/robot/doitduck/firmware/main_board
```

产物在 `firmware/main_board/out/`。功能开关见 [Kconfig](firmware/main_board/Kconfig) 和 [sdkconfig.defaults](firmware/main_board/sdkconfig.defaults)。

## 部署

```sh
ssh root@<IP> '/etc/init.d/S40app stop'
tar cf - -C firmware/main_board/out . | ssh root@<IP> 'mkdir -p /root/doitduck && tar xf - -C /root/doitduck'
scp -r models root@<IP>:/root/doitduck/
```

- 开机自启：`/etc/init.d/S40app` 的 `APP_PATH` 改为 `/root/doitduck/doitduck`。
- 开机连 Wi-Fi：把 [tools/board/S52wifi](tools/board/S52wifi) 放到 `/etc/init.d/`。
- 热点配网需要 `BR2_PACKAGE_WPA_SUPPLICANT_AP_SUPPORT`。
- 蓝牙配网需要 BlueZ 和 GLib（`BR2_PACKAGE_AIC8800D_SDIO_BLE`）。程序链接 GLib，镜像没有时在 Kconfig 关闭 `DOITDUCK_BLUFI`。
- 标定、导入的动作等数据在 `/root/doitduck/data/`，重新部署不会覆盖。

在线升级：网页「设置 → 升级」上传升级包，保留 `data/`。

```sh
sh tools/pack_ota.sh app                                   # 版本号取自 main/app/api.h
sh tools/pack_ota.sh models 1.1 models/microduck_walk.bin  # 只替换同名模型
```

烧录镜像：构建好 V610-Buildroot 后执行 `sh tools/pack_image.sh <V610-Buildroot 路径>`，输出在 `build/image/flash/`，已包含自启和 Wi-Fi 配置。

首次使用（配网、标定、行走）见[快速开始](docs/quickstart.md)。

## 模型

部署时把 `models/` 整个拷到 `/root/doitduck/models/`。缺少的模型对应功能会被关闭。

| 文件 | 用途 | 来源 |
| --- | --- | --- |
| `microduck_walk.bin` 等 | 行走、起身、技能（见 `robots/microduck.json`） | 不随仓库提供，自己训练或转换 |
| `nihaoxiaoya.mipk` | 唤醒词「你好小鸭」 | 仓库自带，MicroWakeWord |
| `kaishipeiwang.mipk` | 命令词「开始配网」 | 仓库自带，MicroWakeWord |
| `mediapipe_hand.mipk` | 手势 | 仓库自带，MediaPipe（Apache-2.0） |

`.mipk` 模型只能在 Hi3516CV610 上运行。

动作模型由 ONNX 和 `manifest.json` 转换而来，只需 Python 标准库：

```sh
python3 tools/import_policy.py policy.onnx manifest.json --out models/microduck_walk.bin
python3 tools/import_policy.py policy.onnx manifest.json --host <IP>   # 导入到运行中的鸭子
```

也可以在网页「动作」页导入。导入的动作需要在网页上标为已验证后，语音、小智、手柄才能调用；替换行走、起身前要先卸力。`manifest` 的 `robot.model` 必须在 `policy.compatible_models` 中。第三方模型请注意许可，例如 Microduck-kissqy 的模型仅限个人非商业使用。

## 使用

### 网页

浏览器打开 `http://<IP>/`。

| 页面 | 内容 |
| --- | --- |
| 控制 | 摄像头、模式（卸力、仅推理、站好、行走）、摇杆、技能、语音命令 |
| 动作 | 导入、验证、删除动作模型 |
| 状态 | 姿态、舵机、IMU、ToF、运行频率 |
| 维护 | 标定向导、舵机工具、标定数据导入导出 |
| 设置 | Wi-Fi、动作平滑、音量、小智、语音命令、许可 |

### 语音和小智

说「你好小鸭」开始和小智对话。首次使用在「设置 → 小智」获取 6 位激活码，到 [xiaozhi.me](https://xiaozhi.me) 添加设备。小智的设备号是 Wi-Fi MAC，恢复出厂后不变。

命令词需要先唤醒，在 `voice.window_s` 秒内说出才执行。每个命令词对应一个 MicroWakeWord 模型，在 `voice.commands` 中配置：

```json
{ "words": "开始配网", "model": "kaishipeiwang", "call": "wifi.ap", "params": { "on": true } }
```

没有 `model` 的命令只能在网页语音命令条中使用。

### 配网

未配网时开机自动打开热点；也可以说「你好小鸭，开始配网」，或在「设置 → Wi-Fi」点「热点配网」。热点名为 `DoitDuck-xxxx`，无密码，打开时不影响当前 Wi-Fi 连接。

- **热点**：手机连上后自动弹出设置页（或打开 `http://192.168.4.1/`），选择网络并输入密码，连上后页面显示新 IP 并关闭热点。
- **蓝牙**：热点打开期间同时广播 `DTXZ_<Wi-Fi MAC>`（如 `DTXZ_10b06e94224e`），用「四博AI」小程序配网。连上后小程序收到 `{"code": "571480", "ip": "192.168.1.23", "url": "http://192.168.1.23"}`，`code` 为小智激活码，已激活时为空；随后热点和蓝牙关闭。

已联网时，热点 5 分钟无操作自动关闭。勾选「热点常开」后热点常开，开机也会打开，可在没有路由器时使用。

### 手势、ToF 和叫声

- 手势：点赞点头、张开手掌张嘴、握拳闭嘴，在 `gesture` 中配置。
- ToF：开机自动检测，「状态」页显示 8×8 距离图。
- 叫声：由 `tools/gen_sounds.py` 生成，`--seed` 更换音色；事件和主动叫在 `sounds` 中配置；网页可上传 16 kHz 单声道 WAV。

## 配置

### Kconfig

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `DOITDUCK_WEB` / `VOICE` / `GESTURE` / `WIFI` / `TOF` / `BLUFI` | 开 | 功能开关 |
| `DOITDUCK_XIAOZHI` | 开 | 小智，需要 mipk 唤醒和网络 |
| `DOITDUCK_GAMEPAD` | 关 | 手柄，尚未实现读取 |
| `DOITDUCK_ROBOT` | `microduck` | 机身配置 |
| `DOITDUCK_HTTP_PORT` | 80 | 网页端口 |
| `DOITDUCK_BUS_DEVICE` | `/dev/ttyAMA1` | 舵机总线串口 |
| `DOITDUCK_BUS_PINMUX` | — | 启动时写入的引脚复用，`寄存器=值`，空格分隔 |
| `DOITDUCK_BUS_TX_ENABLE_BASE` / `PIN` | GPIO1_0 | 总线发送使能，`PIN` 为 -1 表示收发器自动切换方向 |
| `DOITDUCK_TOF_I2C` | `/dev/i2c-1` | ToF 的 I2C 设备 |
| `DOITDUCK_MIC_CHANNEL` / `MIC_GAIN_DB` / `SPEAKER_GAIN_DB` | 0 / 50 / -10 | 麦克风声道和增益、扬声器增益 |

### 机身配置 `robots/*.json`

| 字段 | 说明 |
| --- | --- |
| `motors.ids` / `names` / `home_deg` | 舵机 ID、名称、默认站姿（度） |
| `motors.kp` / `kd` / `mouth_kp` / `kp_by_id` | 舵机增益，`kp_by_id` 单独设置，如 `{ "30": 12 }` |
| `imu.axis_map` / `axis_sign` | IMU 轴到机身轴的映射 |
| `policy.policies` | 动作列表；`doitduck` 字段可设标签、平滑 `alpha`、张嘴 `mouth_deg` / `mouth_until`、`hold_recovery`、`home_deg` |
| `policy.smoothing` / `smoothing_head` | 腿、头的动作平滑默认值 |
| `policy.fall` / `stand` | 摔倒和站稳判定 |
| `limits` | 各控制来源的最大速度 |
| `voice` / `gesture` / `gamepad` / `head_presets` | 语音命令、手势、手柄按键、头部预设 |
| `sounds` | 事件叫声和主动叫 |
| `xiaozhi` | 启用开关、OTA 地址 |

## IMU 小板固件

```sh
cd firmware/imu_board
idf.py set-target esp32c2 && idf.py build && idf.py -p <串口> flash monitor
```

接线、校准和总线协议见 [firmware/imu_board/README.md](firmware/imu_board/README.md)。

## 目录

```text
firmware/main_board/   主控程序
├── main/core/         总线、仲裁、动作、策略推理
├── main/app/          启动、指令、配置、动作库、标定、事件、叫声
├── main/features/     web gamepad voice xiaozhi gesture tof wifi blufi
├── components/        适配层和第三方库
└── pc/                PC 版构建
firmware/imu_board/    IMU 小板固件（ESP-IDF）
robots/                机身配置
web/                   网页
sounds/                叫声和语音
models/                模型
tools/                 导入、生成、打包工具
hardware/              原理图、物料清单
docs/                  文档
```

## 致谢

感谢 Microduck 和 Microduck-kissqy 的作者开放设计和模型。第三方代码及许可见 [THIRD_PARTY.md](THIRD_PARTY.md)。

## 许可

[PolyForm Noncommercial License 1.0.0](LICENSE.md)，禁止商业使用。可修改和分享，分享时需附带 `LICENSE.md`；商业使用需另行获得授权。第三方组件遵循各自许可，见 [THIRD_PARTY.md](THIRD_PARTY.md)。
