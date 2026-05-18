# lmi 硬件支持状态

本表记录 Redmi K30 Pro / POCO F2 Pro（lmi）在当前主线 Linux 适配中的硬件状态。状态只按已经在本项目镜像或主线代码路径中验证过的结果标记；未验证或只存在 Android downstream 参考的项目不视为已支持。

## 状态标记

- 已支持：当前主线镜像已验证基础功能可用。
- 部分支持：核心链路可用，但功能不完整或仍缺关键场景验证。
- 待适配：硬件已知，但当前主线镜像尚未接入。
- 暂不支持：当前阶段不计划支持，或主线路径缺失较多。

## 功能

| 组件 | 型号 / compatible | 连接 / 总线 | 当前状态 | 备注 |
| --- | --- | --- | --- | --- |
| 屏幕 | `samsung,ams667uu01` | DSI0 / MDSS DPU | 已支持 | 已验证 fbcon、DRM/KMS、背光、横向控制台；当前暴露 60Hz 和 77Hz 两个 1080x2400 mode。 |
| 电池 | `qcom,pm8150b-fg` / downstream `qcom,fg-gen4` | PM8150B SPMI | 部分支持 | 已验证 `qcom-battery` 暴露容量、电压、电流、温度和 present；设计参数来自 `simple-battery`，设备专用电池曲线和完整健康策略仍待补齐。 |
| 充电器 | `qcom,pm8150b-charger` / downstream `qcom,qpnp-smb5` | PM8150B SPMI / Type-C | 部分支持 | 已验证 `pm8150b-charger` power_supply、USB online/status/health 和 `usbin_v/usbin_i` IIO 输入电压/电流；私有快充、限充和旁路供电控制未接入。 |
| 充电泵 | `ti,bq2597x-standalone` | `i2c15 @ 0x66` / charger pump | 暂不支持 | Android 用作 `pm8150b-charger` 的泵，IRQ 为 GPIO157；主线没有 `ti,bq2597x-standalone` compatible，且 33W Xiaomi 私有快充因缺少官方线和测试环境暂缓。 |
| 内部存储 | `jedec,ufs-2.0` / `qcom,sm8250-qmp-ufs-phy` | UFS / QMP PHY | 已支持 | 已验证 UFS 枚举、分区扫描、Ubuntu rootfs 从 `/dev/sda34` 启动。 |
| 触摸屏 | `focaltech,ft3518 @ 0x38` | `i2c13` | 已支持 | 已验证硬件 GENI I2C 路径、`/dev/input/event0`、多点触控基础事件。 |
| GPU | `qcom,adreno-650` | MSM DRM / GMU / Adreno SMMU | 已支持 | 已验证 `/dev/dri/renderD128`、Mesa freedreno `FD650`、GLES/GBM/KMS 测试；需要 `a650_sqe.fw`、`a650_gmu.bin` 和 lmi stock `a650_zap.mdt` + bXX 段。 |
| SBA-MUX | `fcs,fsa4480 @ 0x42` | `i2c15` | 待适配 | 可能用于 USB-C analog/audio accessory mux；当前 USB gadget/Type-C 基础链路不依赖它。 |
| 闪光灯 LED | `qcom,spmi-flash-led` | PM8150L SPMI | 待适配 | 未接入 LED class 测试。 |
| Wi-Fi | `qca6391` | PCIe / MHI / ath11k | 已支持 | 已验证 `ath11k_pci`、真实 WLAN MAC、自动连接和 SSH；需要 unsigned ath11k firmware。 |
| 蓝牙 | `qca6391` | UART6 / QCA HCI | 已支持 | 已验证 QCA 固件加载、public address 设置、BR/EDR 与 LE 基础能力。 |
| NFC | `qcom,nq-nci @ 0x28` | `i2c1` | 待适配 | 未接入 NCI/I2C 设备节点和用户态验证。 |
| 调制解调器 | `sdx55m` | PCIe / remote stack | 暂不支持 | 当前主线内核不支持完整 SDX55m 手机 modem 数据/语音链路。 |
| USB OTG | `usb-c-connector` / PM8150B Type-C | DWC3 + PM8150B Type-C | 部分支持 | 已验证 USB ACM gadget 调试、Type-C 基础枚举和标准 PD sink；M14 在 PD 电源上协商到 9V/2A，主机 OTG 和角色切换仍待继续验证。 |
| 传感器 | `hexagonrpcd` / `libSSC` | SDSP remoteproc | 待适配 | 需要 SDSP remoteproc、签名 SDSP firmware 和传感器用户态栈；当前未启用。 |
| 触觉反馈 | `awinic,aw8697` | `i2c11` | 待适配 | 未接入 haptics/input/ff 测试。 |

## 音频

| 组件 | 用途 | 连接 / 总线 | 当前状态 | 备注 |
| --- | --- | --- | --- | --- |
| `ncp,tfa9874` | EAR speaker | `i2c3 @ 0x34` | 待适配 | 未接入 ASoC codec / amplifier。 |
| `ncp,tfa9874` | Main speaker | `i2c3 @ 0x34` | 待适配 | 地址/双功放拓扑需要对照 Android downstream。 |
| `qcom,wcd9380-codec` | Chassis microphones x3 | SoundWire / WCD938x | 待适配 | 需要 SM8250 audio graph、SoundWire、APR/LPASS/WCD codec 路径。 |
| `qcom,wcd9380-codec` | Analog I/O audio port | SoundWire / WCD938x | 待适配 | 与 USB-C analog path / FSA4480 可能有关。 |

## 已验证的基础组合

- 控制台启动：UFS rootfs、DSI fbcon、USB ACM、Wi-Fi SSH 可同时工作。
- 输入显示：FT3518 触摸和 AMS667UU01 DRM/KMS 可同时工作。
- GPU：Adreno A650 / GMU / freedreno 可用于 GBM/EGL/GLES 渲染。
- 电源：PM8150B Type-C/TCPM、charger 和 fuel-gauge 普通路线可同时暴露 USB 输入与电池状态。
- 无线：Wi-Fi 使用真实 WLAN MAC，Bluetooth public address 使用 WLAN MAC + 1 的当前支持策略。

## 电池 / 充电当前结论

当前主线普通路线已接通 PM8150B Type-C/TCPM/VBUS、SMB5 charger 和 gen4 fuel-gauge。`/sys/class/power_supply` 已能看到 `pm8150b-charger`、`qcom-battery` 和 TCPM USB supply；M14 在标准 PD 电源上验证过 9V/2A，M15 验证 charger 在 USB online 时能通过 IIO 上报输入电压/电流。

仍未接入的是 Xiaomi 33W 私有快充、BQ2597x 充电泵、设备专用电池曲线、限充/停充策略和可能的硬件旁路供电控制。这些功能依赖更多私有策略或硬件能力，当前服务器使用目标先以稳定普通供电和电池状态可见为基线。

## 当前电源目标

lmi 作为长期运行服务器使用时，优先目标是稳定供电、电池状态可见、温度/安全信息可见。33W Xiaomi 私有快充和 BQ2597x 充电泵不是当前阶段目标；主线 Type-C/PD 路径能稳定协商 9V/2A 约 18W 即可。若只能以较低功率稳定供电，也先作为可接受基线记录，再逐步完善。Linux 没有通用“电源直供/绕过电池”开关；这类能力必须由 PMIC/charger 硬件和对应驱动暴露，当前 PM8150B 普通 charger 路径只确认了输入、电池和充电状态可见。
