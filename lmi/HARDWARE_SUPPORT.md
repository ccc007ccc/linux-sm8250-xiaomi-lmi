# lmi 硬件支持状态

> 100% AI 编写：本文档由 AI 生成和整理。

本表记录 Redmi K30 Pro / POCO F2 Pro（`lmi`）在当前主线 Linux 适配中的硬件状态。状态只按已经在本项目镜像或主线代码路径中实机验证过的结果标记；未验证或只存在 Android downstream 参考的项目不视为已支持。

更长的 bring-up 记录、warning 解释和调制解调器诊断过程见 [`ADAPTATION_NOTES.md`](ADAPTATION_NOTES.md)。使用和编译流程见 [`docs/`](docs/)。

## 状态标记

- 已支持：当前主线镜像已验证基础功能可用。
- 部分支持：核心链路可用，但功能不完整、依赖额外输入，或仍缺关键场景验证。
- 待适配：硬件已知，但当前主线镜像尚未接入或尚未实机验证。
- 暂不支持：当前阶段不计划支持，或主线路径缺失较多。

## 快速矩阵

| 组件 | 状态 | 当前结论 | 主要限制 / 注意 |
| --- | --- | --- | --- |
| UFS / rootfs | 已支持 | UFS 枚举、分区扫描和 UFS rootfs 启动已验证；当前推荐 Ubuntu 26.04 Server arm64 rootfs 放在 `/dev/sda34`，标签 `ubuntu-rootfs`。 | rootfs 必须有 `/etc/os-release` 和 `/sbin/init`；boot image 不内置完整发行版系统。 |
| 屏幕 / DRM | 已支持 | Samsung AMS667UU01 面板已验证 fbcon、DRM/KMS、背光和横向控制台；当前暴露 60Hz 与 77Hz 两个 1080x2400 mode。 | HBM、FOD HBM、DC dimming、doze、完整 ESD 恢复等 Android 厂商扩展未作为主线基础功能接入。 |
| GPU | 已支持 | Adreno A650 / GMU / freedreno 已验证 `/dev/dri/renderD128`、GBM/EGL/GLES 和 KMS 渲染。 | 需要设备可接受的 A650 firmware / zap 段；这些 blob 只能放本地 ignored firmware 路径，不提交到源码仓库。 |
| 触摸屏 | 已支持 | FocalTech FT3518 已走硬件 `i2c13` / GENI I2C，基础多点触控事件可用。 | 高级手势、固件管理和专用参数未作为当前目标。 |
| 实体按键 | 已支持 | 电源键、音量上、音量下均按标准 input key 上报；调试环境可用音量键调背光、电源键切背光。 | 背光/锁屏策略属于用户态策略，内核只提供标准 key code。 |
| Wi-Fi | 已支持 | QCA6391 Wi-Fi 已验证 `ath11k_pci`、真实 WLAN MAC、自动连接和 SSH。 | 需要设备匹配 firmware；Android/发行版网络策略仍可能影响自动连接。 |
| 蓝牙 | 已支持 | QCA6391 UART Bluetooth 已验证固件加载、public address 设置、BR/EDR 与 LE 基础能力。 | 若未来找到独立原厂 BT address 来源，可替换当前支持层设置策略。 |
| USB / Type-C | 部分支持 | DWC3 gadget、USB ACM 串口、Type-C 基础枚举和标准 PD sink 已验证；标准 PD 曾验证到 9V/2A。 | USB host/OTG、完整角色切换和更多 PD 场景仍需继续验证。 |
| 电池 / 充电 | 部分支持 | PM8150B Type-C/TCPM、SMB5 charger 和 gen4 fuel-gauge 已能暴露 USB 输入、电池容量、电压、电流、温度和状态。 | Xiaomi 33W 私有快充、BQ2597x 充电泵、限充/停充策略、完整电池曲线和旁路供电控制未接入。 |
| 主扬声器 | 已支持 | `nxp,tfa9874` 主扬声器路线已验证可播放 48 kHz S16_LE stereo 测试音。 | NXP/Goodix 专用 DSP/profile/calibration 与安全音量策略仍待完善；测试应先用低振幅短音。 |
| 听筒 | 已支持 | WCD9380 RX / EAR 路线已验证真实听筒播放。 | 音量曲线和普通播放器默认参数仍需继续打磨。 |
| 3.5mm HPH | 部分支持 | WCD9380 HPHL/HPHR 播放、耳机插入状态和阻抗读取已验证。 | 当前没有耳麦测试环境；3.5mm 耳麦麦克风未验证。 |
| 机身麦克风 | 部分支持 | WCD9380 TX capture 已验证，底部麦克风和顶部麦克风可短录音并回放确认。 | 第三个机身麦克风尚未定位，常规录音栈和降噪策略未完成。 |
| Docker / 容器内核能力 | 已支持 | release 配置已验证 overlayfs、cgroup v2、bridge/NAT、端口映射、nftables/iptables 和常见 Docker 运行路径。 | macvlan/ipvlan 可创建，但 Wi-Fi managed client 模式不等同于 Unraid 有线 `br0`，独立 LAN IP 需要单独验证。 |
| 调制解调器 / 蜂窝 | 部分支持（诊断） | SDX55M PCIe/MHI/SBL/Sahara 诊断已推进到多阶段 image transfer 研究，但仍停在 SBL/Sahara/MHI 阶段，未进入 Mission/AMSS。 | SIM、蜂窝数据、语音、IMS/VoLTE、ModemManager 均不可用；当前 release 不把蜂窝作为可用功能宣传，详细历史见 `ADAPTATION_NOTES.md`。 |
| NFC | 待适配 | downstream 显示为 NQ NCI I2C 设备。 | 未接入 NCI/I2C 设备节点和用户态验证。 |
| 传感器 | 待适配 | 需要 SDSP remoteproc、签名 SDSP firmware 和传感器用户态栈。 | 当前未启用，不作为 release 基础能力。 |
| 触觉反馈 | 待适配 | 硬件为 AW8697 类 haptics。 | 未接入 haptics/input/ff 测试。 |
| 闪光灯 LED | 待适配 | PM8150L SPMI flash LED 硬件已知。 | 未接入 LED class 测试。 |
| SBA-MUX / 模拟附件 | 待适配 | FSA4480 可能用于 USB-C analog/audio accessory mux。 | 当前 USB gadget/Type-C 基础链路不依赖它。 |
| 充电泵 / 私有快充 | 暂不支持 | BQ2597x 充电泵和 Xiaomi 私有快充不是当前阶段目标。 | 服务器使用目标优先稳定普通供电与可见电池状态。 |

## 已验证的基础组合

- 启动链路：copydown bootshim、Linux Image.gz、runtime DTB、内嵌 initramfs 和 UFS rootfs 可组合启动。
- 控制台链路：UFS rootfs、DSI fbcon、USB ACM、Wi-Fi SSH 可同时工作。
- 输入显示：触摸、电源/音量键、DRM/KMS/backlight 可同时工作。
- 图形链路：Adreno A650 / GMU / freedreno 可用于 GBM/EGL/GLES 渲染。
- 电源链路：PM8150B Type-C/TCPM、charger 和 fuel-gauge 可同时暴露 USB 输入与电池状态。
- 音频链路：ADSP/PDR/QRTR/APR、SM8250 sound card、TFA9874 主扬声器、WCD9380 听筒、3.5mm HPH 播放、MBHC 插入/阻抗读取和两路机身麦克风基础录音已验证。
- 服务器链路：Ubuntu 26.04 Server rootfs、SSH、Docker bridge/NAT、端口映射、overlayfs、cgroup v2、nftables/iptables 已通过基础验证。

## 电池 / 充电当前目标

lmi 作为长期运行的小型服务器使用时，优先目标是稳定供电、电池状态可见、温度/安全信息可见。当前普通 Type-C/PD 路径能稳定供电即可作为基线；33W Xiaomi 私有快充、BQ2597x 充电泵和可能的硬件旁路供电控制需要更多私有策略或硬件验证，不作为当前 release 必备功能。

Linux 没有通用“电源直供/绕过电池”开关；这类能力必须由 PMIC/charger 硬件和对应驱动暴露。当前 PM8150B 普通 charger 路径只确认了输入、电池和充电状态可见。

## 调制解调器当前结论

SDX55M 已经证明 PCIe2、MHI、signed SBL、Sahara HELLO/HELLO_RESP 和部分 image transfer 诊断链路可运行，但它仍没有进入 Mission/AMSS，也没有可用 QRTR/QMI、SIM、蜂窝数据、语音或 IMS/VoLTE。当前发布说明中应把 modem 标记为不可用或仅诊断阶段，不应把它写成日常蜂窝功能。

继续推进 modem 时，应从 [`ADAPTATION_NOTES.md`](ADAPTATION_NOTES.md) 的 `Modem / SDX55M / X55 5G` 章节恢复上下文，并继续遵守边界：不进入 EDL，不发送 firehose，不写 NV/modem/dtbo/recovery/vbmeta，不提交 firmware blob。
