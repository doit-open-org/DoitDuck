# 第三方组件

| 组件                                | 位置                                                                                                                                            | 许可证                                                                                                                                        | 用途                           |
| ----------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------ |
| cJSON                               | `firmware/main_board/components/cjson/`                                                                                                       | MIT（见目录里的 LICENSE）                                                                                                                     | JSON                           |
| Mbed TLS 3.6.4                      | `firmware/main_board/components/mbedtls/`                                                                                                     | Apache-2.0 或 GPL-2.0-or-later（见目录里的 LICENSE）                                                                                          | 小智的 HTTPS 和 WebSocket      |
| ST VL53L5CX Ultra Lite Driver 1.3.9 | `firmware/main_board/components/vl53l5cx/`（`vl53l5cx_api.c/.h`、`vl53l5cx_buffers.h` 原样；`platform.c/.h` 是我们写的 Linux I2C 适配） | BSD-3-Clause（见目录里的 LICENSE.md），来源 [STMicroelectronics/stm32-vl53l5cx](https://github.com/STMicroelectronics/stm32-vl53l5cx)          | ToF 传感器驱动（含传感器固件） |
| jmuxer 2.0.5                        | `web/vendor/jmuxer.min.js`                                                                                                                    | MIT（见 `web/vendor/jmuxer.LICENSE.txt`）                                                                                                   | 网页里播放摄像头的 H.264       |
| Microduck `sounds` 声音合成器     | `tools/gen_sounds.py`（从 Rust 移植到 Python，改动写在文件开头）                                                                              | Apache-2.0（见 `tools/gen_sounds.LICENSE`），来源 [pollen-robotics/microduck](https://github.com/pollen-robotics/microduck/tree/main/sounds) | 合成 `sounds/` 里的鸭子叫声  |

不在本仓库里、构建或运行时由用户另外提供的：

| 组件                                     | 说明                                                 |
| ---------------------------------------- | ---------------------------------------------------- |
| V610-App-SDK（hifw、media_engine、mipk） | 编译 Cove-610 版需要，许可证见 SDK                   |
| 动作模型                                 | 放在 `models/`，或在网页上导入；许可证见各自的来源 |

`models/nihaoxiaoya.mipk`、`models/kaishipeiwang.mipk` 是本项目自己训练的语音模型，许可和本仓库相同。

manifest 的格式沿用 Microduck 的 `policy-manifest.md`（schema 2），本仓库只读写这个格式。叫声（`sounds/`）由 `tools/gen_sounds.py` 合成，没有使用任何录音；合成算法移植自 Microduck 的 `sounds`（Apache-2.0，见上表）。
