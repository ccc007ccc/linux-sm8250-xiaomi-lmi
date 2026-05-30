# lmi 硬件支持状态

> 100% AI 编写：本文档由 AI 生成和整理。

本表记录 Redmi K30 Pro / POCO F2 Pro（`lmi`）在当前主线 Linux 适配中的硬件状态。状态只按已经在本项目镜像或主线代码路径中实机验证过的结果标记；未验证或只存在 Android downstream 参考的项目不视为已支持。

更长的 bring-up 记录和 warning 解释见 [`ADAPTATION_NOTES.md`](ADAPTATION_NOTES.md)。摄像头适配记录见 [`CAMERA_BRINGUP.md`](CAMERA_BRINGUP.md)。调制解调器诊断过程见 [`MODEM_BRINGUP.md`](MODEM_BRINGUP.md)。使用和编译流程见 [`docs/`](docs/)。

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
| 视频编解码 / Venus | 部分支持（bring-up） | 已按 upstream `qcom,sm8250-venus` 路径在 lmi DTS 接入 Venus codec；当前配置保留 `CONFIG_VIDEO_QCOM_VENUS=m`，实机 bring-up 曾在有匹配模块/firmware 的环境下验证 `/dev/video14` decoder、`/dev/video15` encoder 枚举；encoder 接受 NV12 输入并枚举 H.264/VP8/HEVC 输出；generated NV12 和 OV13B10 RAW 派生的软件 NV12 均已通过 `/dev/video15` 编码为真实 H.264 Annex-B 码流，输出含 SPS/PPS start code。 | `lmi/scripts/build-kernel.sh` 只产 `Image.gz` + dtb、不构建/安装模块，因此 Image.gz+dtb-only 启动不保证 Venus 可用；需要测试环境额外提供匹配的 Venus 模块和本地 ignored stock Venus firmware（当前 DTS 查找 `qcom/sm8250/xiaomi/lmi/venus.mbn`），firmware blob 不提交。当前只验证 H.264 编码，H.265/VP8 和实际应用编码路径仍需继续测；RAW 派生 NV12 是 helper 软件转换结果，这不代表相机已有内核 ISP/YUV 输出或浏览器 `getUserMedia`。 |
| 触摸屏 | 已支持 | FocalTech FT3518 已走硬件 `i2c13` / GENI I2C，基础多点触控事件可用。 | 高级手势、固件管理和专用参数未作为当前目标。 |
| 实体按键 | 已支持 | 电源键、音量上、音量下均按标准 input key 上报；调试环境可用音量键调背光、电源键切背光。 | 背光/锁屏策略属于用户态策略，内核只提供标准 key code。 |
| Wi-Fi | 已支持 | QCA6391 Wi-Fi 已验证 `ath11k_pci`、真实 WLAN MAC、自动连接和 SSH。 | 需要设备匹配 firmware；Android/发行版网络策略仍可能影响自动连接。 |
| 蓝牙 | 已支持 | QCA6391 UART Bluetooth 已验证固件加载、public address 设置、BR/EDR 与 LE 基础能力。 | 若未来找到独立原厂 BT address 来源，可替换当前支持层设置策略。 |
| USB / Type-C | 部分支持 | DWC3 gadget、USB ACM 串口、Type-C 基础枚举和标准 PD sink 已验证；标准 PD 曾验证到 9V/2A。 | USB host/OTG、完整角色切换和更多 PD 场景仍需继续验证。 |
| 电池 / 充电 | 部分支持 | PM8150B Type-C/TCPM、SMB5 charger 和 gen4 fuel-gauge 已能暴露 USB 输入、电池容量、电压、电流、温度和状态；当前代码提供 `charge_behaviour`、`input_current_limit` 目标值、`current_max` 有效值和 `lmi-power` 保守限充服务，默认 70% 停充、65% 恢复，停充状态和策略输出已实机核对。 | 仍需更长时间观察容量/温度保持；Xiaomi 33W 私有快充、BQ2597x 充电泵、完整电池曲线和硬件旁路供电控制未接入。 |
| 主扬声器 | 已支持 | `nxp,tfa9874` 主扬声器路线已验证可播放 48 kHz S16_LE stereo 测试音。 | NXP/Goodix 专用 DSP/profile/calibration 与安全音量策略仍待完善；测试应先用低振幅短音。 |
| 听筒 | 已支持 | WCD9380 RX / EAR 路线已验证真实听筒播放。 | 音量曲线和普通播放器默认参数仍需继续打磨。 |
| 3.5mm HPH | 部分支持 | WCD9380 HPHL/HPHR 播放、耳机插入状态和阻抗读取已验证。 | 当前没有耳麦测试环境；3.5mm 耳麦麦克风未验证。 |
| 机身麦克风 | 部分支持 | WCD9380 TX capture 已验证，底部麦克风和顶部麦克风可短录音并回放确认。 | 第三个机身麦克风尚未定位，常规录音栈和降噪策略未完成。 |
| Docker / 容器内核能力 | 已支持 | release 配置已验证 overlayfs、cgroup v2、bridge/NAT、端口映射、nftables/iptables 和常见 Docker 运行路径。 | macvlan/ipvlan 可创建，但 Wi-Fi managed client 模式不等同于 Unraid 有线 `br0`，独立 LAN IP 需要单独验证。 |
| 调制解调器 / 蜂窝 | 部分支持（诊断） | SDX55M PCIe/MHI/SBL/Sahara 诊断已推进到多阶段 image transfer 研究，但仍停在 SBL/Sahara/MHI 阶段，未进入 Mission/AMSS。 | SIM、蜂窝数据、语音、IMS/VoLTE、ModemManager 均不可用；当前 release 不把蜂窝作为可用功能宣传，详细历史见 [`MODEM_BRINGUP.md`](MODEM_BRINGUP.md)。 |
| NFC | 待适配 | downstream 显示为 NQ NCI I2C 设备。 | 未接入 NCI/I2C 设备节点和用户态验证。 |
| 传感器 | 待适配 | 需要 SDSP remoteproc、签名 SDSP firmware 和传感器用户态栈。 | 当前未启用，不作为 release 基础能力。 |
| 触觉反馈 | 待适配 | 硬件为 AW8697 类 haptics。 | 未接入 haptics/input/ff 测试。 |
| 闪光灯 LED | 待适配 | PM8150L SPMI flash LED 硬件已知。 | 未接入 LED class 测试。 |
| 摄像头 | 部分支持（bring-up） | 第一阶段只接入后置超广角 OV13B10；CAMSS/CCI power-domain 问题已通过内置 SM8250 CAMCC 修复，OV13B10 已在 `&cci0_i2c1` 地址 `0x10` probe 成功，当前已通过标准 media/V4L2 ioctl 路线跑通 `/dev/video3` raw Bayer `pgAA` stream，驱动暴露的 6 个 4-lane 模式（4208x3120/4160x3120/4160x2340/2104x1560/2080x1170/1364x768）均已实机 raw capture 验证；frame interval、crop selection、orientation/rotation、media route、video querycap、控制项扩展元数据（含 `Unit Cell Size` 1120nm x 1120nm）、video frame-size probing 和按 OV13B10 RAW10 media-bus code 收敛后的 format enumeration 等标准发现信息可查询。`lmi-camera-web-preview.py` 是不改 rootfs 的 raw-camera 支持工具，提供稳定 `lmi.raw-camera.discovery.v1` 发现 JSON、彩色 PNG/HTTP multipart 预览、`/snapshot`/`/raw`/`/metadata` Web 导出端点、`/capabilities` HTTP 能力发现、raw frame/metadata sidecar 导出、percentile AE、过滤 AWB 和 preview CCM/gamma 软件处理；同时已接入用户态 C software-ISP（`lmi-isp`）+ v4l2loopback/UVC gadget，可把 RAW 转成独立标准 YUYV/NV12 节点，实测 2080x1170→1280x720 YUYV 约 29fps，Windows UVC MediaCapture 720p 取帧成功。 | 当前**内核**边界仍是 raw RDI/Bayer，`/dev/video3` 输出 `pgAA`，SM8250 mainline CAMSS 仍无完整 ISP/YUV/RGB 输出；Android 运行时 trace 已确认 OV13B10 在厂商 HAL 下会启用 DISP/FD/stats/PDAF/LCR/RDI 等 VFE480 BUS/comp-group 资源，但这仍只是下游运行时证据，不等同于主线已输出 YUV/RGB。用户态 software-ISP/loopback/UVC 是独立标准节点，不把 raw node 伪装成 YUV/RGB；v4l2loopback 默认节点为 `/dev/video20`，已用标准 V4L2 程序对 capture 侧完成 `QUERYCAP`/`ENUM_FMT`/`S_FMT`/`read()` 多帧验证，PipeWire/browser `getUserMedia` 仍待下一阶段验证；当前性能上限按 2080x1170 输入评估，不继续追更高输入分辨率；USB-HS 未压缩 UVC 现实档位是 720p@10 或 640x480@15；`/dev/video3` 的 frame-size enum 是 CAMSS 通用连续范围，离散模式和帧间隔应以 OV13B10 subdev 为准；前摄升降机构和其他后摄暂不启用。 |
| SBA-MUX / 模拟附件 | 待适配 | FSA4480 可能用于 USB-C analog/audio accessory mux。 | 当前 USB gadget/Type-C 基础链路不依赖它。 |
| 充电泵 / 私有快充 | 暂不支持 | BQ2597x 充电泵和 Xiaomi 私有快充不是当前阶段目标。 | 服务器使用目标优先稳定普通供电与可见电池状态。 |

## 已验证的基础组合

- 启动链路：copydown bootshim、Linux Image.gz、runtime DTB、内嵌 initramfs 和 UFS rootfs 可组合启动。
- 控制台链路：UFS rootfs、DSI fbcon、USB ACM、Wi-Fi SSH 可同时工作。
- 输入显示：触摸、电源/音量键、DRM/KMS/backlight 可同时工作。
- 图形链路：Adreno A650 / GMU / freedreno 可用于 GBM/EGL/GLES 渲染。
- 电源链路：PM8150B Type-C/TCPM、charger 和 fuel-gauge 可同时暴露 USB 输入、电池状态与标准限充控制接口。
- 音频链路：ADSP/PDR/QRTR/APR、SM8250 sound card、TFA9874 主扬声器、WCD9380 听筒、3.5mm HPH 播放、MBHC 插入/阻抗读取和两路机身麦克风基础录音已验证。
- 服务器链路：Ubuntu 26.04 Server rootfs、SSH、Docker bridge/NAT、端口映射、overlayfs、cgroup v2、nftables/iptables 已通过基础验证。

## 电池 / 充电当前目标

lmi 作为长期运行的小型服务器使用时，优先目标是稳定供电、电池状态可见、温度/安全信息可见和电池寿命。当前普通 Type-C/PD 路径能稳定供电即可作为基线；33W Xiaomi 私有快充、BQ2597x 充电泵和可能的硬件旁路供电控制需要更多私有策略或硬件验证，不作为当前 release 必备功能。

当前代码提供标准 `charge_behaviour`、`input_current_limit` 目标值和 `current_max` 有效值；rootfs 支持层的 `lmi-power` 默认使用 65%～70% 容量窗口、低输入电流和温度保护来降低长期满电、高温和大电流压力。当前 70% 停充状态和策略输出已实机核对。Linux 没有通用“电源直供/绕过电池”开关；这类能力必须由 PMIC/charger 硬件和对应驱动暴露，因此当前策略不等同于硬件旁路供电，也不能完全消除电池老化。

## 调制解调器当前结论

SDX55M 已经证明 PCIe2、MHI、signed SBL、Sahara HELLO/HELLO_RESP 和部分 image transfer 诊断链路可运行，但它仍没有进入 Mission/AMSS，也没有可用 QRTR/QMI、SIM、蜂窝数据、语音或 IMS/VoLTE。当前发布说明中应把 modem 标记为不可用或仅诊断阶段，不应把它写成日常蜂窝功能。

继续推进 modem 时，应从 [`MODEM_BRINGUP.md`](MODEM_BRINGUP.md) 恢复上下文，并继续遵守边界：不进入 EDL，不发送 firehose，不写 NV/modem/dtbo/recovery/vbmeta，不提交 firmware blob。
