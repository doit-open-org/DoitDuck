# DoitDuck 软件设计

DoitDuck 主控程序的结构和主要机制。IMU 总线协议见 [firmware/imu_board/README.md](../firmware/imu_board/README.md)，使用方法见 [README](../README.md)。

## 1. 设计取舍

PC 版和板子用同一套代码，不依赖 SDK，没有板子也能测网页、指令、仲裁、MCP 和动作导入。

网页、手柄、语音、小智、手势、ToF、Wi-Fi 各是一个功能模块，可以单独关掉，平台没有的能力不编译。语音识别、网络、编解码、存储、音视频都走适配层，换实现只改 Kconfig。

一个动作就是一个权重文件加一份 manifest，由用户导入，仓库不带动作模型。舵机、默认站姿、增益、动作列表、语音命令、叫声都写在 `robots/<名字>.json`。

## 2. 分层

```text
 网页      手柄      麦克风       摄像头      小智服务器
   │         │         │            │              │
┌──┴──┐ ┌────┴──┐ ┌────┴─┐ ┌───────┐ ┌──┴────┐ ┌───┐ ┌────┐
│ web │ │gamepad│ │voice │▶│xiaozhi│ │gesture│ │tof│ │wifi│   features/  功能，可单独开关
└──┬──┘ └───┬───┘ └──┬───┘ └───┬───┘ └───┬───┘ └─┬─┘ └──┬─┘
   └────────┴────────┴─────────┴─────────┴───────┴──────┘
                            ▼
        app/  启动、统一指令、配置、动作库、标定、事件、叫声
                            ▼
        core/ 总线、仲裁、动作、策略推理（只用 osal / bus / storage）
                            ▼
        components/ 各能力的接口 + 实现（V610、PC 空实现）
```

依赖只能往下：`features → app → core`，`features → components`，`core → components/{osal,bus,storage}`。功能之间互不 include，只通过统一指令和事件交互。

## 3. 目录

程序本体（core、app、features）在 `main/`，可替换的适配层和第三方库各是一个组件，用 `REQUIRES` 写明依赖，没被依赖的组件不编译。

```text
firmware/main_board/
├── CMakeLists.txt  Kconfig  sdkconfig.defaults
├── main/
│   ├── main.c           只调 app_run()
│   ├── core/            duck.h bus.c motion.c arbiter.c policy.c（不用 malloc、文件和线程 API）
│   ├── app/             app.c api.c config.c policies.c calib.c sound.c feature.h event.h
│   └── features/        web gamepad voice xiaozhi gesture tof wifi
├── components/
│   ├── osal/            osal.h  linux/
│   ├── bus/             linux_uart.c  none.c
│   ├── storage/         posix.c
│   ├── media/           audio.h camera.h hand.h  v610_mpp.c  *_none.c
│   ├── asr/             mipk_wake.c  none.c
│   ├── net/ opus/ httpd/ pad/
│   └── cjson/ mbedtls/ vl53l5cx/
└── pc/                  PC 版 CMakeLists，用同一套 main 和 components
```

板子参数（舵机串口、引脚复用、总线方向脚、麦克风和扬声器增益、网页端口）在 Kconfig 的 `DoitDuck board` 菜单里，编译时确定，程序不带参数运行。

## 4. 适配层

### 4.1 接口

| 接口 | 主要函数 | 使用者 |
| --- | --- | --- |
| `osal.h` | `os_now` `os_sleep_until` `os_thread`（可指定 CPU 核）`os_lock` `os_device_id` | 全部 |
| `bus.h` | `bus_open` `bus_write` `bus_read` `bus_flush_input` | core |
| `storage.h` | `store_blob`（只读文件）`store_kv_get/set`（小设置）`store_put/get/remove/list`（用户文件） | core、app |
| `audio.h` | `audio_read` `audio_play` `audio_stream` `audio_flush` | voice、xiaozhi、叫声 |
| `camera.h` | `camera_h264_read` `camera_key_frame` `camera_jpeg` `camera_lines` | web、gesture、xiaozhi |
| `hand.h` | `hand_read`（21 个关键点） | gesture |
| `asr.h` | `asr_open` `asr_caps` `asr_feed` `asr_touch`，文字进、文字出 | voice |
| `net.h` | `net_https_post` `net_ws_connect/send_text/send_binary/close` | xiaozhi |
| `opus_codec.h` | `opus_codec_encode` `opus_codec_decode` | xiaozhi |
| `httpd.h` | `httpd_start(port, routes)`，`httpd_ws_send/recv/close` | web |
| `pad.h` | `pad_read`（摇杆和按键） | gamepad |

V610 的音频、摄像头和手势共用一个 MPP context，实现都在 `components/media/v610_mpp.c`；三个接口头文件和各自的 `none.c` 仍然分开。

### 4.2 选择实现

每个接口在 Kconfig 里是一个 `choice`，`sdkconfig.defaults` 选默认值，构建时只编译选中的文件。没有的能力选 `none`，`none` 实现返回 -1，相关功能据此不启动。

| 接口 | V610 | PC |
| --- | --- | --- |
| osal / storage / httpd | linux / posix / posix | 同左 |
| bus | linux_uart（含 GPIO 方向控制） | none |
| audio / camera / hand | v610_mpp | none |
| asr | mipk_wake（唤醒词和命令词，每个词一个 MicroWakeWord 模型） | none |
| net / opus | mbedtls / libopus | mbedtls / none |
| pad | none | none |

## 5. 功能模块

```c
struct feature {
    const char *name;                            /* robots/*.json 里的段名，也是 robot.status 里的字段名 */
    int  (*configure)(const cJSON *section);     /* 非 0 表示不启动（缺能力或被配置关闭） */
    void (*start)(void);                         /* 可选：启动自己的线程 */
    void (*status)(cJSON *r);                    /* 可选：robot.status 里自己的字段 */
    void (*on_event)(int ev, const char *text);  /* 可选 */
    void (*on_audio)(const int16_t *pcm, int n); /* 可选：麦克风数据（voice、xiaozhi） */
    const struct api_method *methods;            /* 可选：自己的指令 */
    int nmethods;
};
```

- `app.c` 里有一张按 Kconfig 条件编译的数组 `FEATURES[]`，启动时依次 `configure` → `start`。
- 麦克风由 app 统一读取，播放叫声期间和之后 0.15 秒不送给功能，避免听到自己。
- `robot.info` 返回编进来并成功启动的功能，网页据此隐藏不可用的页面和控件。

| 功能 | 依赖的接口 | 说明 |
| --- | --- | --- |
| web | httpd，camera 可选 | 网页、WebSocket 指令和状态推送、摄像头 H.264 |
| gamepad | pad | 手柄按键映射到指令（还没有读取实现） |
| voice | audio、asr | 唤醒词；命令词（带 `model` 的命令）不用唤醒随时执行；网页语音命令条按配置的命令表执行指令 |
| xiaozhi | audio、net、opus，唤醒来自 voice | 和小智服务器对话，MCP 工具控制动作 |
| gesture | camera、hand | 手势映射到指令 |
| tof | Linux I2C | VL53L5CX 8×8 测距，线程固定在 CPU1 |
| wifi | wpa_supplicant | 网页扫描、连接无线网络；热点配网 |

`wifi.ap` 通过 wlan0 上 `wpa_supplicant` 的全局控制接口（`-g /var/run/wpa_global`，`tools/board/S52wifi` 启动时带上）再建一个接口 `ap0` 作热点（2.4 GHz 信道 6，无密码，`192.168.4.1`），wlan0 的连接不受影响。需要 `wpa_supplicant` 带 AP 支持（Buildroot `BR2_PACKAGE_WPA_SUPPLICANT_AP_SUPPORT`）和网卡支持 STA + AP 并发（AIC8800 支持）。

- 热点线程自带 DHCP 和 DNS。DNS 把所有域名解析到 `192.168.4.1`，httpd 把从这个地址进来、`Host` 是别的域名的请求 302 到设置页，手机连上就会弹出页面（强制门户）。
- 热点开着时 `wifi.connect` 会先清掉 wlan0 的 IP，网页等到 `COMPLETED` 且有 IP 才弹窗，用户确认后发 `wifi.ap {on: false}` 关闭。
- 没有确认时，距离上次使用 10 分钟且 wlan0 在网上就自动关闭。`{always: true}` 常开，存在 `data/wifi_ap.txt`，开机打开。

事件用 `event_post(ev, text)` 广播，叫声和各功能按需接收：

| 事件 | 发出者 | 用途 |
| --- | --- | --- |
| `EV_WAKE` | voice | 唤醒叫声；小智说话时打断它 |
| `EV_WAKE_IDLE` | voice | 唤醒后开始小智对话 |
| `EV_POLICIES_CHANGED` | 动作库 | 刷新网页技能按钮和 MCP 工具描述 |
| `EV_COMMAND_OK` / `EV_COMMAND_REJECTED` | voice、gesture | 执行、拒绝的叫声 |
| `EV_LISTEN_TIMEOUT` | voice、xiaozhi | 结束聆听 |
| `EV_SKILL_DONE` / `EV_FALL` / `EV_LOW_BATTERY` / `EV_ESTOP` | app（根据机器人状态） | 对应叫声 |

## 6. 控制核心

- 总线线程（实时优先级 80，固定在 CPU0，和舵机串口中断同核）：每 10 ms 同步读 15 个舵机和 IMU 小板，做 IMU 滤波，上力时写目标位置。
- 动作线程（实时优先级 70）：每 20 ms 组观测、跑一次策略、平滑、换算成舵机目标。
- 模式有卸力、仅推理（跑策略但不上力，用来检查）、站好（保持默认站姿）、行走、标定、舵机设置。
- 行走中倾斜超过 `policy.fall.tilt_deg` 持续 `seconds` 判为摔倒，先保持默认站姿 `hold_s` 秒，再切到起身动作；倾斜小于 `policy.stand.tilt_deg` 持续 `seconds` 后回到行走。技能可以用 `hold_recovery` 关掉这个判断（如翻跟头）。
- 启动和上力时写舵机的临时 P/D 寄存器：全局 `kp` / `kd`，嘴 `mouth_kp`，`kp_by_id` 单独设某个舵机。
- 同一时刻只有一个控制来源。优先级手柄 > 网页 > 语音 > 小智 > 手势，高优先级可以随时接管；手柄和网页靠持续发指令保持控制，语音和小智的移动指令自带结束时间。停止和急停任何来源都有效。
- 保护：策略输出非数时卸力；动作线程 250 ms 没有新目标或 IMU 100 ms 没有数据时卸力；电量低于 `low_battery_v` 时拒绝上力。

## 7. 动作（策略）

### 7.1 网络与观测

只支持一种网络：MLP 61 → 512 → 256 → 128 → 14，ELU，50 Hz（Microduck 的格式）。观测布局：

| 下标 | 内容 |
| --- | --- |
| 0..3 / 3..6 | 陀螺仪、重力方向（机身坐标） |
| 6..20 / 20..34 / 34..48 | 14 个关节相对默认站姿的角度、速度、上一帧动作（不含嘴） |
| 48..51 | 命令槽，用法由 `command.encoding` 决定 |
| 51..55 | 头：颈俯仰、头俯仰、头偏航、头横滚 |
| 55..61 | 身体姿态命令，恒为 0 |

动作输出是相对默认站姿的关节角（弧度），乘倍率 1，平滑后加回默认站姿。动作顺序由机身配置的 `policy.action_ids` 决定，左右腿接哪些舵机就在这里对应。

### 7.2 manifest

每个动作 = 权重 + manifest，字段沿用 Microduck schema 2：

| `kind` | 谁结束它 | 例子 |
| --- | --- | --- |
| `perpetual` | 一直运行，直到切换 | 行走（`slot: walk`）、起身（`slot: stand`） |
| `episodic` | 运行 `duration_s` 秒后自己结束 | 翻滚、捡拾 |
| `scripted` | 定时，中途可以打断 | 坐下、站起 |

| `command.encoding` | 命令槽填什么 | 例子 |
| --- | --- | --- |
| `constant` | `command.idle`，默认全 0 | 翻滚 |
| `phase` | `[cos 2πφ, sin 2πφ, 0]`，φ 在 `period_s` 秒内从 0 到 1，到 `end_phase` 结束 | 捡拾 |
| `posture_flag` | vx 槽：1 = 坐，0 = 站 | 坐下、站起（`ramp_s`） |

DoitDuck 自己的字段放在 `doitduck` 子对象里：

| 字段 | 说明 |
| --- | --- |
| `label` | 显示名，`{"zh": ..., "en": ...}` |
| `alpha` | 动作平滑系数（旧值的权重）；不写时：manifest 里 `action_filter.enabled` 为 false 就不平滑，否则用网页设置的平滑 |
| `mouth_deg` / `mouth_until` | 动作开始时张嘴，进度到 `mouth_until` 时闭上 |
| `hold_recovery` | 运行中不判断摔倒 |
| `home_deg` | 这个动作训练时的默认站姿（每个舵机一个，度），和机身配置的不同时填 |

加载时校验 `model_api == 1`、`obs_len == 61`、`action_len == 14`、`action_scale == 1`、文件大小正确、`robot.model` 在机身配置的 `compatible_models` 里、`kind` 和 `encoding` 是已实现的组合。任何一项不对就拒绝，日志和网页给出错误码。

### 7.3 动作库

| 来源 | 位置 | 说明 |
| --- | --- | --- |
| 内置 | 机身配置 `policy.policies`，权重在 `models/` | 缺文件的跳过 |
| 导入 | `data/policies/<名字>.bin` / `.json` | 同名替换内置的；行走和起身按 `slot` 替换；删除后恢复内置的 |

最多 16 个。至少要有行走和起身两个，否则只能卸力、站好和预览。

导入流程：网页「动作」页选择 `policy.onnx` 和 `manifest.json`，浏览器里的 `web/js/onnx.js` 解析 ONNX（要求图为 Sub、Div、(Gemm、Elu)×3、Gemm），取出 mean/std 和各层权重排成 float32，校验 manifest，再调用 `policy.import`。板子再校验一遍后保存并加载，发出 `EV_POLICIES_CHANGED`。`tools/import_policy.py` 走同一个接口，也能只做转换。

- 导入的动作标为「未验证」，只能从网页触发；在网页上标为已验证（`policy.verify`）后语音、小智、手柄才能用。
- 替换行走、起身要二次确认，正在运行的动作不能替换或删除。
- 网页技能按钮、MCP 工具 `do_action` 的可选项、手柄 `skill:<名字>`、语音命令里的技能都从动作库生成或校验；配置引用了不存在的动作时跳过。

## 8. 统一指令

网页（WebSocket `/ws`）、语音、小智、手势、手柄都调用同一套方法，列在 `app/api.c` 和各功能的 `methods` 表里：

| 方法 | 归属 | 只允许网页 |
| --- | --- | --- |
| `robot.info / status / set_mode / move / stop / estop / skill / head / mouth / sound` | app | — |
| `robot.filter`、`servo.*`、`config.get` | app | ✓ |
| `calib.*` | app/calib | ✓ |
| `policy.list / import / remove / verify` | app/policies | ✓（`list` 除外） |
| `sound.list / upload / remove / settings` | app/sound | ✓（`list` 除外） |
| `voice.command / status / enable / wake` | voice | ✓ |
| `xiaozhi.status / mcp` | xiaozhi | ✓ |
| `gesture.control` | gesture | ✓ |
| `wifi.status / scan / connect` | wifi | ✓ |
| `wifi.ap` | wifi | — |
| `pad.status` | gamepad | ✓ |

错误返回 `{"code": "...", "message": "..."}`：`code` 是固定的英文标识，网页按它翻译；`message` 是英文说明，给脚本和日志看。

小智的 MCP 工具：`self.duck.get_status / stand / rest / walk / turn / stop / do_action / look / mouth / quack`，以及 `self.camera.take_photo`。

## 9. 网页

- 不需要构建工具，原生 JS（ES modules）+ CSS，改了直接刷新。
- 页面和控件按 `robot.info` 返回的功能列表显示。
- 手机用底部导航，宽屏用左侧导航；浅色、深色跟随系统；点击区域不小于 44 px。
- 一条 WebSocket `/ws` 收发 JSON-RPC 指令，状态每 100 ms 推送一次；摄像头画面走 `/video`。

| 页面 | 内容 |
| --- | --- |
| 控制 | 摄像头画面和手势骨架、模式、技能按钮、双摇杆、嘴、语音条（状态、最近一句、小智字幕、唤醒、打断） |
| 动作 | 动作库、导入、验证、删除 |
| 状态 | 姿态仪、舵机表、IMU、ToF、运行频率 |
| 维护 | 标定向导、舵机工具（扫描、改 ID、增益）、标定数据导出、导入、撤销 |
| 设置 | Wi-Fi、动作平滑、声音、小智、语音命令和手势一览、关于和许可 |

顶栏固定显示连接状态、模式、语言切换和急停按钮。缺零位标定、缺 IMU 水平时顶栏下方提示，点进去是标定向导。

翻译在 `web/i18n/zh.json`、`en.json`，HTML 用 `data-i18n="key"`，JS 用 `t(key, params)`；默认跟随浏览器，切换存在 `localStorage`。配置里的显示名（舵机名、动作名）写成 `{"zh": ..., "en": ...}`。`tools/i18n_check.py` 检查两份翻译的 key 是否一致。

```text
web/
├── index.html
├── css/        base.css（变量、浅色/深色）  layout.css  components.css
├── js/
│   ├── main.js        启动、路由、按功能显示页面
│   ├── rpc.js         WebSocket、重连、错误码
│   ├── i18n.js        翻译
│   ├── store.js       最新状态，页面订阅
│   ├── onnx.js        ONNX 解析和转换
│   ├── components/    joystick attitude diagram chart toast
│   └── pages/         control actions status maintain settings
├── i18n/       zh.json  en.json
└── vendor/     jmuxer.min.js（播放 H.264）
```

## 10. 标定

「维护 → 标定向导」四步，每步可以单独重做：

| 步骤 | 做什么 | 判断 |
| --- | --- | --- |
| 1. 舵机检查 | 逐个 ping 15 个舵机和 IMU 小板；可以扫描整条总线 | 列出缺少和多出的 ID |
| 2. IMU 零偏 | 静置 2 秒，IMU 小板测零偏 | 小板返回成功或失败 |
| 3. 零位 | 卸力手摆：把鸭子摆成标定姿势，一次或逐个记录；上力调整：舵机以小力保持当前位置，用滑块逐个转到零位再记录 | — |
| 4. 站姿 | 站好后测躯干横滚、俯仰和各关节跟踪误差，按 `calibration.roll_fix` 算出建议微调；可以手动 ± 微调；最后把当前姿态设为水平 | 横滚在 1° 以内 |

机身配置的 `calibration` 段：

```json
"calibration": {
  "pose_deg": [0, 0, ...],     // 标定姿势下各关节的角度（用夹具时填夹具对应的角度）
  "roll_fix": []               // 横滚超过 1° 时的建议微调：[[舵机下标, 每度横滚调多少度], ...]
}
```

标定数据存在 `data/calibration.json`（零位、IMU 水平、时间、机身名、各步骤是否完成），每次保存前把上一份存成 `calibration.prev.json`，网页可以撤销一次；也可以导出、导入。保存时先把数据写进闪存再替换文件，断电不会丢。

| 方法 | 说明 |
| --- | --- |
| `calib.enter` / `exit` | 进入时全部卸力；`{hold: true}` 时从当前位置以小力保持 |
| `calib.targets` / `finish` | 上力调整：设目标、记录一个关节 |
| `calib.capture` | 卸力手摆：记录全部或 `{index}` 一个 |
| `calib.imu` | 零偏、水平 |
| `calib.suggest` / `nudge` | 建议微调、应用微调 |
| `calib.mark` / `export` / `import` / `undo` | 步骤标记、标定数据 |

## 11. 叫声

| 名字 | 默认用在 |
| --- | --- |
| `quack` | 唤醒 |
| `quack_double` | 命令执行成功 |
| `quack_happy` | 完成技能 |
| `quack_question` | 等待命令超时 |
| `quack_angry` | 指令被拒绝、急停 |
| `quack_scared` | 摔倒 |
| `quack_sad` | 电量低 |
| `quack_sleepy` | 长时间没人互动 |
| `tick` | 网页按钮反馈 |

- 叫声由 `tools/gen_sounds.py` 合成（算法移植自 Microduck 官方 `sounds`，Apache-2.0），16 kHz 单声道 WAV，种子决定嗓音，默认 146。
- 网页可以上传自己的录音，存到 `data/sounds/`，同名替换内置叫声。
- 事件和叫声的对应、主动叫（站着没人互动时隔一段随机时间叫一声）在机身配置的 `sounds` 段。
- 播放叫声和小智说话时，嘴巴按声音每 20 ms 的响度张合（只在上力、没有技能占用嘴巴时）。
- 扬声器同一时刻只播一个声音：小智说话 > 事件叫声 > 主动叫和网页点的叫声。
- 代码在 `app/sound.c`，只依赖 `audio.h`；没有扬声器时静默跳过，`robot.sound` 返回 `no_audio`。

## 12. 数据存放

| 数据 | 例子 | 位置 |
| --- | --- | --- |
| 程序文件 | `doitduck`、`robots/`、`web/`、`sounds/` | 程序目录 |
| 模型 | 动作权重（用户提供）、唤醒和手势模型（仓库自带） | `models/` |
| 导入的动作 | `<名字>.bin` / `.json` | `data/policies/` |
| 用户的录音 | `<名字>.wav` | `data/sounds/` |
| 标定 | `calibration.json` 和上一份 | `data/` |
| 小设置 | 小智设备号、音量等 | `data/*.json`、`data/*.txt` |

更新程序只覆盖程序目录，不碰 `data/`。

## 13. 平台

V610 版：在 V610-Apps 根目录 `sh tools/build.sh projects/robot/doitduck/firmware/main_board`，产物在 `firmware/main_board/out/`。

PC 版：`cmake -S firmware/main_board/pc -B build/pc && cmake --build build/pc`，运行 `./doitduck`。没有总线、音频和摄像头，网页、指令、仲裁、MCP、动作导入都能测试；`tools/ci.sh` 编译它并检查翻译。
