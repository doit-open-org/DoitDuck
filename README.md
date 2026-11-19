# DoitDuck

DoitDuck 是 [Microduck](https://github.com/pollen-robotics/microduck) 双足机器鸭飞特 HD1910 舵机版本的复刻：机械结构、舵机和动作模型沿用这个版本，主控换成四博智联的 Cove-610（Hi3516CV610），并加上了网页控制、语音对话和视觉功能。

- 15 个总线舵机 + 总线 IMU，100 Hz 反馈，50 Hz 运行强化学习动作模型
- 行走、摔倒自动起身，以及捡东西、翻跟头等技能
- 手机、电脑浏览器控制：双摇杆、技能按钮、摄像头画面、状态监控、标定向导、Wi-Fi 设置，中英文
- 「你好小鸭」唤醒后和小智大模型对话，大模型可以直接让鸭子做动作
- 摄像头手势、ToF 测距（可选）、合成的鸭子叫声

| 我想…… | 看这里 |
| --- | --- |
| 烧录镜像、配网、开始玩 | [快速开始](docs/quickstart.md) |
| 买料、做板子、组装 | [物料清单](hardware/BOM.md)、下文「硬件」 |
| 编译、部署、改代码 | 下文「编译」「部署」，[设计文档](docs/doitduck-design.md) |
| 训练自己的动作 | [训练动作模型](docs/training.md) |

## 和 Microduck 的关系

| | 沿用 Microduck | DoitDuck 的改动 |
| --- | --- | --- |
| 机械 | HD1910 版本的骨架打印件（如 [Microduck-kissqy](https://github.com/kissqy/Microduck-kissqy)）、15 个飞特 HD1910 舵机及其布局 | — |
| 动作模型 | 网络结构（MLP 61 → 512 → 256 → 128 → 14）、ONNX + `manifest.json`（schema 2），兼容的 Microduck 模型可以直接导入 | 转换成 float32 权重文件，在主控上推理 |
| 叫声 | 合成算法，来自 Microduck 的 `sounds` | 从 Rust 移植到 Python，改为 16 kHz；另外支持主动叫和上传录音 |
| 主控和电路 | — | Cove-610 主控、自制 IMU 小板（ESP32-C2 + LSM6DSV16X，兼做电源板）、CV610 连接板 |
| 交互 | — | 网页控制和标定向导、热点和蓝牙配网、唤醒词和小智对话、手势、ToF |

感谢 Microduck 的作者开放设计和模型。

## 硬件

物料清单见 [hardware/BOM.md](hardware/BOM.md)。

| 部件           | 说明                                                                                                                                                  |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| 主控           | Cove-610（Hi3516CV610），舵机总线用 UART1（U-Boot 环境变量 `sdio1_pins=none`）                                                                      |
| CV610 连接板   | 插在 Cove-610 上：舵机总线收发（半双工 1 Mbps，发送使能接 GPIO1_0）、舵机总线电源降压到 3.3 V、ToF 测距接口，原理图 [hardware/cv610_board.pdf](hardware/cv610_board.pdf) |
| IMU 小板       | ESP32-C2 + LSM6DSV16X，挂在舵机总线上（ID 200）；板上有电池接口、电源开关和 3 个舵机接口，固件 [firmware/imu_board](firmware/imu_board)，原理图 [hardware/imu_board.pdf](hardware/imu_board.pdf) |
| 舵机           | 15 个，ID 见下表                                                                                                                                      |
| 可选           | 摄像头（网页画面、手势），麦克风和扬声器（唤醒、小智、叫声），ToF VL53L5CX（接 CV610 连接板的测距接口，I2C1：SDA GPIO1_5，SCL GPIO1_4）                                          |

| ID     | 关节                                 |
| ------ | ------------------------------------ |
| 10–14 | 右腿：髋偏航、髋横滚、髋俯仰、膝、踝 |
| 20–24 | 左腿：同上                           |
| 30–33 | 颈俯仰、头俯仰、头偏航、头横滚       |
| 34     | 嘴                                   |

新舵机可以用 Windows 上位机 [tools/servo_tool.py](tools/servo_tool.py) 成批设置（USB 转舵机总线的串口板，1 Mbps；打包 exe：`pyinstaller -F -w tools/servo_tool.py`）：

- 批量改 ID：把同一 ID 的舵机并在一条总线上，填原 ID 和新 ID，一次全部改好。
- 转到零位：舵机转到 2048 并保持上力，这时装舵盘；用完点卸力。
- 当前位置设为零位：把舵机当前位置写成 2048。
- 位置控制：拖动滑动条控制「零位」一栏填的 ID（0～4095），用完点卸力。

「扫描总线」会显示舵机数量。ID 各不相同时是准确数量；同 ID 的舵机回复会冲突，数不出具体几个，只显示「至少 N 个」，改 ID 后也只能确认「旧 ID 已无回复、新 ID 有回复」。

## 机身配置

`robots/microduck.json` 描述机身：舵机、默认站姿、增益、动作列表、语音命令、叫声等，对应 Microduck 官方骨架（如 [Microduck-kissqy](https://github.com/kissqy/Microduck-kissqy) 打印件 + 飞特 HD1910）。动作模型是 `microduck_walk.bin`、`microduck_ground_pick.bin`、`microduck_roulade.bin`。

## 编译

### 电脑上（不需要板子）

用来试网页和指令，没有舵机和音视频：

```sh
cmake -S firmware/main_board/pc -B build/pc && cmake --build build/pc
cd build/pc && ./doitduck          # 浏览器打开 http://localhost:9527
```

### Cove-610

需要 V610-App-SDK。在 V610-Apps 根目录：

```sh
source <V610-App-SDK>/export.sh
sh tools/build.sh projects/robot/doitduck/firmware/main_board
```

产物在 `firmware/main_board/out/`：程序 `doitduck` 和 `robots/`、`web/`、`sounds/`，以及空的 `models/`。功能开关和板子参数在 [Kconfig](firmware/main_board/Kconfig)，默认值在 [sdkconfig.defaults](firmware/main_board/sdkconfig.defaults)，见下文「配置参考」。

## 部署

```sh
ssh root@<IP> '/etc/init.d/S40app stop'
tar cf - -C firmware/main_board/out . | ssh root@<IP> 'mkdir -p /root/doitduck && tar xf - -C /root/doitduck'
scp -r models root@<IP>:/root/doitduck/            # 模型，见下文
```

- 开机自启：把 `/etc/init.d/S40app` 的 `APP_PATH` 改成 `/root/doitduck/doitduck`。
- 开机连 Wi-Fi：把 [tools/board/S52wifi](tools/board/S52wifi) 拷到 `/etc/init.d/` 并 `chmod +x`。
- 热点配网需要系统的 `wpa_supplicant` 带 AP 支持（Buildroot 选项 `BR2_PACKAGE_WPA_SUPPLICANT_AP_SUPPORT`）。
- 蓝牙配网需要镜像带 BlueZ 和 GLib（Buildroot 选项 `BR2_PACKAGE_AIC8800D_SDIO_BLE`）。程序链接了 GLib，镜像里没有它程序就启动不了；不用蓝牙时在 Kconfig 关掉 `DOITDUCK_BLUFI`。
- 零位、IMU 水平、导入的动作、小智设备号等都存在 `/root/doitduck/data/`，重新部署不会覆盖。

已经部署好的鸭子可以在网页「设置 → 升级」上传升级包，应用和模型分开升级，`data/` 保留：

```sh
sh tools/pack_ota.sh app                                   # 应用，版本号是 main/app/api.h 的 DOITDUCK_VERSION
sh tools/pack_ota.sh models 1.1 models/microduck_walk.bin  # 模型，可以列多个，只替换同名的
```

也可以直接做一个带 DoitDuck 的烧录镜像：先构建好 V610-Buildroot，再在本目录运行 `sh tools/pack_image.sh <V610-Buildroot 路径>`，烧录文件在 `build/image/flash/`，开机自启和 Wi-Fi 都已装好。

## 第一次使用

自己部署的，先放好动作模型（见下文「模型」）；烧录镜像的已经带了。之后配网、标定向导、开始行走的完整步骤见[快速开始](docs/quickstart.md)。

## 模型

语音和手势模型在仓库的 `models/` 里，只能用在 Hi3516CV610 上。动作模型仓库不带，要自己放到 `models/`。部署时整个目录拷到 `/root/doitduck/models/`。缺哪个，对应的功能就关掉，其他照常运行。

| 文件                     | 用途               | 来源                                                                                    |
| ------------------------ | ------------------ | --------------------------------------------------------------------------------------- |
| 「机身配置」里的动作模型 | 行走、起身、技能   | 自己训练（见 [docs/training.md](docs/training.md)），或转换兼容的 Microduck 模型（见下） |
| `nihaoxiaoya.mipk`     | 「你好小鸭」唤醒   | 仓库自带，自己训练的 MicroWakeWord 模型                                                 |
| `kaishipeiwang.mipk`   | 「开始配网」命令词 | 仓库自带，自己训练的 MicroWakeWord 模型                                                 |
| `mediapipe_hand.mipk`  | 手势               | 仓库自带，MediaPipe 手部模型（Apache-2.0）                                              |

动作模型是一个 ONNX（MLP 61 → 512 → 256 → 128 → 14）加一份 Microduck schema 2 的 `manifest.json`。用 `tools/import_policy.py` 转换成程序用的 `.bin`（只需要 Python 标准库）：

```sh
python3 tools/import_policy.py policy.onnx manifest.json --out models/microduck_walk.bin   # 转换后放进 models/
python3 tools/import_policy.py policy.onnx manifest.json --host <IP>                      # 或者直接导入到运行中的鸭子
```

也可以在网页「动作」页导入。导入的动作先标为「未验证」，只能从网页触发，在网页上标为已验证后语音、小智、手柄才能用；替换行走、起身动作要先卸力。manifest 的 `robot.model` 要在机身配置的 `policy.compatible_models` 里。

使用别人发布的模型前请看清许可证，例如 Microduck-kissqy 的自训模型是个人非商业许可。

## 网页

`http://<IP>/`，手机和电脑都能用，支持中英文，浅色和深色跟随系统。

| 页面 | 内容                                                                   |
| ---- | ---------------------------------------------------------------------- |
| 控制 | 摄像头画面、模式（卸力、仅推理、站好、行走）、双摇杆、技能、语音命令条 |
| 动作 | 导入、验证、删除动作模型                                               |
| 状态 | 姿态、舵机角度和在线情况、IMU、ToF 距离图、运行频率                    |
| 维护 | 标定向导、舵机工具（扫描、改 ID、增益）、标定数据导出、导入、撤销      |
| 设置 | Wi-Fi、动作平滑、音量和叫声、小智、语音命令和手势一览、关于和许可      |

左上角按钮切换中英文。

## 语音和小智

说「你好小鸭」唤醒后开始和小智对话，大模型通过 MCP 让鸭子行走、转向、做技能、动头和嘴。第一次使用时，在「设置 → 小智」看到 6 位激活码，到 [xiaozhi.me](https://xiaozhi.me) 控制台添加设备并输入激活码。

### 命令词

机身配置 `voice.commands` 里带 `model` 的命令，先说唤醒词「你好小鸭」，再在 `voice.window_s` 秒内说命令词才执行，每个命令词一个 MicroWakeWord 模型（`models/<model>.mipk`）。唤醒词的模型由 `voice.wake_model` 指定。没有 `model` 的命令只在网页语音条里用。加命令词：训练一个 mipk 模型放进 `models/`，再在 `voice.commands` 里加一项：

```json
{ "words": "开始配网", "model": "kaishipeiwang", "call": "wifi.ap", "params": { "on": true } }
```

### 热点配网

还没配过网时鸭子开机自动打开热点；之后说「开始配网」，或在「设置 → Wi-Fi」点「热点配网」。鸭子打开无密码热点 `DoitDuck-xxxx`，现有的 Wi-Fi 连接不会断。

1. 手机连上热点，设置页会自动弹出；没弹出就打开 `http://192.168.4.1/`。
2. 选择网络、输入密码并连接，页面实时显示连接状态。
3. 连上后弹窗显示新的 IP，确认后关闭热点。密码错了热点不关，改了再连。

热点开着时，鸭子也用蓝牙广播 `DTXZ_<Wi-Fi MAC>`（如 `DTXZ_10b06e94224e`，和小智设备号是同一个 MAC），可以用「四博AI」小程序通过蓝牙配网（BluFi）。连上后（最多再等 20 秒小智的激活码）小程序收到自定义数据 `{"code": "571480", "ip": "192.168.1.23", "url": "http://192.168.1.23"}`，`code` 是小智激活码，已激活或没开小智时为空。之后热点和蓝牙一起关闭（勾选「热点常开」时不关）。

没有操作时，鸭子已经在网上的话，热点 5 分钟后自动关闭。勾选「热点常开」后热点一直开着，开机也自动打开，外出没有路由器时手机连热点就能控制。

## 手势、ToF 和叫声

- 摄像头看到点赞点头、张开手掌张嘴、握拳闭嘴，对应关系在机身配置的 `gesture` 段。
- 开机自动检测 ToF（VL53L5CX），网页「状态」页显示 8×8 距离图；没接时启动日志提示一次并关闭。
- `sounds/` 里的叫声由 `tools/gen_sounds.py` 合成，`--seed <数字>` 换一种嗓音。哪个事件叫哪个声音、站着没人理时多久叫一次，在机身配置的 `sounds` 段。网页上可以上传自己的录音（16 kHz 单声道 WAV）、调音量、关掉主动叫。

## 配置参考

### Kconfig

| 选项                                                             | 默认             | 说明                                               |
| ---------------------------------------------------------------- | ---------------- | -------------------------------------------------- |
| `DOITDUCK_WEB` / `VOICE` / `GESTURE` / `WIFI` / `TOF`  | 开               | 各功能开关                                         |
| `DOITDUCK_XIAOZHI`                                             | 开               | 小智（需要 mipk 唤醒和网络）                       |
| `DOITDUCK_GAMEPAD`                                             | 关               | 手柄（还没有读取实现）                             |
| `DOITDUCK_ROBOT`                                               | `microduck`    | 机身配置                                           |
| `DOITDUCK_HTTP_PORT`                                           | 80               | 网页端口                                           |
| `DOITDUCK_BUS_DEVICE`                                          | `/dev/ttyAMA1` | 舵机总线串口                                       |
| `DOITDUCK_BUS_PINMUX`                                          | —               | 启动时写的引脚复用，`寄存器=值`，空格分隔        |
| `DOITDUCK_BUS_TX_ENABLE_BASE` / `PIN`                        | GPIO1_0          | 总线发送使能；`PIN` 为 -1 表示总线收发器自己切换方向 |
| `DOITDUCK_TOF_I2C`                                             | `/dev/i2c-1`   | ToF 所在 I2C                                       |
| `DOITDUCK_MIC_CHANNEL` / `MIC_GAIN_DB` / `SPEAKER_GAIN_DB` | 0 / 50 / -10     | 麦克风声道和增益、扬声器增益                       |

### 机身配置（robots/*.json）

| 字段                                                     | 说明                                                                                                                                                                                   |
| -------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `motors.ids` / `names` / `home_deg`                | 舵机 ID、名称、默认站姿（度）                                                                                                                                                          |
| `motors.kp` / `kd` / `mouth_kp` / `kp_by_id`     | 舵机增益；`kp_by_id` 单独给某个舵机设 P，如 `{ "30": 12 }`                                                                                                                         |
| `imu.axis_map` / `axis_sign`                         | IMU 板轴到机身轴的映射                                                                                                                                                                 |
| `policy.policies`                                      | 动作列表：文件、名字、类型、时长等；`doitduck` 段可写标签、平滑 `alpha`、张嘴 `mouth_deg` / `mouth_until`、动作中不判断摔倒 `hold_recovery`、动作自己的默认站姿 `home_deg` |
| `policy.smoothing` / `smoothing_head`                | 腿、头的动作平滑默认值（网页可调）                                                                                                                                                     |
| `policy.fall` / `stand`                              | 摔倒和站稳的判断                                                                                                                                                                       |
| `limits`                                               | 各控制来源的最大速度                                                                                                                                                                   |
| `voice` / `gesture` / `gamepad` / `head_presets` | 语音命令、手势、手柄按键、头部预设                                                                                                                                                     |
| `sounds`                                               | 事件叫声和主动叫                                                                                                                                                                       |
| `xiaozhi`                                              | 是否启用、OTA 地址                                                                                                                                                                     |

## IMU 小板固件

```sh
cd firmware/imu_board
idf.py set-target esp32c2 && idf.py build && idf.py -p <串口> flash monitor
```

接线、晶振和校准见 [firmware/imu_board/README.md](firmware/imu_board/README.md)。

## 目录

```text
firmware/main_board/   主控程序
├── main/core/         控制核心：总线、仲裁、动作、策略推理
├── main/app/          启动、统一指令、配置、动作库、标定、事件、叫声
├── main/features/     web gamepad voice xiaozhi gesture tof wifi，可单独开关
├── components/        适配层和第三方库
└── pc/                电脑版构建
firmware/imu_board/    IMU 小板固件（ESP-IDF）
robots/  机身配置      web/  网页        sounds/  叫声
tools/   导入、合成叫声、打包镜像等工具  hardware/  原理图、物料清单   docs/  设计文档
```

设计文档：[docs/doitduck-design.md](docs/doitduck-design.md)（软件），[docs/training.md](docs/training.md)（训练动作模型）。IMU 小板的接线和总线协议见 [firmware/imu_board/README.md](firmware/imu_board/README.md)。第三方代码和许可见 [THIRD_PARTY.md](THIRD_PARTY.md)。

## 许可

本项目采用 [PolyForm Noncommercial License 1.0.0](LICENSE.md)，**禁止商业使用**。可以修改和分享，分享时需附带 `LICENSE.md`。商业使用需另行获得授权。

第三方组件不受此限制，遵循各自的许可证，见 [THIRD_PARTY.md](THIRD_PARTY.md)。
