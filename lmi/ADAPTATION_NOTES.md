# lmi 主线适配备注

本文件记录已经能稳定工作的模块里仍会出现的非致命提示。后续新增驱动时，如果功能验证通过但日志仍有可解释的 warning，也按同样格式追加到这里，避免把已知提示和真正回退混在一起。

## 判定规则

- 已验证核心功能可用、不会阻塞启动或当前目标场景的问题，记录为非致命提示。
- 记录内容需要包含：日志关键字、含义、当前影响、后续处理条件。
- 如果提示开始影响启动、设备枚举、挂载、显示、输入、网络或稳定性，必须从本文件移出并当作待修复问题处理。

## UFS / userdata

当前状态：UFS 已作为主线 Ubuntu 控制台阶段的稳定基线使用。`ufshcd-qcom` 能绑定，UFS PHY 正常，`sda1` 到 `sda34` 能枚举；当前 `/dev/sda34` 已被替换为 Ubuntu rootfs，label 为 `ubuntu-rootfs`，initramfs 能挂载后 `switch_root` 进入 Ubuntu 24.04。

### `freq-table-hz property not specified`

含义：UFS 平台驱动会先检查旧式 `freq-table-hz`，缺失时打印该提示；当前主线 SM8250 节点已经使用 `operating-points-v2 = <&ufs_opp_table>`，运行时也能看到 37.5 MHz / 300 MHz OPP 被加载。

当前影响：非致命；不影响 UFS 枚举、分区扫描、Ubuntu rootfs 挂载和 `switch_root`，当前 UFS debugfs error counters 为 0。

后续处理：不要为了消除该提示再补 deprecated `freq-table-hz`；binding 要求 `freq-table-hz` 和 `operating-points-v2` 互斥。只有后续发现实际频率/性能问题，才基于主线 OPP 表调整。

### `vdd-hba-supply regulator ... assuming enabled`

含义：UFS common code 会尝试读取可选的 host controller regulator。Android downstream 把该项接到 UFS PHY GDSC，当前主线 SM8250 已通过 `power-domains = <&gcc UFS_PHY_GDSC>` 建模 GDSC，不能把它伪装成 regulator 只为消除日志。

当前影响：非致命；当前 `vcc`、`vccq`、`vccq2` 和 UFS PHY 供电已建模，UFS probe、分区枚举和 rootfs 挂载稳定。

后续处理：只有确认 stock/downstream 中存在可对应到主线 RPMh regulator 的真实 HBA supply 后再补齐，避免错误 regulator 影响 probe 顺序或电源开关行为。

### `WB buf lifetime is exhausted 0x0B`

含义：UFS 设备上报 WriteBooster buffer lifetime 状态。它来自存储器件自身状态，不是 DTS 或主线适配错误。

当前影响：非致命；不影响当前读写和挂载验证。

后续处理：除非后续压力测试证明 WriteBooster 相关行为导致稳定性或性能问题，否则不处理。

## Display / Samsung AMS667UU01

当前状态：AMS667UU01 已能通过 MSM DRM/DPU/DSI 显示 fbcon 控制台。当前默认使用大字体和横向 fbcon：`fbcon=font:TER16x32 fbcon=rotate:1`。DRM 节点、DSI connector、backlight 节点均能出现；M10 验证中 panel 驱动已暴露默认 60 Hz 和可选 77 Hz 两个 1080x2400 mode。启动早期白屏/条纹已通过 lmi 专用 bootshim 在跳入 Linux 前拉低 GPIO46 panel reset 消除，随后 panel 驱动仍按正常流程重新初始化屏幕。

### 启动早期白屏/条纹

含义：bootloader 遗留的 panel/splash/DSI 状态在 Linux 接管前仍处于可见状态，导致简单帧缓冲和 DRM 接管窗口里出现短暂白屏或条纹。

当前影响：已验证修复；M5h 镜像在 bootshim 阶段拉低 lmi 的 GPIO46 panel reset 后，开机不再出现白屏，DRM fbdev、backlight、ACM 串口和触摸输入仍正常。

后续处理：该修复依赖 lmi 的 GPIO46 连接到 AMS667UU01 reset，只能用于 lmi 专用 bootshim；不要移植到通用 SM8250 bootshim。若后续更换面板 DTS 或启动链，需要重新确认 reset GPIO。

### `arm-smmu 15000000.iommu: Unhandled context fault ... iova=0x9c... SID=0x820/0xc20`

含义：显示接管期间，MDSS/DPU 相关 stream 在访问 bootloader splash framebuffer 所在的 `0x9c000000` 附近地址时触发 SMMU fault。

当前影响：非致命；当前 DRM fbdev、DSI panel、backlight 和 Ubuntu 控制台显示均正常。

后续处理：继续作为显示接管/continuous splash/IOMMU handoff 问题跟踪。已验证关闭 simple-framebuffer 并不能减少该 fault，因此当前保留 simple-framebuffer 和 `&mdss memory-region = <&cont_splash_memory>` 的更稳定基线。

### `disp_cc_mdss_pclk0_clk_src: rcg didn't update its configuration`

含义：DSI link clock 设置期间，display clock controller 的 pclk0 RCG 更新没有按驱动预期完成。

当前影响：非致命；不阻塞 panel 初始化或 fbcon 显示。

后续处理：如果后续出现刷新异常、黑屏、帧率异常或 panel timing 调整失败，再结合 dispcc/DSI clock 配置单独分析。

### `msm_dsi ae94000.dsi: supply refgen not found, using dummy regulator`

含义：DSI host 请求 `refgen` supply，但当前 DTS 没有提供该 regulator，内核使用 dummy regulator 继续。

当前影响：非致命；DSI host 能绑定，panel 能显示。

后续处理：确认 SM8250 stock/downstream 对 refgen 供电的建模后再补齐。

### `msm_dpu ae01000.display-controller: no GPU device was found`

含义：MSM DRM 初始化时未找到 GPU 设备。当前阶段只验证 display controller、DSI panel 和 fbdev console。

当前影响：非致命；不影响当前屏幕控制台目标。

后续处理：等后续适配 Adreno/GPU 或完整图形栈时再处理。

### HBM / FOD / DC dimming / ESD GPIO51

含义：Android downstream 的 J11 面板描述还包含 HBM、FOD HBM、DC dimming、doze、gamma/CRC/ACL 和 GPIO51 ESD error IRQ 等厂商扩展命令。主线 DRM panel 当前只建模标准显示、TE、backlight、60 Hz/77 Hz mode 和基本开关屏流程。

当前影响：非致命；不影响 fbcon、普通背光、默认 60 Hz 显示或 77 Hz mode 枚举，但不代表完整 Android 显示特性已经齐全。

后续处理：等进入完整图形桌面、指纹/息屏显示、高亮模式或 ESD 恢复需求时，再逐项把对应能力接到主线可接受的接口，避免把 downstream 私有 dispparam 命令一次性硬塞进基础 panel 驱动。

## USB / Type-C / ACM 调试

当前状态：PM8150B Type-C、DWC3 QCOM glue、USB HS PHY 和 configfs ACM gadget 已能支撑 Ubuntu 控制台调试。initramfs 会创建 VID/PID `1d6b:0104` 的 ACM gadget，Ubuntu 中提供 `/dev/ttyGS0` 登录，Windows 侧枚举为 COM 口。

### `qcom,pmic-typec ... isr: tx_sig`

含义：PM8150B Type-C 控制器上报 CC/PD PHY 相关中断，是当前 Type-C 控制器工作过程中的早期日志。

当前影响：非致命；不影响 DWC3 gadget 绑定、ACM 串口枚举和 Ubuntu 串口登录。

后续处理：如果后续启用更多 USB-C 角色切换、PD 协商或主机模式时出现断连，再结合 Type-C/role-switch 日志单独分析。

### 软件重启进入 bootloader/recovery

含义：lmi bootloader 能读取 PM8150 PON spare register 中的 reboot reason。主线 `qcom-pon` 写入 `mode-bootloader = <0x2>` / `mode-recovery = <0x1>` 后，`reboot bootloader` 和 `reboot recovery` 不再退化为普通重启。

当前影响：已验证可用；M4a PON-only 镜像确认 Ubuntu 中执行 `reboot bootloader` 能进入 fastboot，执行 `reboot recovery` 能进入 recovery ADB。

后续处理：保持 `CONFIG_POWER_RESET_QCOM_PON=y`，不要依赖模块自动加载；如果后续更换 PMIC/启动链后失效，再回看 stock DTB 中的 IMEM `restart_reason@65c` 作为备选路径。

## Touchscreen / FocalTech FT3518

当前状态：FT3518 已切回硬件 `i2c13` / GENI I2C 路径。`gpi_dma1` 使用 Android downstream 对应的 execution environment offset `qcom,gpi-ee-offset = <0x6000>` 后，M9 验证中不再出现 `EV ALLOCATE completion timeout`，`/sys/bus/i2c/devices/13-0038` 和 `/dev/input/event0` 均正常出现。GPIO38 为 reset，GPIO39 为 IRQ，GPIO72 必须和 reset/IRQ 一起进入 active pinctrl，否则 0x38 不 ACK。

### `input: generic ft5x06 (48)`

含义：`edt-ft5x06` 能通过 FT3518 的通用 FocalTech 寄存器完成 probe，但该芯片的 ID 寄存器没有返回 EDT 风格型号字符串，所以驱动以 `generic ft5x06 (48)` 命名输入设备。

当前影响：非致命；`/dev/input/event0` 存在，ABS/MT 坐标和 BTN_TOUCH 事件可用。

后续处理：如果后续需要更准确的设备名、手势、固件管理或高级参数，再补 FT3518 专用识别和寄存器表；当前不要为改名增加不必要的协议分支。

### `qcom,gpi-ee-offset = <0x6000>`

含义：Android downstream 的 SM8250 `gpi_dma1` 使用 `0x6000` 作为 GPI execution environment register offset；主线默认 `sm8250-gpi-dma` match data 为 `0x0`，会导致 `i2c13` 分配 GPI RX DMA channel 时超时。

当前影响：已验证可用；硬件 `i2c13` 触摸路径能 probe FT3518，并且不再需要 GPIO bitbang I2C fallback。

后续处理：如果后续把该改动整理给上游，需要继续评估是保留 per-node `qcom,gpi-ee-offset`，还是按 SoC/instance 建模 GPI EE offset。

## Wireless / QCA6391 Wi‑Fi + Bluetooth

当前状态：QCA6391 Wi‑Fi 按 QCA6390 PCIe 设备建模，`ath11k_pci` 能枚举 `17cb:1101`，接口为 `wlp1s0`，当前使用 `/persist/wlan_mac.bin` 中的真实 WLAN MAC `98:f6:21:c4:66:24`。Bluetooth 走 UART6 `qcom,qca6390-bt`，固件加载后通过 mgmt public-address 设置为 `98:F6:21:C4:66:25`，`btmgmt info` 显示 `missing options:` 为空。M8 验证中 `CONFIG_BT_LE=y`、`CONFIG_BT_RFCOMM_TTY=y`、`CONFIG_BT_HIDP=y` 已生效，`btmgmt` current settings 包含 `le`。

### 无线服务不阻塞启动

含义：rootfs 中的 firmware import、wireless reprobe 和 Wi‑Fi connect 服务不再作为 `Type=oneshot` 阻塞 `multi-user.target`；Wi‑Fi 连接服务由 systemd 前台托管 `wpa_supplicant`，避免脚本退出时杀掉后台 supplicant。

当前影响：已验证可用；M8 镜像 userspace 到达 `multi-user.target` 约 2.5 秒，Wi‑Fi 随后保持 `wlp1s0` 连接并获取 `192.168.0.41/24`。

后续处理：后续若把 Wi‑Fi/BT 转成模块，应继续保持无线连接和 firmware 导入不阻塞启动，并确认模块自动加载不会重新引入 initramfs firmware/MAC 时序问题。

### Bluetooth public address 由支持层设置

含义：当前 QCA Bluetooth 固件路径不会自行从设备 NVM 暴露真实 public address，因此 initramfs 从真实 WLAN MAC 派生 BT 地址并通过 mgmt `Set Public Address` 写入控制器。

当前影响：功能可用；`hci0` 为 `UP RUNNING`，地址稳定为 `98:F6:21:C4:66:25`，LE/BR-EDR 基础能力已开启。

后续处理：如果后续找到原厂 BT address 的独立持久化来源，或主线 QCA 路径获得设备专用 NVM 地址解析，再替换当前 WLAN+1 派生策略。
