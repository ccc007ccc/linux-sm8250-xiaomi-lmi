# lmi 主线适配备注

> 100% AI 编写：本文档由 AI 生成和整理。

本文件只保留 Redmi K30 Pro / POCO F2 Pro (`lmi`) 主线 Linux 适配的全局状态、关键坑点和跨子系统摘要。过长的单专题 bring-up 记录已拆分，避免主记录成为上下文炸弹。

面向使用者的硬件状态见 [HARDWARE_SUPPORT.md](HARDWARE_SUPPORT.md)，摄像头适配见 [CAMERA_BRINGUP.md](CAMERA_BRINGUP.md)，基带适配见 [MODEM_BRINGUP.md](MODEM_BRINGUP.md)，编译和刷写教程见 [docs/](docs/)。

## 当前基线

- 设备：Redmi K30 Pro / POCO F2 Pro，代号 `lmi`，Qualcomm SM8250 / Snapdragon 865。
- 启动：Android boot image -> copydown bootshim -> Linux `Image.gz` + runtime DTB -> 内嵌 initramfs -> UFS rootfs。
- 当前主验证系统：Ubuntu 26.04 Server arm64 rootfs，推荐 `/dev/sda34` / `LABEL=ubuntu-rootfs`。
- 适配边界：普通发行版 rootfs 尽量保持纯净，硬件适配优先放在内核、DTS、initramfs 和必要支持层中。
- 测试刷写边界：开发阶段优先 `fastboot boot` 临时启动，不写 boot 分区；lmi 当前流程是先在设备上 `systemctl --no-wall --reboot-argument=bootloader reboot`，再用 Windows `fastboot.exe boot ...img`。
- 固件边界：stock/signed firmware 只放 ignored/local firmware 路径，不提交到源码仓库，不发布包含未公开固件的制品。

## 阅读导航

| 主题 | 记录位置 |
| --- | --- |
| 全局 bring-up 摘要 / warning 判定 | 本文件 |
| 摄像头 / OV13B10 / CAMSS / Venus | [CAMERA_BRINGUP.md](CAMERA_BRINGUP.md) |
| SDX55M / X55 5G 基带 | [MODEM_BRINGUP.md](MODEM_BRINGUP.md) |
| 用户可见硬件状态 | [HARDWARE_SUPPORT.md](HARDWARE_SUPPORT.md) |
| 构建、boot、发布说明 | [docs/](docs/) |

## 判定规则

- 已验证核心功能可用、不会阻塞启动或当前目标场景的问题，记录为非致命提示。
- 记录内容只保留：日志关键字、含义、当前影响、后续处理条件。
- 如果提示开始影响启动、设备枚举、挂载、显示、输入、网络或稳定性，必须从“已解释 warning”升级为待修复问题。
- 单专题长日志不要继续堆在本文档；新专题需要独立 `*_BRINGUP.md`，主文档只放摘要和链接。

## UFS / userdata

当前状态：UFS 已作为 Ubuntu 控制台阶段的稳定基线使用。`ufshcd-qcom` 能绑定，UFS PHY 正常，`sda1` 到 `sda34` 能枚举；当前 `/dev/sda34` 已作为 Ubuntu rootfs 使用，initramfs 能挂载后 `switch_root`。

已解释 warning：

- `freq-table-hz property not specified`：当前主线 SM8250 UFS 节点使用 `operating-points-v2`，不要为了消日志补 deprecated `freq-table-hz`。
- `vdd-hba-supply regulator ... assuming enabled`：host controller regulator 是可选项；当前通过 UFS PHY GDSC、`vcc`、`vccq`、`vccq2` 建模供电，不能把 downstream 的 GDSC 伪装成 regulator。
- `WB buf lifetime is exhausted 0x0B`：来自 UFS 设备自身 WriteBooster lifetime 状态；当前不影响读写和挂载。

后续只有在 UFS 频率、性能、错误计数或挂载稳定性出现问题时再处理。

## Display / Samsung AMS667UU01

当前状态：AMS667UU01 已能通过 MSM DRM/DPU/DSI 显示 fbcon 控制台。默认使用大字体和横向 fbcon：`fbcon=font:TER16x32 fbcon=rotate:1`。DRM 节点、DSI connector、backlight 节点均能出现；panel 驱动已暴露 60 Hz 和 77 Hz mode。

关键结论：

- 启动早期白屏/条纹已通过 lmi 专用 bootshim 在跳入 Linux 前拉低 GPIO46 panel reset 解决；不要移植到通用 SM8250 bootshim。
- 显示接管期间 `arm-smmu ... iova=0x9c... SID=0x820/0xc20` 属于 bootloader splash/continuous framebuffer handoff 问题；当前 DRM fbdev、backlight 和控制台正常，暂非致命。
- `disp_cc_mdss_pclk0_clk_src: rcg didn't update its configuration` 和 `dsi_err_worker: status=5` 暂不阻塞 panel 初始化；若后续出现闪屏、黑屏、ESD 恢复或 mode switch 异常再处理。
- HBM/FOD/DC dimming/doze/gamma/ESD GPIO51 等 Android 私有扩展尚未完整主线化，不影响基础显示。

## GPU / Adreno A650 / Freedreno

当前状态：SM8250 Adreno A650 走主线 DRM/MSM/Freedreno 路径。lmi DTS 启用 SoC `&gmu`、`&gpu`，并把 `&gpu_zap_shader` 指到设备匹配的 local firmware path。验证中过 `/dev/dri/renderD128`、GMU firmware、`eglinfo -B` 和 `kmscube`。

关键结论：

- `adreno ... supply vdd/vddcx not found, using dummy regulator` 当前非致命；GPU 电源主要通过 GPUCC GDSC、RPMh power domains、GMU 和 OPP 建模。不要为消日志添加假 regulator。
- A650 zap firmware 必须使用设备可接受的 signed MDT + split 段。通用 firmware 曾导致初始化失败；local blobs 不提交。
- 若以后模块化 DRM/MSM，需要同步 modules/install、firmware 和 initramfs/rootfs 路径。

## Power / PM8150B charger / fuel-gauge

当前状态：PM8150B Type-C/TCPM/VBUS、SMB5 charger 和 gen4 fuel-gauge 已接通。`/sys/class/power_supply` 出现 `pm8150b-charger`、`qcom-battery` 和 TCPM supply；PD 输入和普通 5V 输入均做过验证。

关键结论：

- 当 `CONFIG_CHARGER_QCOM_SMB2=y` 和 `CONFIG_BATTERY_QCOM_FG=y` 为 built-in 时，ADC provider 也必须 built-in：`CONFIG_QCOM_VADC_COMMON=y`、`CONFIG_QCOM_SPMI_ADC5=y`、`CONFIG_QCOM_SPMI_ADC_TM5=y`。
- `pm8150b-charger` 输入电压/电流上报应按 USB online 读取 IIO，不应绑定到电池是否处于 `Charging`。
- 当前内核暴露标准 `charge_behaviour` 和 `input_current_limit`；长期插电服务器策略由 `lmi-power` 支持层实现，默认 75% 停充、70% 恢复，温度策略优先。
- 这不是硬件旁路供电，也不覆盖 Xiaomi 私有快充/充电泵完整支持。

## USB / Type-C / ACM 调试

当前状态：PM8150B Type-C、DWC3 QCOM glue、USB HS PHY 和 configfs ACM gadget 可支撑 Ubuntu 控制台调试。initramfs 创建 ACM gadget，Windows 侧枚举为 COM 口，Ubuntu 中提供 `/dev/ttyGS0` 登录。

关键结论：

- `qcom,pmic-typec ... isr: tx_sig` 当前是 Type-C 控制器工作日志，非致命。
- `qcom-pon` reboot reason 已验证可让 `reboot bootloader` / `reboot recovery` 进入对应模式；保持 `CONFIG_POWER_RESET_QCOM_PON=y`。
- 当前 fastboot 测试仍推荐从系统内用 `systemctl --no-wall --reboot-argument=bootloader reboot` 进入 bootloader，再跑 Windows `fastboot.exe boot`。

## Touchscreen / FocalTech FT3518

当前状态：FT3518 走硬件 `i2c13` / GENI I2C 路径。`gpi_dma1` 使用 downstream 对应的 `qcom,gpi-ee-offset = <0x6000>` 后，不再出现 GPI DMA allocate timeout；`/dev/input/event0` 可用。

关键结论：

- `input: generic ft5x06 (48)` 只是通用 FocalTech 识别名，不影响触摸事件。
- GPIO38 reset、GPIO39 IRQ、GPIO72 active pinctrl 都是 ACK 所需条件。
- `qcom,gpi-ee-offset = <0x6000>` 是 lmi 上硬件 I2C 稳定的关键；后续上游化时再评估 per-node 属性或 SoC/instance match data。

## Input / hardware keys

当前状态：PM8150 PON pwrkey、PM8150 RESIN 和 PM8150 GPIO6 音量上键均能枚举。内核 DTS 只负责上报标准 key code；亮度/电源键策略由支持层处理。

关键结论：

- 电源键必须能唤醒背光，调试阶段短按只切换 backlight，不触发关机/挂起。
- 音量键亮度策略属于 userspace daemon，不要把内核 key code 改成非标准值规避 shell 字符。
- 如果 shell 收到音量键字符，先检查 daemon active、evdev grab 和日志。

## Wireless / QCA6391 Wi‑Fi + Bluetooth

当前状态：QCA6391 Wi-Fi 按 QCA6390 PCIe 设备建模，`ath11k_pci` 可枚举 `17cb:1101`，接口为 `wlp1s0`。Bluetooth 走 UART6 `qcom,qca6390-bt`，固件加载后可用。

关键结论：

- firmware import、wireless reprobe 和 Wi-Fi connect 不应阻塞 `multi-user.target`。
- Wi-Fi 使用设备持久化 MAC 来源中的真实 WLAN MAC。
- BT public address 当前由支持层从 WLAN MAC 派生并通过 mgmt 设置；若以后找到真实 BT address 来源再替换。
- 若 Wi-Fi 因 Android/Google 连通性检测失败断开，先从网络策略排查，不要误判为内核驱动失败。

## Audio / QDSP6 / TFA9874

当前状态：TFA9874 主扬声器目标路由、WCD9380 听筒、3.5mm HPH 播放、MBHC 插入/阻抗读取、WCD9380 TX 两个机身麦克风首版支持已验证。SM8250 sound card 注册为 `Xiaomi lmi`。当前这台调试机的物理主扬声器已在 Android 下确认损坏，后续主扬声器声学验证需要换正常硬件。

关键结论：

- ADSP firmware 和 split 段必须位于 ignored/local firmware；没有 `qcom/sm8250/adsp.mbn` 时 audio APR 服务不会建立。
- `CONFIG_QCOM_PD_MAPPER=y` 和 `CONFIG_QRTR_SMD=y` 必须保持 built-in，确保 PDR audio_pd 和 APR 子服务出现。
- Q6ASM PCM 对 period/buffer 参数敏感；已验证 48 kHz S16_LE、period 480、buffer 960 的低音量测试可用。
- 主扬声器路线是 `PRI_MI2S_RX -> TFA9874`；听筒/3.5mm 走 `RX_CODEC_DMA_RX_0 -> WCD9380 RX`。
- TFA9874 需要 downstream 对照出的关键寄存器配置，尤其 TDM delay 位，避免 `SPK_AMP=1` 仍异常大声。
- 麦克风当前定位：底部麦克风 `mic1 -> AMIC1`，顶部麦克风 `mic2 -> AMIC5`；第三个机身麦克风仍未定位。两路机身麦克风已用 48 kHz S16_LE 录音验证 `tail_after_1s` 持续非零。
- WCD capture link 必须使用 TX SoundWire capture DAI：`<&swr2 1>`。`swr2` 有 1 个 `dout` 口，`SDW Pin0` 是 playback/output；若误写成 `<&swr2 0>`，DAPM/Q6AFE 可能看似启动，但 SoundWire master 不会为 TX capture 分配/enable port，录音只剩 startup transient 或全零 tail。
- 听筒 `EAR_PA Volume` 控件方向与直觉相反：`0` 是最大 EAR PA 增益，`16` 是最小增益；当前听筒实测需要最大端才容易听见。
- 实机音频测试必须低振幅、短时长，避免突然大声。

## Camera / OV13B10 / CAMSS / Venus

摄像头详细状态已拆到 [CAMERA_BRINGUP.md](CAMERA_BRINGUP.md)。当前全局摘要：

- 后置超广角 OV13B10 已在 `&cci0_i2c1`、地址 `0x10` probe，工作路线是 `OV13B10 -> CSIPHY1 -> CSID1 -> VFE1 RDI0 -> /dev/video3`。
- `/dev/video3` 是真实 RAW10 Bayer RDI，fourcc `pgAA`，不是 ISP/YUV/RGB；`vfe480_yc_pp_chain_configured()` 仍必须保持关闭。
- OV13B10 subdev 暴露并验证 6 个原生模式：`4208x3120`、`4160x3120`、`4160x2340`、`2104x1560`、`2080x1170`、`1364x768`。默认 UVC/照片 profile 已收敛为 `native-modes`，只 advertised 这 6 个原生 MJPEG frame。
- UVC `COMMIT` frame 会一对一切换到对应 `mode_index`，再以同尺寸重新配置 `/dev/video3 pgAA` 和用户态 `lmi-isp` 输出；不再把一个 RAW mode 缩放成多个看似原生的档位。
- initramfs 只在 `/run` tmpfs 注入 transient UVC demand-start 支持层：host 可先枚举 `UVC Camera`，但 OV13B10/CAMSS/`lmi-isp` 只在 host `STREAMON` 后启动，`STREAMOFF`/`DISCONNECT` 后回到 idle 枚举状态；普通 rootfs 不落盘安装服务，可用 `lmi.uvc=0`/`lmi.no_uvc` 禁用。
- Rust `lmi-camera` 运行时现在负责生产/default 相机控制面，配合 C `lmi-isp` 与 `lmi-uvc-gadget` 提供不改普通 rootfs 的 software-ISP/UVC 支持；H.264 实验路径另有 `lmi-venus-enc` 桥接 tight NV12 到 Venus `/dev/video15`，但默认/public 仍是 MJPEG native-six。这些都是支持层，不是 libcamera/PipeWire/browser 标准栈，也不把 RAW node 伪装成 processed node。
- Venus codec 当前按 Image.gz+dtb/initramfs 内建路径准备，配置保持 `CONFIG_V4L2_MEM2MEM_DEV=y`、`CONFIG_VIDEO_QCOM_VENUS=y`、`CONFIG_VIDEOBUF2_DMA_CONTIG=y`；在提供本地 ignored Venus firmware 时，已验证 generated NV12 -> H.264 硬件编码。这证明 Venus encoder 可用于实验链路，但不等于相机已有真 YUV 输出，也不代表 firmware blob 可以提交。
- H.264 UVC 低分辨率 frame 6 已在临时 `/run` 链路验证到 Windows DirectShow/ffmpeg 可解码动态画面：`/dev/video3 pgAA 1364x768` -> software NV12 -> Venus H.264 -> UVC。关键兼容性是 H.264 OUTPUT buffer 默认不打 `V4L2_BUF_FLAG_KEYFRAME/PFRAME`，避免 f_uvc 生成 `UVC_STREAM_STI`；34-byte/48-byte PROBE/COMMIT 不是决定因素。由于默认 no-keyframe-flags，ffmpeg 纯 stream copy 需 `-copyinkf -c:v copy`。
- VFE480 `/dev/video6` 仍只是实验 `BA10` RAW_DUMP 诊断节点。Android HAL/CHI/tuning/CDM/DMI 运行时生成结构化 common-path 程序，但不是可复制进 mainline 的静态常量；不能用 fake NV12、rootfs workaround 或 Android tuning/DMI/CDM 值掩盖缺口。

## Modem / SDX55M / X55 5G

基带详细状态已拆到 [MODEM_BRINGUP.md](MODEM_BRINGUP.md)。当前全局摘要：

- PCIe2 可枚举 SDX55M endpoint，signed SBL 可加载，设备可进入 `SECONDARY_BOOTLOADER` / M0。
- `/dev/mhi_bl0` 可读 SBL boot log，`/dev/mhi_sahara0` 可读 Sahara HELLO 并完成 HELLO_RESP 诊断。
- 还没有 Mission/AMSS、QMI、MBIM、SIM、蜂窝数据或语音支持。
- 当前阻塞点在 ESOC/request-image/Sahara image-loader/MHI lifecycle 组合，而不是基础 PCIe 或 SBL firmware 加载。
- 严禁 EDL/firehose/NV 写入；modem firmware 不提交。

## PCIe / regulator warnings

- `qcom-pcie 1c10000.pcie: supply vdda/vddpe-3v3 not found, using dummy regulator` 当前按非致命记录。PCIe2 能用于 SDX55M endpoint bring-up，QCA6391 PCIe 也能工作。
- 不要为消日志添加假 regulator。只有后续发现 PCIe 稳定性、L1SS、功耗或 resume 问题，再对照 downstream/PMIC 真实供电关系补齐。

## 文档维护规则

- 本文件只保留跨子系统状态和短结论。
- 摄像头新增测试结论优先写入 [docs/camera/](docs/camera/) 对应专题文档，并同步压缩到 [CAMERA_BRINGUP.md](CAMERA_BRINGUP.md) 主索引。
- 基带新增测试结果写入 [MODEM_BRINGUP.md](MODEM_BRINGUP.md)，优先记录阶段结论，不再逐轮堆完整日志。
- 用户可见支持状态同步到 [HARDWARE_SUPPORT.md](HARDWARE_SUPPORT.md)。
- 若需要完整历史，优先查 git history 或构建/测试产物，不把所有原始日志重新粘回 Markdown。
