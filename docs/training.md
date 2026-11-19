# 训练动作模型

DoitDuck 运行 Microduck 格式的动作模型。机身和舵机与 [Microduck-kissqy](https://github.com/kissqy/Microduck-kissqy)（飞特 HD1910）相同，训练参考它的训练控制台；控制台基于 [pollen-robotics/microduck_rl](https://github.com/pollen-robotics/microduck_rl)。

注意：原版 Microduck 使用 Dynamixel XL330，`microduck_rl` 的执行器是 XL330 的 BAM 模型（M6，固件 P 200）；HD1910 运行时的 P 为 5，两者不能直接对应。用官方执行器训练的模型在 HD1910 上关节可能跟不上，训练前要换成 HD1910 的执行器参数（见第 3、4 节）。

## 1. 模型要满足的条件

| 项目     | 要求                                                                                        |
| -------- | ------------------------------------------------------------------------------------------- |
| 网络     | MLP 61 → 512 → 256 → 128 → 14，ELU，归一化（mean / std）包含在 ONNX 里                  |
| 频率     | 50 Hz                                                                                       |
| 观测     | 陀螺仪、重力方向、14 个关节角度、速度、上一帧动作、3 个命令、4 个头部命令、6 个身体姿态命令 |
| 动作     | 14 个关节相对默认站姿的角度（弧度），倍率 1                                                 |
| 关节顺序 | 左腿 5 个、颈和头 4 个、右腿 5 个；接哪个舵机由机身配置的 `policy.action_ids` 决定        |
| manifest | Microduck schema 2，`robot.model` 要在机身配置的 `policy.compatible_models` 里          |

`microduck_rl` 和 Microduck-kissqy 训练控制台导出的模型都满足这些条件。

## 2. 环境

需要 NVIDIA GPU 和 CUDA（训练用 MuJoCo Warp，Linux 或 WSL2）、git 和 [uv](https://docs.astral.sh/uv/)。

**训练控制台（推荐）**：Microduck-kissqy 的 `microduck-hd1910/training-console/`（Microduck Training Studio），用网页界面选任务、训练和导出。用 `start-training.cmd` 或 `start-training.command` 启动，浏览器打开 `http://127.0.0.1:8092`，详见它的 README。

**命令行**：直接用 `microduck_rl`，固定到控制台所用的版本：

```sh
git clone https://github.com/pollen-robotics/microduck_rl
cd microduck_rl && git checkout 8d0db74916a4f833d1d9b95d6a1d7f4d13b9d5ec
uv sync                                      # 第一次下载 CUDA 依赖较大，可设 UV_HTTP_TIMEOUT=600
uv run train Mjlab-Velocity-Flat-MicroDuck --env.scene.num-envs 4096 --agent.logger tensorboard
```

- 不用 wandb 时加 `--agent.logger tensorboard`，日志在 `logs/rsl_rl/<任务>/`。
- 显存不够时减少 `--env.scene.num-envs`。参考：12 GB 显存跑行走类任务用 2048 个环境，起身类任务需要的显存少得多。
- 训练时间长，放在 `tmux` 里运行，SSH 断开不影响。

## 3. 测量实机参数

这个尺寸的机器人，仿真和实机的差距主要来自舵机。训练前先量这些：

| 项目           | 方法                                                                                                                         | 用在                              |
| -------------- | ---------------------------------------------------------------------------------------------------------------------------- | --------------------------------- |
| 整机和头部质量 | 称重                                                                                                                         | 仿真质量                          |
| 舵机模型       | 用 [BAM](https://github.com/Rhoban/bam) 摆锤台架辨识 HD1910，舵机 P（DoitDuck 默认 `motors.kp: 5`）和供电电压与实际使用一致 | 执行器模型                        |
| 指令延迟       | 写目标到读回位置开始变化的时间                                                                                               | 延迟随机化范围                    |
| 齿隙           | 卸力后来回扳关节，读编码器空程                                                                                               | 齿隙随机化（`*-Backlash` 任务） |
| 电压           | 满电和带载时的电池电压                                                                                                       | 电压随机化范围                    |
| IMU            | 静置时的噪声，站立时的重力方向                                                                                               | 观测噪声和安装偏差                |

验收：单个关节的阶跃响应，仿真和实机曲线基本重合。

## 4. 训练配置

在自己的分支里只改和机身有关的部分，任务、奖励、观测保持原样，这样 61 维观测、14 维动作、50 Hz 都不变：

- 骨架：机身和官方 Microduck 骨架一致时直接用官方的 `robot_*.xml`，只改质量；自己设计的结构导出自己的 MJCF（如用 onshape-to-robot）。
- 执行器：把 `src/mjlab_microduck/robot/microduck_constants.py` 里的 XL330 BAM 模型换成 HD1910 的辨识结果，电压、延迟、摩擦的随机化范围用实测值。
- 默认站姿：和机身配置的 `motors.home_deg` 一致；不一致时在动作的 manifest 里用 `doitduck.home_deg` 写训练时的站姿。

验收：`uv run list-envs` 能看到自己的任务，并能开始训练。

## 5. 任务

| 任务                    | DoitDuck 里的用途 | 说明                                  |
| ----------------------- | ----------------- | ------------------------------------- |
| `Velocity-*-Backlash` | 行走              | 只会走，不会起身                      |
| `StandUp-*-Backlash`  | 起身              |                                       |
| `VelStand-*-Backlash` | 行走 + 倒地恢复   | 一个模型同时用作行走和起身            |
| `GroundPick-*`        | 捡东西            | `phase` 编码，周期 4 s              |
| `Roulade-Flat`        | 翻跟头            | 运行中不判断摔倒（`hold_recovery`） |
| `BallKick*`           | 踢球              |                                       |
| `SitStand-*`          | 坐下、站起        |                                       |

`VelStand` 不是从零训练的：官方配置从一个行走检查点热启动，并把起身模型当老师做蒸馏，行走检查点同时当锚点。要得到完全自己训练的模型，先自己训 `Velocity-*-Backlash` 和 `StandUp-*-Backlash`，再把热启动、老师、锚点的检查点路径都指向自己的，并确认不会从网上下载别人的检查点。其余任务没有热启动或蒸馏。

中断后可以从自己的检查点续训：`--agent.load-checkpoint model_XXXX.pt --agent.resume True`。

## 6. 验收

### 仿真

- `uv run scripts/infer_policy.py --walking walk.onnx --standing ...` 在 CPU MuJoCo 里复跑，检查换一个仿真器还能不能走。
- 用 `--vin`、`--kp-fw` 固定到实测最差的电压和增益再跑。
- 标准：平地走 60 秒不摔；推一下能恢复；摔倒后 5 秒内站起；速度指令跟得上。

### 实机

1. 导出 ONNX 和 manifest，转换后放进 `models/` 或在网页上导入（见下一节）。
2. 依次检查：「仅推理」模式下手扶着看输出是否合理 → 悬空上力看左右是否对称、有没有抖 → 落地站好 → 慢走（从 0.1 m/s 开始，再加转向、侧移）。
3. 和仿真对不上时，回到第 3、4 节修正参数重新训练；不要靠运行时加平滑或改倍率掩盖。

## 7. 放到 DoitDuck 上

```sh
uv run scripts/export.py <任务> --checkpoint-file logs/rsl_rl/<实验>/<运行>/model_XXXX.pt --onnx-file policy.onnx
python3 tools/import_policy.py policy.onnx manifest.json --out models/<名字>.bin   # 在 DoitDuck 仓库里
```

`manifest.json` 按 [Microduck schema 2](https://github.com/pollen-robotics/microduck/blob/main/docs/policy-manifest.md) 写，`microduck_rl` 的 `uv run publish` 发布模型时会生成一份，也可以照现有模型的 manifest 改。

训练时没有动作平滑（`microduck_rl` 和 Microduck-kissqy 都不加），部署时把机身配置的 `policy.smoothing` 设为 0，或在 manifest 里写 `doitduck.alpha: 0`。

然后在机身配置的 `policy.policies` 里加一项（文件名、`name`、`kind`、`slot` 或 `duration_s`、`command` 等，写法见现有配置），或者直接在网页「动作」页导入。manifest 的 `robot.model` 要在 `policy.compatible_models` 里。
