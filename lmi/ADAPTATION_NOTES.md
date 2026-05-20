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

### `dsi_err_worker: status=5`

含义：DSI error worker 读到 panel/DSI 状态异常位，当前没有伴随 DRM connector 丢失或 framebuffer 退出。

当前影响：暂按非致命记录；fbcon、`modetest -M msm`、`kmscube` 和 DSI 60/77 Hz mode 仍可用。

后续处理：如果后续出现闪屏、黑屏、ESD 恢复失败或 mode switch 不稳定，再结合 Android downstream 的 GPIO51 ESD/status 逻辑和 DSI error status 单独处理。

### HBM / FOD / DC dimming / ESD GPIO51

含义：Android downstream 的 J11 面板描述还包含 HBM、FOD HBM、DC dimming、doze、gamma/CRC/ACL 和 GPIO51 ESD error IRQ 等厂商扩展命令。主线 DRM panel 当前只建模标准显示、TE、backlight、60 Hz/77 Hz mode 和基本开关屏流程。

当前影响：非致命；不影响 fbcon、普通背光、默认 60 Hz 显示或 77 Hz mode 枚举，但不代表完整 Android 显示特性已经齐全。

后续处理：等进入完整图形桌面、指纹/息屏显示、高亮模式或 ESD 恢复需求时，再逐项把对应能力接到主线可接受的接口，避免把 downstream 私有 dispparam 命令一次性硬塞进基础 panel 驱动。

## GPU / Adreno A650 / Freedreno

当前状态：SM8250 Adreno A650 已按主线 DRM/MSM/Freedreno 路径启用。lmi DTS 只启用 SoC 已有的 `&gmu`、`&gpu` 并把 `&gpu_zap_shader` 指到 stock segmented zap `qcom/sm8250/xiaomi/lmi/a650_zap.mdt`。M11b 验证中 `/dev/dri/renderD128` 出现，GMU firmware v2.1.8 加载，`eglinfo -B` 和 `kmscube -D /dev/dri/card0` 均显示 `freedreno` / `FD650`。

### `adreno 3d00000.gpu: supply vdd/vddcx not found, using dummy regulator`

含义：MSM Adreno common path 会请求可选 `vdd` / `vddcx` regulator；当前主线 SM8250 GPU 节点主要通过 GPUCC GDSC、RPMh power domains、GMU 和 OPP 建模电源/频率，lmi 尚未确认存在可安全映射到这两个名字的板级 regulator。

当前影响：非致命；GPU hw init 成功，render node 出现，Mesa 使用 freedreno FD650 而不是 llvmpipe。

后续处理：只有后续发现 GPU 压力测试、频率切换、挂起恢复或功耗异常时，再对照 stock/downstream regulator 关系补真实 supply；不要为消日志添加假 fixed regulator。

### lmi `a650_zap.mdt` firmware

含义：A650 secure zap 需要设备可接受的签名 MDT + bXX 段。通用 `qcom/sm8250/a650_zap.mbn` 曾导致 `error -22 initializing firmware` 和 GMU OOB timeout。

当前影响：已验证可用；firmware blob 放在 initramfs/rootfs 的 ignored `local/firmware` 路径，不进入 git。因为 `CONFIG_DRM_MSM=y` 是内建，initramfs 需要包含 `a650_sqe.fw`、`a650_gmu.bin` 和 lmi zap 段。

后续处理：如果以后模块化 DRM/MSM，也要同步 modules/install、firmware 和 rootfs 路径；不要把 stock zap blob 提交到源码仓库。

## Power / PM8150B charger / fuel-gauge

当前状态：PM8150B Type-C/TCPM/VBUS、SMB5 charger 和 gen4 fuel-gauge 已按普通充电路线接通。M14 验证 `/sys/class/power_supply` 出现 `pm8150b-charger`、`qcom-battery` 和 TCPM supply，并在标准 PD 电源上协商到 9V/2A；M15 验证 `pm8150b-charger` 在 USB online 时能上报 `voltage_now` 和 `current_now`。

### `deferred probe pending: qcom-smbx-charger: Couldn't get usbin_v IIO channel`

含义：PM8150B charger 是 built-in，probe 时需要 `ADC5_USB_IN_I` / `ADC5_USB_IN_V_16` IIO channel；M13 中 ADC5/VADC/ADC_TM 仍是模块，导致 charger 和 fuel-gauge 持续 deferred probe。

当前影响：已验证修复；`CONFIG_QCOM_VADC_COMMON=y`、`CONFIG_QCOM_SPMI_ADC5=y`、`CONFIG_QCOM_SPMI_ADC_TM5=y` 后，M14 起 charger/FG 能随内核内建路径正常 probe。

后续处理：只要 `CONFIG_CHARGER_QCOM_SMB2=y` 和 `CONFIG_BATTERY_QCOM_FG=y` 维持 built-in，就保持 ADC provider 也是 built-in；如果后续模块化电源栈，需要同步验证模块加载和 IIO probe 顺序。

### `pm8150b-charger` 在 `Full` 状态下输入电压/电流为 0

含义：早期 SMB5 backport 复用了按 `POWER_SUPPLY_STATUS_CHARGING` 判断的 helper，导致 USB 已在线但电池状态不是 `Charging` 时不读取 `usbin_v/usbin_i` IIO。

当前影响：已验证修复；M15 改为按 `smb_get_prop_usb_online()` 判断是否读取 IIO，当前普通 5V 输入下 `pm8150b-charger` 上报 `voltage_now=4623520`、`current_now=442470`。

后续处理：后续接 PD 电源时继续确认 TCPM 的 9V/2A 与 charger 侧 IIO 读数同时合理；如果加入限充/停充策略，也不要把输入电压/电流上报重新绑定到电池是否正在充电。

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

## Modem / SDX55M / X55 5G

当前状态：M16 起启用 SM8250 PCIe2 和 modem PCIe PHY 后，SDX55M endpoint `17cb:0306` / subsystem `17cb:010c` 能以 PCIe Gen3 x2 枚举；M17 将该 endpoint 的 MHI 匹配改到 `qcom-sdx55m` 配置后，`mhi-pci-generic` 会请求 `qcom/sdx55m/sbl1.mbn`。M18 已把设备匹配的 signed `sbl1.mbn` 放入 ignored initramfs local firmware，MHI 能加载 SBL，regdump 显示 `Device EE: SECONDARY_BOOTLOADER state: M0`。M19/M20 验证 AP/MDM sideband GPIO 静态电平后仍不能进入 Mission/AMSS；M21 加入 SBL `SAHARA` channel 2/3 后仍未生成 MHI 设备；M22 临时轮询硬件 EE 并手动排队 SBL transition 后出现 `mhi0_SAHARA`；M24 额外强制 host 进入 `MHI_PM_M0` 后，`mhi_sahara_diag` 才能完成 probe 并注册 `/dev/mhi_sahara0`。M27/M28 确认 SAHARA START completion 和 48 字节 HELLO event 已写入 event ring，但主线 IRQ/tasklet 路径没有及时处理，强制 shared MSI 也不足以修复。M29-M32 的主动 drain 验证了事件解析和 callback 路径本身可用：START/RESET command completion 可从 er0 处理，DL `SAHARA` 可读出 48 字节 HELLO；M39 在 SDX55M profile 中补 downstream-like SBL `BL` channel 25，并扩展诊断 client 后，`mhi0_BL` / `/dev/mhi_bl0` 可出现，读取到 2358 字节 SBL boot log；M42 在 PCI endpoint 枚举后、MHI power-up 前执行 AP2MDM_STATUS low→150ms→high，实测 MDM2AP_STATUS/ERRFATAL 仍为 0，BL/HELLO 行为不变，HELLO_RESP 后仍无后续 READ_DATA/下一包；M43 进一步在 AP2MDM_STATUS 拉低期间通过 modem PMIC USID8 PON S2 序列触发 warm reset，实测 AP 未异常重启，PCIe2 endpoint、MHI SBL/M0、BL log 和 Sahara HELLO 基线仍可用，但 MDM2AP_STATUS/ERRFATAL 仍为 0，HELLO_RESP 后仍无 READ_DATA/后续包；M44 在诊断 client 中加入 20ms 周期 event drain 后，确认 BL/SAHARA 的 START completion、RX event、HELLO_RESP UL completion 和 RESET completion 都能被轮询处理，但 SDX55M event MSI 计数仍为 0，HELLO_RESP 后仍无 READ_DATA/后续包；M46 确认 event context 本身为 VALID，er1 `msivec=2` / `irq=2` 映射到 Linux IRQ 190，PCI sysfs 也已分配 188-194 七个 MSI，但 SDX55M event MSI 计数仍为 0；M47 在诊断 client 中收到 48 字节 HELLO 后立即由内核自动 queue HELLO_RESP，且 er1 上能看到 ch2 UL completion，但仍无 READ_DATA/后续包；M48 证明过早 auto-start SAHARA 会触发 SYS_ERROR 和 BL START timeout，不能作为修复路线；M49 改为先让 BL auto-start、2 秒后延迟 auto-start SAHARA，BL log 和 HELLO_RESP UL completion 恢复但仍无 READ_DATA/后续包；M50 注册 MDM2AP_STATUS/ERRFATAL IRQ 并在 ESOC_REQ_IMG/SBL 阶段轮询，AP2MDM_STATUS 拉高后 MDM2AP_STATUS/ERRFATAL 仍全程为 0，设备保持 SBL/M0；M51 按 Android downstream `kona-mhi.dtsi` 对齐 SDX55M channel/event profile 后分配到 10 个 MSI vector 和 17 个 event ring，但 SDX55M event IRQ 189-197 仍全 0，HELLO_RESP 后仍无 READ_DATA/后续包；M52 解析当前 Sahara HELLO 为 `cmd=1 length=48 version=2 compatible=1 max_cmd_len=1024 mode=0`，自动 HELLO_RESP 为 `cmd=2 length=48 version=2 compatible=1 status=0 mode=0`，UL completion `status 0 bytes 48`，10 秒观察后仍保持 SBL/M0 且无 READ_DATA/后续包；M53 在 AP2MDM_STATUS 拉高并标记 ESOC_REQ_IMG 后等待 2 秒再启动 MHI，等待窗口结束时 MDM2AP_STATUS/ERRFATAL 仍为 0，随后仍能进入 SBL/M0 并复现 BL/SAHARA/HELLO_RESP 基线，但 HELLO_RESP 后仍无 READ_DATA/后续包。M54 将 HELLO_RESP 的 mode 改为标准 Sahara command mode `3`，SBL HELLO 仍为 `mode=0`，HELLO_RESP 仍有 ch2 UL completion，但 10 秒内没有任何 command-mode 短包、READ_DATA 或下一包。M58 改为更接近 Android downstream `mhi_uci` 的 open-time SAHARA start、近满 DL RX ring 预排和 close unprepare 后，首次 open 仍是 HELLO→HELLO_RESP→timeout，但第二次 channel restart 后出现真实 `READ_DATA`：`3 20 34 0 40`，随后旧探测脚本误把 20 字节包当作不完整 HELLO 并写入 HELLO_RESP，收到 `END_OF_IMAGE`：`4 16 34 0`。当前缺口已经从“完全没有 READ_DATA”收窄为 image 34 / `TRDATA` / `mdmddr` 的被动传输和后续 ESOC/MDM2AP/Mission 时序，而不是 PCIe、SBL firmware、BL/SAHARA ring 数据、HELLO_RESP 字段、HELLO_RESP mode=0、HELLO_RESP 用户态响应延迟、MHI profile 规模/MSI 数量、host event context 映射、ESOC_REQ_IMG 到 MHI power-up 之间的简单等待窗口，或单独切 Sahara command-mode。

### `MHI PCI device found: foxconn-sdx55`

含义：主线 `mhi-pci-generic` 中 `17cb:0306` / `17cb:010c` 的精确子系统匹配排在通用 `qcom-sdx55m` 前面，默认会选到没有 `.fw` 的 `foxconn-sdx55` 配置。

当前影响：已验证修复；M17 改为使用 `qcom-sdx55m` 后，驱动会请求 `qcom/sdx55m/sbl1.mbn`，不再停在 `No firmware image defined or !sbl_size || !seg_len`。

后续处理：保持 lmi 设备使用 SDX55M boot firmware 路径。若未来整理上游提交，需要再评估该 subsystem ID 是否也覆盖独立 Foxconn 模块，避免影响非 lmi 设备。

### `Direct firmware load for qcom/sdx55m/sbl1.mbn failed with error -2`

含义：MHI 已进入 firmware 请求阶段，但 initramfs/rootfs 当时没有设备可接受签名的 SDX55M `sbl1.mbn`。

当前影响：已验证修复；M18 从 stock modem 分区提取的 `sbl1.mbn` 放在 ignored local firmware 路径后，driver 不再报 `-2`，并能把 modem 带到 SBL。当前 local `sbl1.mbn` 大小为 `548056`，sha256 为 `0fd2fdaf19831c8ff482ca77ca236ee282101c9bdf5ab7b46dc4e24a484e84dc`。

后续处理：继续只把设备匹配的 signed `sbl1.mbn` 放到 ignored/local firmware 路径，例如 `sm8250-xiaomi-lmi-initramfs/local/firmware/qcom/sdx55m/sbl1.mbn`，不要提交 firmware blob。如果该错误重新出现，优先检查 initramfs 是否包含 local firmware，而不是修改 DTS 或 MHI 匹配。

### `Device EE: SECONDARY_BOOTLOADER state: M0`

含义：`sbl1.mbn` 已经通过 BHI 加载，modem 从 PBL 进入 SBL，但 host 仍未看到 Mission/AMSS。Android downstream 的 MHI 配置包含 SBL `SAHARA` channel 2/3 和 `BL` channel，且还有 ESOC/PON reset 时序；当前 mainline 第一版只走 PCIe + `mhi-pci-generic`。

当前影响：部分支持；说明 PCIe2、PERST/CLKREQ/WAKE、firmware request 和 signed SBL 加载已走通，但蜂窝功能仍不可用。M20 已确认 GPIO56 为 output high、GPIO57 为 output low、GPIO1/3 为无上下拉输入，静态 AP/MDM sideband 电平不足以让 modem 进入 Mission/AMSS。M21 已确认只补 SBL `SAHARA` channel 2/3 不会自动生成 `mhi0_SAHARA`，因为 host 仍停在 `PRIMARY_BOOTLOADER`。

后续处理：M22 的 EE polling 只作为诊断补丁保留，用来证明硬件已到 SBL 且手动触发 transition 后会创建 `mhi0_SAHARA`。后续应先定位 mainline 为什么没有收到或处理 SBL EE event，再评估是否需要最小 Sahara/UCI 诊断客户端加载后续 AMSS；`qdsp6sw.mbn` 等 modem firmware 只能放 ignored/local 路径，不提交。不要进入 EDL、firehose 或 NV 写入流程。

### `mhi0_SAHARA`

含义：M22 在 firmware load 后临时轮询硬件 EE，读到 `SECONDARY_BOOTLOADER` 并手动排队 SBL transition 后，MHI core 才创建 SBL `SAHARA` 设备。这说明 SBL channel 表是必要条件，但缺少 SBL EE transition 时不会被实例化。

当前影响：诊断有效但不是完整支持；M24 还需要额外根据硬件 `MHI_STATE_M0` 强制 host 进入 `MHI_PM_M0`，否则 `mhi_probe()` 会在 client driver probe 前因 `mhi_device_get_sync()` 失败而阻止 `mhi_sahara_diag` 绑定。

后续处理：不要把 EE polling 或 M0 forcing 当最终修复提交。下一步应先定位 mainline 为什么没有自然收到或处理 SBL EE/M0 transition，再决定是否需要受控的 SDX55M quirk。

### `/dev/mhi_sahara0`

含义：M24 起新增的诊断 client 只匹配 SBL `SAHARA` channel，打开设备时才 prepare transfer 和排 RX buffer，暴露 `/dev/mhi_sahara0` 供本地读取 Sahara bootloader 数据。

当前影响：M32 已验证 `dd if=/dev/mhi_sahara0 bs=48 count=1` 能读到 48 字节 Sahara HELLO；该节点只证明 SBL Sahara 数据面可访问，不表示 AMSS/Mission、QMI、MBIM、SIM、语音或蜂窝数据已经支持。

后续处理：保持它作为 bring-up 诊断工具，不自动发送 firehose、EDL、NV 或未确认的 AMSS payload。若后续需要加载 `qdsp6sw.mbn` 等 modem firmware，也只能放 ignored/local 路径并单独验证协议阶段。

### `/dev/mhi_bl0`

含义：M39 按 Android downstream 的 SDX55M SBL `BL` channel 25 增加只读诊断通道。该 channel 为 DL-only、SBL EE、event ring 1，probe 时自动 prepare transfer 和排 RX buffer，暴露 `/dev/mhi_bl0`。

当前影响：已验证 M39 能生成 `mhi0_BL` 和 `/dev/mhi_bl0`；直接读取该节点可得到 2358 字节 SBL boot log，内容包含 `Secure Boot: On`、`QC_IMAGE_VERSION_STRING=BOOT.SBL.4.1-00168`、`IMAGE_VARIANT_STRING=MAAHANAZA`、`Boot Interface: PCIe` 和 modem serial。该节点只证明 SBL boot log 数据面可访问，不表示 AMSS/Mission、QMI、MBIM、SIM、语音或蜂窝数据已经支持。

后续处理：保持它为只读 bring-up 诊断工具，不加入 firehose、EDL、NV 或未知 payload 写入。M39/M42 复测确认 BL 日志可读、Sahara HELLO 仍可读，HELLO_RESP 的 UL completion 也能主动 drain 到，但 SBL 没有继续发 READ_DATA/后续包；M42 的 AP2MDM_STATUS low→high 诊断未改变这一点，下一步应评估独立的 PON modem warm reset/ESOC 生命周期，而不是把 BL 视为 Mission mode 入口。

### M43 PON modem warm reset 诊断

含义：Android downstream 的 SDX55M ESOC 首次上电会调用 PMIC PON modem warm reset，lmi 对应硬件在 SPMI USID8 的 `qcom,modem-reset` PON S2 寄存器上。M43 只在 `mhi-pci-generic` 的 SDX55M 诊断路径中通过 DTS phandle 直接写 USID8 PON S2 warm reset 序列，不修改通用 AP `qcom-pon` 重启路径，也不引入完整 ESOC。

当前影响：已验证 warm reset 序列能触发，AP 未异常重启，PCIe2 Gen3 x2 endpoint、`mhi-pci-generic`、signed `sbl1.mbn` 加载、SBL/M0、`/dev/mhi_bl0` 和 `/dev/mhi_sahara0` 均未回退。M43 读取到 2340 字节 BL log，Sahara 仍读到 48 字节 HELLO，HELLO_RESP 写入 48 字节并能看到 UL completion，但约 20 秒内仍没有 READ_DATA 或下一包；MDM2AP_STATUS/ERRFATAL 仍为 0。

后续处理：M43 说明单独的 PON warm reset 不足以解决当前阻塞，后续应继续检查完整 ESOC 生命周期、`ESOC_REQ_IMG` 语义、Sahara 后续镜像传输期望以及 SBL 后 event IRQ/tasklet 路径。该直接 SPMI 写法只保留为诊断补丁，不视为最终上游设计。

### SAHARA event ring 主动 drain

含义：M27/M28 证明 START completion 和 48 字节 HELLO event 已经由 modem 写入 MHI event ring；M29-M32 通过同步/周期性主动 drain 调用 event parser 后，可以处理 er0 上的 START/RESET completion 和 er1 上的 DL `SAHARA` transfer event。

当前影响：这把问题缩小到正常 IRQ/tasklet/event 调度路径，强制 shared MSI 不能单独解决；event ring 内容、TRE pointer、xfer callback 和 `mhi_sahara_diag` read path 本身已经被 M32 验证可用。M44 进一步验证 20ms 周期 drain 能处理 BL 2340 字节 log、Sahara 48 字节 HELLO、HELLO_RESP 的 UL completion 和 release 时的 RESET completion，但仍不能让 SBL 发出 READ_DATA 或下一包；`/proc/interrupts` 中 SDX55M BHI 为 1，MHI event MSI vector 仍全为 0，而同机 QCA6391 MHI MSI 正常递增。

后续处理：主动 drain 是诊断 workaround，不应作为最终通用 MHI 修复。下一步重点检查为什么 SDX55M SBL 不触发 event MSI、forced M0 是否绕过了正常 interrupt enable/order，以及 HELLO_RESP 后是否需要 Android ESOC request engine 的 `ESOC_REQ_IMG` / `ESOC_IMG_XFER_DONE` 语义配合。所有 modem firmware 继续放 ignored/local 路径，避免 EDL、firehose 或 NV 写入。

### M46 event context / MSI 映射诊断

含义：M46 在 debugfs 和实际 SAHARA event drain 日志中补充 event context 的 `ertype`、`msivec`、host `irq`、Linux IRQ 和 `intmod`，用于区分 host event context 配置错误与 modem 侧不发 MSI。

当前影响：M46 已验证 SDX55M `mhi0` event context 均为 `type: 0x1`，er0 `msivec: 1` / `linux_irq: 189`，er1 `msivec: 2` / `linux_irq: 190`；PCI sysfs 已给 endpoint 分配 188-194 七个 MSI，BHI IRQ 188 计数为 1。同一轮 BL/SAHARA 测试中，er1 上 BL log、Sahara HELLO 和 HELLO_RESP UL completion 都显示 `er_type 0x1 msivec 2 irq 2 linux_irq 190 intmod 0x0`，但 `/proc/interrupts` 中 SDX55M event IRQ 189-194 仍全为 0。

后续处理：M46 基本排除 host event context `msivec`/Linux IRQ 映射错误；当前更像 SDX55M SBL 阶段没有产生 event MSI，或完整 ESOC/request-engine 生命周期未让 SBL 进入预期的后续加载状态。

### M47 自动 Sahara HELLO_RESP 诊断

含义：M47 在 `mhi_sahara_diag` 中检测到 SBL 发送的 48 字节 Sahara HELLO 后，直接由内核构造并 queue HELLO_RESP，字段为 command 2、length 48、version/compatible/mode 继承自 HELLO、status 0，用来排除用户态脚本响应太慢导致 SBL 不继续发送的可能。

当前影响：M47 已验证自动 HELLO_RESP 能看到 ch2 UL completion，er1 event context 仍为 `type 0x1`、`msivec 2`、Linux IRQ 190；但 SBL 仍没有继续发送 READ_DATA 或下一包。当前阻塞点不在 HELLO_RESP 响应延迟或基本 UL completion 路径。

后续处理：下一轮应更接近 Android downstream UCI 行为，优先验证 SAHARA 是否需要在 userspace 打开前就 auto-start 并预排 RX buffer；同时继续避免 EDL、firehose、NV 写入和长期 rootfs 定制。

### M48 SAHARA probe auto-start 诊断

含义：M48 把 SAHARA 改为像 BL 一样在 probe 阶段立即 `mhi_prepare_for_transfer()` 并预排 RX buffer，测试 SBL 是否要求 userspace 打开前通道已就绪。

当前影响：M48 启动后 SAHARA ch2/ch3 START completion 正常，HELLO 在 probe 阶段到达，内核自动 queue HELLO_RESP；但随后没有看到 ch2 UL completion，BL ch25 START 在约 8 秒后超时，硬件状态变为 `device_ee INVALID_EE device_state SYS ERROR`，PCI recovery 失败。说明“probe 阶段过早启动 SAHARA 并立即回 HELLO_RESP”会破坏当前 SBL 时序，不能作为修复路线。

后续处理：M49 改为保留 BL 的 probe auto-start，SAHARA 延迟到 BL 之后再自动 start，用来区分“需要预排 RX”与“SAHARA 早于 BL/ESOC 时序导致 SYS_ERROR”。若仍无 READ_DATA，则回到 ESOC/request-engine 生命周期和 SBL 用户态加载语义分析。

### M49 SAHARA 延迟 auto-start 诊断

含义：M49 保留 BL channel 25 在 probe 阶段 auto-start，同时把 SAHARA ch2/ch3 延迟 2 秒后自动启动，避免 M48 中 SAHARA 早于 BL 触发 SBL SYS_ERROR。

当前影响：M49 已验证 BL START completion 和 2340 字节 BL log 恢复；SAHARA 延迟启动后 ch2/ch3 START completion 正常，48 字节 HELLO 到达，内核自动 HELLO_RESP 后能看到 ch2 UL completion，但 5 秒用户态读取窗口内仍只有 HELLO，没有 READ_DATA 或后续包；SDX55M event MSI 计数仍为 0。

后续处理：M49 排除了“userspace 打开前没有预排 RX buffer”和“HELLO_RESP 太慢”这两个阻塞点。下一步应测试 Sahara 其他 HELLO_RESP mode 是否有任何响应，或继续对照 Android 用户态 image loader / ESOC request-engine 的协议语义，而不是继续调整 SAHARA 打开时机。

### M50 最小 ESOC 生命周期诊断

含义：M50 在 SDX55M 诊断路径中保存 AP2MDM/MDM2AP GPIO descriptor，注册 MDM2AP_STATUS 和 MDM2AP_ERRFATAL 双边沿 IRQ，并在 MHI power-up/SBL 后以 500 ms 周期轮询 24 次，同时记录 AP2MDM_STATUS、AP2MDM_ERRFATAL、MDM2AP_STATUS、MDM2AP_ERRFATAL、cached EE/state、寄存器 EE/state 和 MHI pm_state。

当前影响：M50 已验证 AP2MDM_STATUS low→PON warm reset→150 ms→high 后，MDM2AP_STATUS 和 MDM2AP_ERRFATAL 全程为 0，对应 GPIO IRQ 195/196 计数也为 0；MHI 仍进入 `SECONDARY_BOOTLOADER` / `M0`，BL 2340 字节 log、SAHARA HELLO、自动 HELLO_RESP 和 ch2 UL completion 仍可复现，但没有 READ_DATA/后续包。`/proc/interrupts` 中 SDX55M BHI IRQ 188 为 1，MHI event IRQ 189-194 仍为 0。

后续处理：M50 说明当前 AP2MDM high 与单次 PON warm reset 没有触发 Android downstream 期望的 MDM2AP_STATUS/ERRFATAL 侧带变化，也没有让 SBL 进入后续 image request 阶段。下一步应对照 downstream ESOC request engine 的 `ESOC_REQ_IMG` / `ESOC_IMG_XFER_DONE` / `ESOC_BOOT_DONE` 时序，做最小化生命周期模拟或继续确认 Sahara HELLO_RESP mode/后续 image loader 语义；不要把 GPIO 轮询或 PON 直写当最终上游设计。

### M51 downstream SDX55M profile 对齐诊断

含义：M51 把 `qcom-sdx55m` 的 channel map、event ring 数量、event ring 大小和 MSI vector 分配按 Android downstream `kona-mhi.dtsi` 的 SDX55M MHI profile 对齐到当前主线 `mhi-pci-generic` 配置，重点验证 upstream profile 过小或 MSI 数量不足是否导致 SBL 阶段 event MSI 不触发。

当前影响：M51 已验证 `mhi-pci-generic` 可分配 10 个 MSI vector，并创建 17 个 event ring；BL/SAHARA 基线仍可复现，`/dev/mhi_bl0` 可读 SBL log，SAHARA HELLO、自动 HELLO_RESP 和 UL completion 仍正常。但 `/proc/interrupts` 中 SDX55M event IRQ 189-197 仍全为 0，MDM2AP_STATUS/ERRFATAL 仍为 0，HELLO_RESP 后仍没有 READ_DATA 或后续包。

后续处理：M51 基本排除“SDX55M channel/event profile 太小”或“MSI vector 数量不足”作为当前主阻塞点。下一步应继续对照 Android ESOC request engine 和 Sahara image-loader 语义，重点检查 `ESOC_REQ_IMG` / `ESOC_IMG_XFER_DONE` / `ESOC_BOOT_DONE` 时序、HELLO_RESP 后是否需要真实镜像请求/传输，以及 forced SBL/M0 诊断路径是否绕过了某个 downstream 生命周期事件。

### M52 Sahara HELLO 字段诊断

含义：M52 在 `mhi_sahara_diag` 中打印 SBL 发来的 Sahara HELLO 原始 12 个 32-bit word，并打印自动构造的 HELLO_RESP 字段，同时在 HELLO_RESP UL completion 后继续 10 秒主动 drain event ring，用来确认当前卡住是否由 HELLO_RESP version/compatible/status/mode 字段错误引起。

当前影响：M52 实测 HELLO 为 `cmd=1 length=48 version=2 compatible=1 max_cmd_len=1024 mode=0`，自动 HELLO_RESP 为 `cmd=2 length=48 version=2 compatible=1 status=0 mode=0`。HELLO_RESP 能产生 ch2 UL completion，结果为 `status 0 bytes 48`；10 秒观察结束时 cached/reg 状态仍为 `SECONDARY_BOOTLOADER` / `M0`，没有 READ_DATA 或下一包。`/proc/interrupts` 中 SDX55M BHI IRQ 为 1，event IRQ 189-197 仍全为 0，MDM2AP_STATUS/ERRFATAL IRQ 仍为 0。

后续处理：M52 基本排除 HELLO_RESP 字段构造、mode 选择和用户态响应延迟作为当前主阻塞点。下一步不要继续扩大 MHI profile 或调整 SAHARA 打开时机，应对照 Android downstream ESOC request-engine 与 Sahara image-loader 语义，重点检查 `ESOC_REQ_IMG`、真实 image transfer、`ESOC_IMG_XFER_DONE`、`ESOC_BOOT_DONE` 和 MDM2AP_STATUS 的时序关系。继续避免 EDL、firehose、NV 写入、modem 分区写入和未知 payload。

### M53 ESOC_REQ_IMG 到 MHI power-up 窗口诊断

含义：downstream ESOC 首次 power-on 会在 AP2MDM_STATUS 拉高后向 userspace queue `ESOC_REQ_IMG`，再由 userspace 建立 MHI/UCI/Sahara 链路并传镜像。M53 在当前诊断路径里模拟这个 request-engine 窗口：AP2MDM_STATUS 拉高后先记录 `ESOC_REQ_IMG` armed，等待 2 秒，再继续 `mhi_prepare_for_power_up()` / `mhi_async_power_up()`，只验证时序窗口是否影响 SBL 后续行为，不发送 image、firehose、NV 或未知 payload。

当前影响：M53 实测 2 秒等待窗口结束时，AP2MDM_STATUS=1、AP2MDM_ERRFATAL=0、MDM2AP_STATUS=0、MDM2AP_ERRFATAL=0；随后 MHI 仍能加载 signed `sbl1.mbn` 并进入 `SECONDARY_BOOTLOADER` / `M0`，`/dev/mhi_bl0` 和 `/dev/mhi_sahara0` 仍出现，BL log、Sahara HELLO、自动 HELLO_RESP 和 ch2 UL completion 仍正常。但 10 秒观察后仍无 READ_DATA 或下一包，MDM2AP_STATUS/ERRFATAL IRQ 仍为 0，SDX55M event IRQ 189-197 仍全为 0。

后续处理：M53 排除“AP2MDM_STATUS 拉高后太快启动 MHI / 没给 ESOC_REQ_IMG userspace 窗口”作为当前主阻塞点。下一步应进入真实 Sahara image-loader 语义核对：确认 Android userspace 在收到 `ESOC_REQ_IMG` 后对 SAHARA 设备发送的第一批命令/模式是否只是一条标准 HELLO_RESP，还是还有 open/ioctl/notify 顺序或 image table 选择；若没有 Android 运行日志，只能继续做标准 Sahara command-mode/read-data 级别的只读协议探测。

### M54 Sahara command-mode HELLO_RESP 诊断

含义：M54 在 `mhi_sahara_diag` 中保留 M49/M52/M53 的 BL 先启动、SAHARA 延迟启动和 10 秒 post-HELLO_RESP 主动 drain，只把自动 HELLO_RESP 的 mode 从继承 SBL HELLO 的 `0` 改成标准 Sahara command mode `3`，用来确认 SBL 是否会进入 command-mode 并返回短包或命令响应。

当前影响：M54 实测 SBL HELLO 仍为 `cmd=1 length=48 version=2 compatible=1 max_cmd_len=1024 mode=0`，自动 HELLO_RESP 为 `cmd=2 length=48 version=2 compatible=1 status=0 mode=3`，ch2 UL completion 仍为 `status 0 bytes 48`。10 秒观察结束时 cached/reg 状态仍为 `SECONDARY_BOOTLOADER` / `M0`，没有 `SAHARA RX short`、READ_DATA 或下一包；`/proc/interrupts` 中 SDX55M event IRQ 189-197 仍全为 0，MDM2AP_STATUS/ERRFATAL IRQ 仍为 0。

后续处理：M54 排除“HELLO_RESP 必须切 command mode 才会有响应”作为当前主阻塞点。下一步如果没有 Android 真实 ESOC/UCI/Sahara userspace 日志，应继续核对标准 Sahara image-transfer 语义和 Android userspace 在 `ESOC_REQ_IMG` 后的第一批操作，但仍不能发送 firehose、进入 EDL、写 NV、写 modem 分区或提交 firmware blob。

### M55 Android `mdm_helper` / Sahara image-loader 只读核对

含义：M55 不改内核、不发送 modem payload，只把现有 Android `super` 里的 `system`、`vendor`、`system_ext`、`odm` 按 liblp offset 以 `ro,noload` 临时挂载，核对 vendor 用户态在 `ESOC_REQ_IMG` 后实际由谁处理 Sahara image transfer。

当前影响：`vendor/bin/init.mdm.sh` 会在 `ro.baseband=mdm|mdm2` 时启动 `vendor.mdm_helper`；`vendor/etc/init/hw/init.target.rc` 中 `vendor.mdm_helper` 是 disabled core service，由 `vendor.mdm_launcher` 间接启动。`vendor/bin/mdm_helper` 字符串显示它依赖 `libmdmdetect.so` / `libmdmimgload.so`，通过 ESOC request engine 等待 `ESOC_REQ_IMG`，随后进入 “Beginning sahara image transfer”，完成后发送 `IMG_XFER_DONE` 和 `ESOC_BOOT_DONE`，并等待 `MDM2AP_STATUS`。Android `fstab.qcom` 会把 `modem` 分区只读挂到 `/vendor/firmware_mnt`，`init.qcom.rc` 再创建 `/firmware -> /vendor/firmware_mnt` symlink；`ueventd.rc` 也把 `/vendor/firmware_mnt/image/` 作为 firmware directory。`libmdmimgload.so` 内有 `SDX55M` / `/vendor/firmware_mnt/image/sdx55m` 的 Sahara 映射，包含 `apps.mbn`、`qdsp6sw.mbn`、`sbl1.mbn`、`aop.mbn`、`tz.mbn`、`acdb.mbn`、`hyp.mbn`、`multi_image_qti.mbn`、`multi_image.mbn`、`xbl_config.elf`、`apdp.mbn`、`devcfg.mbn`、`sec.elf`，并使用 `/dev/mhi_pipe_2` / `/dev/mhi_%s%s` 这类 MHI SAHARA 设备节点。当前 modem firmware 分区中实际文件名是 `xbl_cfg.elf` 而不是 `xbl_config.elf`，后续若实现 loader 必须先只读确认映射和文件名，不要假定可直接照搬字符串。

后续处理：M55 明确当前缺口不是继续扩大 MHI profile，而是缺 Android 用户态 `mdm_helper` / `libmdmimgload.so` 那一层真正的 Sahara image-transfer 语义。下一步应在不进入 EDL、不发送 firehose、不写 NV、不写 modem 分区、不提交 firmware blob 的前提下，设计一个只从本机 signed firmware 读取、按 SBL `READ_DATA` 请求被动发送镜像块的最小诊断 loader；在看到真实 `READ_DATA` 之前，不应继续尝试 SIM、APN、IMS、VoLTE 或 ModemManager。

### M55 手动 Sahara 模式验证

含义：M55 把 `mhi_sahara_diag` 改为默认不自动发送 HELLO_RESP，只保留 BL auto-start、SAHARA 2 秒延迟 auto-start、RX 预排和主动 drain；`auto_hello_resp` / `auto_hello_mode` 只作为显式诊断参数保留。

当前影响：M55 内核构建通过，boot 镜像 `/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-boot/builds/ubuntu-console/M55-sdx55m-sahara-manual/boot-linux-copydown-lmi.img` 已刷入并启动。实机日志显示 `mhi0_SAHARA` 注册为 `auto_hello_resp=0 auto_hello_mode=0`，`/dev/mhi_sahara0` 和 `/dev/mhi_bl0` 正常出现；用户态执行 `dd if=/dev/mhi_sahara0 bs=48 count=1` 能直接读到 `1 48 2 1 1024 0 0 0 0 0 0 0`，且没有内核自动 HELLO_RESP。随后用户态手动写入标准 image-transfer HELLO_RESP `2 48 2 1 0 0 0 0 0 0 0 0`，写入 48 字节成功并出现 UL completion `status 0 bytes 48`，但 10 秒内仍没有 READ_DATA 或下一包。这证明后续临时 loader 可以从用户态接管第一包，同时进一步排除“内核自动回复路径导致 SBL 不继续”的可能。

后续处理：下一步才是写最小用户态 Sahara image-transfer loader：先读 HELLO，再发送标准 image-transfer HELLO_RESP，然后等待真实 `READ_DATA`；在出现 `READ_DATA` 前不发送任何 firmware 块。loader 只能从本机只读 modem firmware 路径读取 signed blobs，不进入 EDL、不发送 firehose、不写 NV、不写 modem 分区、不把 firmware/blob/日志提交进仓库。

### M56 Sahara RX buffer sniff 诊断

含义：M56 在 `mhi_sahara_diag` 中给已投递的 SAHARA DL RX buffer 维护 `queued_rx` 列表；用户态或自动 HELLO_RESP queue 成功后，10 秒观察窗口内继续 20ms 主动 drain event ring，同时直接扫描仍在队列中的 RX buffer 前 12 个 little-endian word。这个诊断只用于确认是否存在“SDX55M 已把 READ_DATA 或下一包写进 host RX buffer，但没有生成/处理 completion event”的情况，不发送任何 firmware payload。

当前影响：M56 内核构建通过，boot 镜像 `/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-boot/builds/ubuntu-console/M56-sdx55m-rx-sniff/boot-linux-copydown-lmi.img` 已刷入并启动。用户态仍能从 `/dev/mhi_sahara0` 读到 48 字节 HELLO `1 48 2 1 1024 0 0 0 0 0 0 0`，写入标准 image-transfer HELLO_RESP `2 48 2 1 0 0 0 0 0 0 0 0` 后出现 UL completion `status 0 bytes 48`，但用户态 10 秒没有收到下一包。M56 新增的内核总结为 `SAHARA RX sniff summary: seen_nonzero=0 seen_read_data=0 last_cmd=0 last_len=0`，regdump 仍保持 `Device EE: SECONDARY BOOTLOADER state: M0`，SDX55M MHI event IRQ 189-197 仍全 0，MDM2AP_STATUS/ERRFATAL IRQ 198/199 仍为 0。

后续处理：M56 排除“READ_DATA 已经写入已投递 SAHARA RX buffer，只是 completion/MSI 没出来”作为当前主分支。下一步不要扩大 MHI profile，应继续定位为什么 SBL 在 HELLO_RESP 后根本不写下一包：重点转向完整 ESOC request-engine / Android `mdm_helper` 状态差异、SBL `TRDATA` / `mdmddr` 前置条件，或 AP/MDM sideband 生命周期差异；在看到真实 `READ_DATA` 前仍不能发送任何 firmware 块、firehose、EDL 命令、NV 或 modem 分区写入。

### M57 ESOC reset 前置到 PCI claim 之前

含义：M57 只调整 SDX55M 诊断用 ESOC reset/status 时序，把 PON warm reset、150ms 等待和 AP2MDM_STATUS 拉高前移到 `mhi_pci_claim()`、PCI enable、MSI 分配和 MHI register 之前，更接近 Android downstream 的 `ESOC_PWR_ON` 先于 client link power-on 顺序；不扩大 MHI profile，不发送 firmware payload。

当前影响：M57 内核构建通过，boot 镜像 `/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-boot/builds/ubuntu-console/M57-sdx55m-esoc-preclaim/boot-linux-copydown-lmi.img` 已刷入并启动。实机仍能进入 PCIe2 Gen3 x2、`mhi-pci-generic`、signed `sbl1.mbn`、`SECONDARY_BOOTLOADER` / `M0`，`/dev/mhi_sahara0` 和 `/dev/mhi_bl0` 正常出现；用户态读到 HELLO `1 48 2 1 1024 0 0 0 0 0 0 0`，写入 HELLO_RESP `2 48 2 1 0 0 0 0 0 0 0 0` 后仍只有 UL completion `status 0 bytes 48`。10 秒观察没有 READ_DATA 或下一包，RX sniff 总结仍为 `seen_nonzero=0 seen_read_data=0 last_cmd=0 last_len=0`，SDX55M MHI event IRQ 仍全 0，MDM2AP_STATUS/ERRFATAL IRQ 仍为 0。

后续处理：M57 排除“PON warm reset / AP2MDM_STATUS 执行得晚于 PCI/MSI/MHI 初始化”作为当前主阻塞点。下一步仍应收窄在 SBL 为什么不写 READ_DATA：继续对照完整 ESOC request-engine 状态、Android `mdm_helper` / `libmdmimgload.so` 的 image-loader 语义、SBL `TRDATA` / `mdmddr` 前置条件和 AP/MDM sideband 生命周期；在真实 READ_DATA 出现前继续禁止发送 firmware 块、firehose、EDL 命令、NV 或 modem 分区写入。

### Sahara image-transfer 首包语义核对

含义：主线 `drivers/accel/qaic/sahara.c` 的标准 Sahara loader 路径和当前手动测试一致：收到 HELLO 后 host 只应发送 HELLO_RESP，然后等待 device 侧发出 `READ_DATA`，再按 `image/offset/length` 被动返回对应数据。host 不应在没有 `READ_DATA` 的情况下主动推送 firmware/image bytes。

当前影响：本地标准实现构造的 HELLO_RESP 为 `cmd=2 length=48 version=2 version_compat=2 status=0 mode=<hello mode>`；M55 已实测 compat=1、compat=2 以及 command mode 变体都能产生 UL completion 但不能触发下一包。只读扫描当前 signed `sbl1.mbn` 发现 `TRDATA` 是 SBL 镜像 token 表中的真实条目，邻近 `EFS1/EFS2/EFS3/ACDB/SEC/QTI_MISC/OEM_MISC/QHEE/QSEE/.../XBLConfig`，同时存在 `%s Image Load, Start` 日志格式，和 BL log 停在 `TRDATA Image Load, Start` 的现象相符。

后续处理：Sahara 首包语义已经基本排除“还缺一个 host 主动首包”作为安全路线。下一步应继续只读核对 Android `mdm_helper` / `libmdmimgload.so` 对 SDX55M image-id 到文件/分区的映射，以及 ESOC request-engine 在 `ESOC_REQ_IMG`、`ESOC_IMG_XFER_DONE`、`ESOC_BOOT_DONE` 前后的状态切换；除非看到真实 `READ_DATA`，仍不能发送任何 firmware 块、firehose、EDL 命令、NV 或 modem 分区写入。

### Android modem loader 只读复核

含义：在当前 Ubuntu rootfs 下只读解析设备 `super` 动态分区 metadata，临时以 `ro,noload` 挂载 Android vendor logical partition，并只读检查 `vendor/bin/init.mdm.sh`、`vendor/bin/mdm_helper` 和 `vendor/lib64/libmdmimgload.so` 字符串；同时只读检查已挂载的 modem firmware 分区 `/mnt/lmi-firmware-modem/image/sdx55m`。该步骤不写 super/vendor/modem/mdmddr，不运行 Android loader，不向 modem 发送 payload。

当前影响：`init.mdm.sh` 只在 `ro.baseband=mdm|mdm2` 时启动 `vendor.mdm_helper`。`mdm_helper` 字符串显示状态机会等待 ESOC request，然后切到 SAHARA：包含 `ESOC_WAIT_FOR_REQ`、`switching state to SAHARA`、`Beginning sahara image transfer`、`IMG_XFER_DONE`、`ESOC_BOOT_DONE`、`/dev/mhi_%s%s` 和 `/dev/mhi_pipe_10`。`libmdmimgload.so` 中 SDX55M 映射再次确认：`6:apps.mbn`、`8:qdsp6sw.mbn`、`21:sbl1.mbn`、`23:aop.mbn`、`25:tz.mbn`、`29:acdb.mbn`、`33:hyp.mbn`、`36:multi_image_qti.mbn`、`37:multi_image.mbn`、`38:xbl_config.elf`、`40:apdp.mbn`、`41:devcfg.mbn`、`42:sec.elf`、`34:/dev/block/bootdevice/by-name/mdmddr`，并有 `9:mdmddr` 特殊项和 `/dev/mhi_pipe_2`。进一步从 stock `vendor.img` 只读解出 `/vendor/bin/ks` 后确认，`libmdmimgload.so` 的 `mdm_img_transfer()` 会 fork/exec `/vendor/bin/ks`，命令行包含 `-o`、`-p <mhi_port>`、`-w <path>`、`-r -1`、`--partition_path /dev/block/bootdevice/by-name/` 和 Sahara image 映射；真正的 Sahara 状态机在 `ks` 内。`ks` 字符串包含 `SAHARA_WAIT_DONE_RESP`、`SAHARA_WAIT_DONE_RESP recieved with SAHARA_MODE_IMAGE_TX_PENDING=0x%.8X`、`Still More images to be uploaded, entering Hello wait state`，说明 `DONE_RESP 6 12 0` 对 Android kickstart 路径应按“仍有 image transfer pending，回到 HELLO wait”理解，而不是全流程完成。modem 分区实际文件包括 `xbl_cfg.elf` 而不是 `xbl_config.elf`，还包括 40 字节 `mdmddr.mbn`；`/dev/disk/by-partlabel/mdmddr` 分区大小为 1 MiB，首 word 也是 34，和 image-id 34 / `TRDATA` 方向吻合。

后续处理：当前证据进一步支持“Android 用户态 loader 负责完整 Sahara image transfer，kernel ESOC 只触发 request/状态通知”的模型，但仍不能解释 SBL 为什么在 HELLO_RESP 后完全不发第一条 READ_DATA。下一步应比较 Android loader 打开 `/dev/mhi_pipe_2` / `/dev/mhi_%s%s` 前后的设备节点语义，以及 current `mhi_sahara_diag` 是否缺少 downstream UCI/Sahara 设备打开时带来的 channel state/doorbell/queue 行为；仍不得在 READ_DATA 前发送任何 firmware 块或写 modem/NV/分区。

### M58 downstream UCI-like SAHARA open 行为诊断

含义：M58 把 `mhi_sahara_diag` 的 SAHARA 路径改得更接近 Android downstream `mhi_uci`：BL 仍在 probe 阶段自动启动；SAHARA 不再无人打开时延迟 auto-start，而是在首次 userspace open 时 prepare/start，并把 DL RX ring 预排到接近整环的 127 个 TRE；close 时 unprepare/reset SAHARA。该诊断仍不发送 firmware payload、firehose、EDL、NV 或 modem 分区写入。

当前影响：M58 内核构建通过，boot 镜像 `/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-boot/builds/ubuntu-console/M58-sdx55m-uci-like-sahara/boot-linux-copydown-lmi.img` 已刷入并启动。首次 open 读到 48 字节 HELLO `1 48 2 1 1024 0 0 0 0 0 0 0`，用户态写入 HELLO_RESP `2 48 2 2 0 0 0 0 0 0 0 0` 后仍只有 UL completion，10 秒没有下一包。close 触发 SAHARA reset 后再次 open，首个包变成 20 字节 READ_DATA，内核日志为 `SAHARA RX short len 20 word_count 5 words: 3 20 34 0 40 0 0 0 0 0 0 0`，表示 SBL 请求 image 34、offset 0、length 40；旧用户态脚本仍按 HELLO 逻辑等待 48 字节并误写 HELLO_RESP，随后收到 `END_OF_IMAGE`：`4 16 34 0`。这说明 M58 已经把 SBL 推到 image 34 / `TRDATA` / `mdmddr` 请求阶段，但旧探测脚本不能再使用“首包必为 HELLO”的假设。

后续处理：下一步应实现最小只读、被动 Sahara loader：通用解析 HELLO / READ_DATA / END_OF_IMAGE，只在合法 HELLO 后发送 HELLO_RESP，只在真实 READ_DATA 后按 image id 34 从本机只读 `mdmddr`/`mdmddr.mbn` 来源返回请求的 40 字节，并记录后续请求；不要主动推送未请求数据，不进入 EDL/firehose，不写 NV 或 modem 分区。若要继续测试，需要先重启恢复 modem 状态，因为误写后的 close/reset 已可能让 SDX55M 进入 SYS_ERROR。

### M59 最小只读 Sahara image 34 loader

含义：M59 在项目脚本中新增 `lmi/scripts/lmi-sahara-image34-loader.py`，作为临时用户态诊断 loader。它打开 `/dev/mhi_sahara0` 后通用解析 HELLO / READ_DATA / END_OF_IMAGE：只在合法 HELLO 后发送 HELLO_RESP，只在真实 READ_DATA 后处理 image id 34，只从 `--image34` 指定的本机只读来源按 offset/length 读取并返回请求范围，END_OF_IMAGE status 0 后发送 DONE。

当前影响：该 loader 已完成脚本级实现和 Python 语法检查。与 `/tmp` 早期原型不同，项目内版本在 image34 读取失败、短读、未知 image id 或请求长度超过 `--max-chunk` 时直接停止，不会补零或主动发送未请求 payload。rootfs 支持层已同步安装同一 loader 到 `/usr/local/bin/lmi-sahara-image34-loader`，并新增 `/usr/local/sbin/lmi-sahara-image34-test` 作为双次 open 测试和 dmesg 采集入口；本地语法与 whitespace 检查通过。接手清理时已从 `mhi_sahara_diag` 移除未验证的 post-write RX sniff/observe 逻辑，保留 M58 的 open 时 start、127 个 SAHARA DL TRE 预排和 close 时 unprepare/reset 行为。2026-05-20 通过 SSH 在一版仍带旧 post-write observe 逻辑的运行中 boot 上临时从 stdin 运行该 loader，open 后首包即为 `READ_DATA 3 20 34 0 40`，loader 从 `/dev/disk/by-partlabel/mdmddr` 返回 40 字节且 UL completion 正常；但 30 秒内没有 `END_OF_IMAGE` 或后续 READ_DATA，关闭通道 reset 时 SDX55M 进入 `SYS_ERROR`，PCIe endpoint `0002:01:00.0` 停在 D3cold 且 `/dev/mhi_sahara0` / `/dev/mhi_bl0` 消失。因此该结果只能说明 image34 payload 正确送达后 SBL 没有继续请求下一包，不能视为干净 M59 boot 的完整验证。随后刷入干净 M59 boot `#88`，确认无旧 `post-hello` / `observe` / `sniff` 日志；使用 stock `NON-HLOS.bin` 内 `image/sdx55m/mdmddr.mbn` 作为 image34 来源复测，payload 为 40 字节 `220000000300000000000000ffffffff000000000000000000000000000000000000000000000000`。干净 M59 上首次 open 仍为 HELLO→HELLO_RESP→30 秒 timeout，第二次 open 收到 `READ_DATA 3 20 34 0 40`，返回 stock 40 字节后 UL completion `status 0 bytes 40` 正常，但 30 秒内仍无 `END_OF_IMAGE` 或后续 READ_DATA；本次 close/reset 正常完成，`/dev/mhi_sahara0` 与 `/dev/mhi_bl0` 仍存在，未复现 `SYS_ERROR`/D3cold。stock `mdmddr.mbn` 与 live `/dev/disk/by-partlabel/mdmddr` 前 40 字节不同，但两者都不能让 SBL 继续请求下一包。

后续处理：干净 M59 已确认 image34 传输链路本身可写通，但 live `mdmddr` 和 stock `image/sdx55m/mdmddr.mbn` 都停在 40 字节 UL completion 后。下一步应把 `NON-HLOS.bin` 中 `image/sdx55m` 的启动镜像集合、`image/sdx55/qdsp6m.qdb` 和 `image/modem_pr` 的 MCFG/SO 配置树作为 Android loader 语义参考，继续核对 `mdm_helper` / `libmdmimgload.so` 在 image 34 之后是否还推进 ESOC/sideband 状态、发送特定 Sahara 命令、切 `/dev/mhi_pipe_2`，或采用不同的 close/reset 时序。M58 中旧脚本误在 READ_DATA 后写入 48 字节 HELLO_RESP 并触发 `END_OF_IMAGE` 只能作为线索，不能作为修复路线；仍只允许在真实 READ_DATA 后返回请求范围，禁止补零、额外发送未请求 image bytes、EDL、firehose、NV 写入、modem 分区写入和提交 firmware blob。

### M60 live `mdmddr` / `msadp` multi-image Sahara 诊断

含义：在 M59 的 image34-only loader 之后，临时 `/tmp/lmi_sahara_multi_loader.py` 按 Android `libmdmimgload.so` 的 SDX55M image-id 映射扩展为 multi-image 被动 loader。它仍然只在真实 `READ_DATA` 后返回请求范围，不主动推送未请求 bytes；image 34 优先从 live `/dev/disk/by-partlabel/mdmddr` 只读读取，image 40 优先从 live `/dev/disk/by-partlabel/msadp` 只读读取，其他 signed images 从 modem FAT `image/sdx55m/*` 只读读取。

当前影响：2026-05-20 实测确认 FAT 内 `image/sdx55m/mdmddr.mbn` 只有 40 字节，不能等价于 Android image 34 来源；切到 live `mdmddr` 后，SBL 在 `READ_DATA 3 20 34 0 40` 之后继续请求 `READ_DATA 3 20 34 40 1596`，loader 成功被动返回合计 1636 字节，随后收到 `END_OF_IMAGE 4 16 34 0`，发送 `DONE 5 8` 并收到 `DONE_RESP 6 12 0`。同一轮 DONE_RESP 后又收到新的 HELLO `1 48 2 1 1024 0 ...`，回复标准 mode 0 HELLO_RESP 后，下一次 open/restart 收到真实 image 40 请求 `READ_DATA 3 20 40 0 52`，从 live `msadp` 返回 52 字节且 UL completion 正常。并行读取 `/dev/mhi_bl0` 得到 SBL log，停在 `TRDATA Image Load, Start` 附近；BL 读端随后因设备/channel 消失返回 `ENODEV`。

后续处理：M60 把当前阻塞点推进到 image 40 / `msadp` 之后。image40 返回 52 字节后，90 秒内没有继续收到 `END_OF_IMAGE` 或下一条 `READ_DATA`；随后用户态 close 触发 SAHARA channel reset，其中 DL chan3 reset completion 正常，但 UL chan2 reset 超时，硬件变为 `device_ee INVALID_EE device_state SYS ERROR`，PCIe 进入 AER/D3cold 恢复失败并移除 `/dev/mhi_sahara0` / `/dev/mhi_bl0`。因此后续不能把 image40 后的观察结束等价为普通 close/reset；需要让 image40 后保持通道或安全跳过 release-time unprepare/reset，再观察 SBL 是否继续请求 image 40 后续范围、切换到其他 image id，或进入 Mission/其他 MHI channel。SYS_ERROR 后不要读取 `mhi0/oem_pk_hash` 等会访问不可达 PCI BAR 的 sysfs 属性；本轮该只读操作触发 `mhi_pci_read_reg()` 空指针 oops。

### M61 image40 后 keep-prepared 诊断

含义：M61 在 `mhi_sahara_diag` 中加入 `keep_prepared_on_release` 参数，用来验证 image40 payload 返回后，是否可以在用户态关闭或长时间观察时保留 SAHARA transfer prepared 状态，避免 close/release 触发 channel reset 把 SDX55M 推入 `SYS_ERROR`。用户态 `lmi-sahara-loader` 仍是被动 loader，只在真实 `READ_DATA` 后按请求范围读取 signed 本机来源；image 34 优先 live `mdmddr`，image 40 优先 live `msadp`。

当前影响：M61 boot `#89` 已验证 `/sys/module/mhi_sahara_diag/parameters/keep_prepared_on_release` 存在且默认 `N`，`/dev/mhi_sahara0` / `/dev/mhi_bl0` 可恢复出现。实测如果启动测试前就把 keep-prepared 置为 `Y`，SBL 只走到 HELLO/HELLO_RESP，后续 session 为空，说明 image34/image40 前仍需要原有 open/close reset 节拍推进；正确方向仍是初始保持 `N`，只在真实 image40 payload 已返回后由 loader 切到 `Y`。另一次把 idle timeout 缩到 30 秒的尝试被证明不安全：空 session 在 30 秒后 close/reset 会触发 SAHARA chan2 RESET timeout，设备进入 `device_state SYS ERROR`，PCIe 随后 D3cold/reset failed，`/dev/mhi_sahara0` / `/dev/mhi_bl0` 消失。rootfs 当前启动镜像未包含新 Sahara 工具，本轮仍需临时上传到 `/tmp` 执行。外层 image40 watcher 曾误把日志头里的 `hold_after_image=40` 当作真实 image40 命中，导致过早写入 keep-prepared；该触发条件已修正为只匹配真实 `read_data: image=40`。

补充验证：为核对标准 Sahara `DONE_RESP` 语义，曾把 `DONE_RESP status=0` 改成继续保持同一 fd 读取。干净重启后该版本能复现 HELLO、image34 两段 live `mdmddr` 传输、`END_OF_IMAGE`、发送 `DONE` 并收到 `DONE_RESP 6 12 0`，但在同一 fd 上 90 秒内没有新的 HELLO 或 image40 `READ_DATA`。随后 session close/reset 触发 SAHARA chan2 RESET timeout，设备进入 `device_state SYS ERROR`；该实验说明当前 MHI/UCI-like reset 节拍下不能把 `DONE_RESP` 后长时间 hold-fd 作为默认策略。本地和 rootfs 的 `lmi-sahara-loader` 已恢复为 `DONE_RESP status=0` 后快速 restart，避免默认路径复现该 SYS_ERROR。后续加入分阶段 restart delay 后，`DONE_RESTART_DELAY=2` / `DONE_RESP_RESTART_DELAY=2` 可以更快拿到 DONE_RESP，但没有触发 image40；修正后的 M60-like 复现使用 90 秒原始节拍、`DONE_RESP` 后同 fd 观察 180 秒，再开下一 session，在 600 秒外层有界窗口内仍没有真实 `read_data: image=40`。该轮停在 `DONE_RESP 6 12 0` 之后，没有出现历史 M60 中紧随 DONE_RESP 的新 HELLO；测试结束后 `/dev/mhi_sahara0`、`/dev/mhi_bl0` 仍存在，`keep_prepared_on_release=N`，未复现新的 `SYS_ERROR`/D3cold。

进一步按 Android `ks` 的 pending 语义测试 `DONE_RESP` 后才临时打开 keep-prepared：本轮以 `keep=N` 起步，90 秒原始 restart 节拍推进到 HELLO、image34 两段 live `mdmddr`、`END_OF_IMAGE`、`DONE` 和 `DONE_RESP 6 12 0`，随后 loader 把 `keep_prepared_on_release` 置为 `Y` 并在同一 fd 继续观察 75 秒，但仍无新 HELLO 或 image40 `READ_DATA`；再开一次 session 后 90 秒也为空，脚本以 `rc=0` 结束，`/dev/mhi_sahara0`、`/dev/mhi_bl0` 保持存在。测试结束时参数曾停在 `Y`，已手动恢复为 `N`，并且 loader 已调整为退出时恢复启动时的 keep-prepared 参数值，避免污染下一轮。dmesg 同期在 er1 上记录到 SAHARA `DONE_RESP` 后多个空 event 和一次 chan25 `BL` 48 字节 event / `Event element points to an unexpected TRE`；chan25 是 `MHI_CHANNEL_CONFIG_DL_SBL(25, "BL", ...)`，该现象暂记为 BL 通道线索，不按 SAHARA HELLO 或 image40 请求处理。

追加 sideband watcher 验证：在后续有界侧带监控运行中，GPIO 只记录到初始状态 `gpio1=low gpio3=low gpio56=high gpio57=low`，即 AP2MDM_STATUS 已高、MDM2AP_STATUS/ERRFATAL 仍低；本轮没有进入 Sahara 包交换，session 1-5 都是 open 后 90 秒 idle timeout、packets=0，session 6 open `/dev/mhi_sahara0` 返回 `EIO`。随后设备状态变为 `/dev/mhi_sahara0` 与 `/dev/mhi_bl0` 消失，dmesg 显示 SAHARA START timeout、`device_ee INVALID_EE`、`device_state SYS ERROR`、PCIe D3cold/reset failed。该结果不证明 sideband 状态能推进 image40，只说明在当前敏感状态下重复空 Sahara open/close 也会把 modem 推入 SYS_ERROR；后续不能在失败或 DONE_RESP 后状态上继续跑多轮空 session，应先普通 Linux reboot 恢复再做下一轮最小复测。

后续处理：`lmi-sahara-test` 默认 idle timeout 已恢复到 90 秒，不再用 30 秒空 session close 作为加速手段，也不要在 `DONE_RESP` 后长时间持有 fd，包括 DONE_RESP 后才临时启用 keep-prepared 的分支；也不要在已出现多轮空 session 后继续消耗 SAHARA open/close 次数。下一轮继续保持 `KEEP_PREPARED_ON_RELEASE=0` 起步，等待真实 image40 `READ_DATA` 被服务后再切到 `Y` 并保持 fd/transfer ring 观察；stock `ks` 已确认 `DONE_RESP status=0` 属于 `SAHARA_MODE_IMAGE_TX_PENDING` 分支，历史 M60 的可推进路径也是 `DONE_RESP` 后同 session 立即再收到 mode0 HELLO，然后下一次 open 收到 image40，但该行为当前不可稳定复现，需要继续比较 Android loader/ESOC 状态切换和现有 open/close reset 节拍，而不是补发未请求数据。主会话只做有上限的短状态采样或后台 watcher，不做无意义硬等。SYS_ERROR/D3cold 后只通过普通 Linux reboot 恢复，不读取 `mhi0/oem_pk_hash` 等 BAR-backed sysfs，不进入 EDL/firehose，不写 NV、modem、dtbo、recovery 或 vbmeta。

### M62 Android `ks` pending / 空 session 保护复测

含义：M62 把 Android `/vendor/bin/ks` 的 pending 语义拆成单独的有界诊断：image34 `END_OF_IMAGE status=0` 后发送 `DONE`，再通过 `--ks-pending-timeout` 在同一 fd 上等待 `DONE_RESP` 或后续 HELLO，避免把“close/reopen 触发出来的包”和 Android 同端口状态机混在一起。同时修正 loader 的空 session 保护，使 `EMPTY_LIMIT=1` 在全程 `total_packets=0` 时也会停止。

当前影响：首轮 `KS_PENDING_TIMEOUT=75` 复测在敏感状态下连续得到 0 包 session，旧保护条件因为要求 `total_packets > 0` 没有生效，导致多次空 open/close 后 `/dev/mhi_sahara0` 消失并进入 `SYS_ERROR` / PCIe D3cold；本轮只通过普通 Linux reboot 恢复。修正后干净复测稳定复现 HELLO、image34 live `mdmddr` offset 0/40 字节、offset 40/1596 字节、`END_OF_IMAGE 4 16 34 0`，host 发送 `DONE 5 8` 后内核记录 `SAHARA UL completion status 0 bytes 8`，但同 fd 等待 75 秒没有收到 `DONE_RESP`；随后 release/reset clean，`/dev/mhi_sahara0`、`/dev/mhi_bl0` 仍存在，`keep_prepared_on_release=N`。这说明当前 `mhi_sahara_diag` 的同 fd 等待并不能复现 Android `ks` 的连续端口 pending 行为，历史 M60/M61 中 `DONE_RESP` 和 image40 仍更像依赖 close/reopen/reset 节拍暴露出来的状态。

静态对比：Android downstream `drivers/bus/mhi/devices/mhi_uci.c` 的 SAHARA UCI 节点同样是在首个 open 时 `mhi_prepare_for_transfer()`，随后按可用 DL descriptor 预排 RX buffer；read 完一个 RX buffer 后立即重新 `mhi_queue_transfer(... DMA_FROM_DEVICE ... MHI_EOT)`；单个 Sahara 小包 write 也是 `MHI_EOT`；最后一个 fd release 时才 `mhi_unprepare_from_transfer()`。这与当前 `mhi_sahara_diag` 的 basic open/read/write/requeue/last-close 语义基本一致，说明 M62 同 fd 无 `DONE_RESP` 不能简单归因于用户态没 requeue RX、DONE 未写出或 close 太早。剩余差异更集中在当前诊断路径的 forced SBL/M0/event drain、exclusive misc node、keep-prepared/reset 诊断分支，以及 Android 完整 ESOC request-engine + downstream MHI core + `/dev/mhi_pipe_2` 组合。

MHI core 补充对比：Android downstream `mhi_boot.c` 在 firmware load 后等待真实 SBL EE event 唤醒；当前 lmi `host/boot.c` 为诊断加入了 500ms 后读取寄存器、必要时强制 `mhi_pm_m0_transition()` 并手动 queue SBL transition 的路径。Android downstream `mhi_prepare_channel()` / `__mhi_unprepare_channel()` 只按正常 command completion 处理 START/RESET；当前 lmi 对 SBL boot channel 增加了 event ring dump/drain，并且当设备寄存器显示 `SBL/M0` 且 channel context 已 `RUNNING` 时可把缺失 completion 当作成功。另一个更具体的差异是 data transfer completion：Android downstream `parse_xfer_event()` 会按 event 指向的 TRE 顺序推进并回收中间 descriptor；当前 lmi 主线 core 对非 next TRE 且未 chain 的 event 会报 `Event element points to an unexpected TRE` 并丢弃。本项目的 `mhi_sahara_diag` 会预排最多 127 个 DL RX buffer，M61 中 chan25 `BL` 48 字节 event / unexpected TRE 可能正落在这个差异上；它暂不能直接解释 M62 同 fd 无 `DONE_RESP`，但足以作为下一轮 M63 诊断候选：先只针对 SBL boot channel 记录并验证 downstream-style multi-TRE completion 处理，避免把有效 BL/SAHARA DL completion 当成异常丢掉。

后续处理：后续 pending 复测必须以 `keep=N` 起步，`EMPTY_LIMIT=1` 保护失败态，且不得在 DONE、DONE_RESP 或多轮空 session 后继续消耗 SAHARA open/close 次数。下一步重点不是补发未请求数据，而是比较 Android `/dev/mhi_pipe_2` / UCI 路径之外的生命周期差异，尤其是 ESOC request-engine、downstream MHI core state transition、当前 forced M0/event drain 诊断路径，以及 SBL boot channel 上的 multi-TRE completion 处理；仍禁止 EDL、firehose、NV 写入、modem 分区写入、dtbo/recovery/vbmeta 写入和 firmware blob 提交。

### M63 SBL boot channel multi-TRE completion 诊断

含义：M63 加入默认关闭的 `mhi.sbl_accept_multi_tre_completion` 诊断开关，只在手动置 `Y` 时允许 `SAHARA` / `BL` 这类 SBL boot channel 采用 Android downstream 风格的 multi-TRE completion 推进，默认路径仍保留主线的严格 `unexpected TRE` 检查。

当前影响：M63 `#90` 已 boot-only 刷入并启动，默认 `keep_prepared_on_release=N`、`sbl_accept_multi_tre_completion=N`，`/dev/mhi_sahara0` 和 `/dev/mhi_bl0` 正常存在。打开 `sbl_accept_multi_tre_completion=Y` 后，以 `KS_PENDING_TIMEOUT=75`、`EMPTY_LIMIT=1` 跑一轮只读 Sahara 复测：仍稳定走到 HELLO、live `mdmddr` image34 offset 0/40 与 40/1596、`END_OF_IMAGE 4 16 34 0` 和 host `DONE`；同 fd 等待 75 秒仍没有 `DONE_RESP`，close/reopen 后收到 `DONE_RESP 6 12 0`，再等 75 秒没有新 HELLO 或 image40，随后一轮 90 秒空 session 后由 `EMPTY_LIMIT=1` 停止。测试结束后 `keep=N`、`multitre=N`，`/dev/mhi_sahara0` / `/dev/mhi_bl0` 仍存在，没有触发 `SYS_ERROR` / D3cold。dmesg 同期确认 chan25 `BL` 的 48 字节 event 命中了新分支：`SBL channel 25 accepting multi-TRE completion ...`，说明 M63 能避免把该 BL completion 作为 `unexpected TRE` 丢弃，但它本身没有解锁 Sahara image40 或 Android `ks` pending 连续端口语义。

后续处理：M63 开关保持默认关闭，作为后续 BL/SBL 日志完整性诊断工具，不应作为默认修复提交。当前主阻塞仍在 Sahara/ESOC 生命周期语义：`DONE` 后同 fd 无 `DONE_RESP`、`DONE_RESP` 后无 HELLO/image40，以及当前 forced M0/event drain 诊断路径与 Android downstream MHI/ESOC 组合之间的差异。下一步应继续围绕完整 ESOC request-engine、真实 SBL event 等待和 channel START/RESET lifecycle 对齐做最小诊断，不补发未请求镜像数据，也不扩大到 NV、modem 分区或蜂窝业务栈。

### M64/M65 firmware-load fallback poll / forced M0 诊断

含义：M64/M65 把当前 SDX55M firmware-load 后的两个诊断 workaround 拆开验证：一是 500ms 后读取寄存器并手动 queue SBL/Mission transition 的 fallback poll，二是在寄存器显示 `MHI_STATE_M0` 但 host 仍未进入 M0 时调用 `mhi_pm_m0_transition()` 的 forced host M0。M64 首轮只把 `mhi.sbl_fw_poll_force_transition=0` 写进 Android boot header cmdline，运行时 `/proc/cmdline` 和 `/sys/module/mhi/parameters/sbl_fw_poll_force_transition` 证明该参数没有生效；当前 boot flow 的 Linux bootargs 来自嵌入 runtime DTB 的 `/chosen/bootargs`。

当前影响：M64b 改为在 runtime DTB bootargs 中设置 `mhi.sbl_fw_poll_force_transition=0` 后，Wi-Fi `mhi1` 保持可用，SDX55M PCIe2 endpoint 仍枚举，但 `mhi0` 不生成 `mhi0_SAHARA`、`mhi0_BL`、`/dev/mhi_sahara0` 或 `/dev/mhi_bl0`。dmesg 显示 `Skipping firmware-load fallback poll`，调试轮询显示 cached state 仍为 `PRIMARY BOOTLOADER` / `READY`，而硬件寄存器已经是 `SECONDARY BOOTLOADER` / `M0`；这说明当前 SDX55M 不是没进 SBL，而是 mainline host 没有自然收到或处理到对应 SBL/M0 transition。M65 再保留 fallback poll/queue，但用 `mhi.sbl_fw_poll_force_m0=0` 禁止 forced M0，结果会创建 `mhi0_SAHARA` 和 `mhi0_BL` sysfs 设备，但 `mhi_sahara_diag` probe 失败 `-EIO`，没有 `/dev/mhi_sahara0` 或 `/dev/mhi_bl0`；M65 default 恢复 `force_transition=Y` 与 `force_m0=Y` 后两个字符设备恢复，Wi-Fi 仍可用。

后续处理：当前 fallback register poll/queue 与 forced host M0 都仍是 SDX55M Sahara 诊断路径的必要 quirk，不能直接删除或默认关闭；但它们仍不是最终上游质量设计。下一步应定位 SDX55M 为什么不像 Android downstream 路径那样通过真实 SBL EE/M0 event 唤醒 host，或把该行为整理成受控的 SDX55M-specific quirk；继续避免把问题扩大到 Mission/AMSS、QRTR/QMI、SIM、蜂窝数据、EDL、firehose、NV 或 modem 分区写入。

### M66 Sahara restart / keep-prepared 语义诊断

含义：M66 不改 boot 镜像，只给项目内 `lmi-sahara-loader.py` 增加默认关闭的诊断选项，用来区分四种路径：`READ_DATA` payload 后同 fd 有界等待、payload 后 close/reopen、指定 image payload 后先启用 `keep_prepared_on_release` 再 close/reopen，以及 `END_OF_IMAGE` 后发送 `DONE` 立即启用 keep-prepared。loader 仍只在真实 `READ_DATA` 后被动返回请求范围，image34 使用 live `mdmddr`，image40 使用 live `msadp`，不发送未请求数据，不写 modem/NV/分区。

当前影响：在 M65 default 工作基线（fallback poll/queue 与 forced host M0 均启用，`keep_prepared_on_release=N`）上，`--read-data-follow-timeout=6 --ks-pending-timeout=6` 复测显示同 fd 等待不会收到下一包：HELLO 后 close/reopen 才得到 image34 offset 0/40，发送 40 字节后同 fd 等 6 秒无 offset 40 请求；close/reopen 后才得到 offset 40/1596，发送 1596 字节后同 fd 等 6 秒无 `END_OF_IMAGE`；再次 close/reopen 后才得到 `END_OF_IMAGE 4 16 34 0`，发送 `DONE` 后同 fd 等 6 秒无 `DONE_RESP`。该 DONE 后 release/reset 触发 SAHARA chan2 RESET timeout，设备进入 `SYS_ERROR` / PCIe D3cold，`/dev/mhi_sahara0` 和 `/dev/mhi_bl0` 消失；通过普通 Linux reboot 恢复。随后用 `--keep-prepared-after-image 34` 复测，image34 offset 0/40 后把 `keep_prepared_on_release` 置为 `Y` 并 close/reopen，结果后续两个 15 秒 session 都没有 offset 40 请求，脚本退出时恢复为 `N`，字符设备保持存在。

后续处理：当前可推进 image34 多段和 `END_OF_IMAGE` 的不是单纯 close/reopen，而是 release-time SAHARA RESET/下一次 START 的副作用；但在 DONE 之后继续依赖 release/reset 会把 modem 推入 `SYS_ERROR`。`read-data-follow`、`keep-prepared-after-image` 与 `keep-prepared-after-done` 保持默认关闭，只作为短窗口诊断开关。后续方向应从“盲目延长同 fd 等待”转向弄清 Android ESOC/`ks` 在 image 之间是否还有 `IMG_XFER_DONE`、channel reset/start 或 sideband 状态切换，并把这种节拍做成受控的 SDX55M-specific 生命周期，而不是在 DONE/DONE_RESP 或多轮空 session 后继续消耗 open/close/reset。

### M67 Sahara 分阶段 reset 安全默认

含义：M67 在 M65 default boot 基线上继续收敛 Sahara 生命周期，不扩大到 AMSS、SIM、蜂窝数据或 NV 写入。内核侧修正 `mhi_sahara_diag` 的 prepared-open 行为：只要设备仍处于 `prepared`，再次 open 都会尝试补 RX buffer，不再依赖当前 `keep_prepared_on_release` 参数仍为 `Y`。用户态 `lmi-sahara-loader.py` 同步改成分阶段安全默认：READ_DATA 阶段仍允许 close/reopen 利用当前 release-time RESET/下一次 START 节拍推进 image34 多段；但 `END_OF_IMAGE` 后发送 `DONE` 或收到 `DONE_RESP status=0` 时，默认设置 keep-prepared 并终止 session，避免 DONE/DONE_RESP 后继续 release/reset。旧的危险实验路径保留为显式 `--unsafe-done-restart`。

当前影响：M67 boot-only 镜像 `/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-boot/builds/ubuntu-console/M67-sahara-refill-prepared-20260520-1/boot-linux-copydown-lmi.img` 已只刷 `boot` 验证，hash 为 `4bfac1824a6c482c7397854dcb1fa9cb91d465553416f238c6db59625c1c1a96`。设备启动后 SDX55M 仍为 PCIe2 Gen3 x2 endpoint `17cb:0306`，`mhi0_SAHARA` / `mhi0_BL` 和 `/dev/mhi_sahara0` / `/dev/mhi_bl0` 均存在。prepared-refill 回归显示：image34 offset 0/40 后启用 keep-prepared 并恢复 `N`，下一次 open 仍不会自然出现 offset 40，但 8 秒超时后 close/reset 干净完成，未触发 `SYS_ERROR`。新版 loader 随后从待处理 offset 40 状态继续，收到 `READ_DATA image=34 offset=40 length=1596`，下一 session 收到 `END_OF_IMAGE 4 16 34 0`，发送 `DONE` 后自动把 `keep_prepared_on_release` 置为 `Y` 并停止，close 只记录 `SAHARA keeping transfer prepared on close`；退出时参数恢复为 `N`，字符设备仍存在，未复现 M66 的 DONE 后 chan2 RESET timeout / D3cold。

后续处理：M67 只是把当前可控边界推进到“安全发送 DONE 并停住”，还没有让 modem 进入 AMSS/Mission。下一步应继续对照 Android ESOC/`ks`：确认 DONE 后是否需要 `IMG_XFER_DONE`、`ESOC_BOOT_DONE`、MDM2AP_STATUS 边沿、受控 channel reset/start，或切到更接近 `/dev/mhi_pipe_2` 的 UCI 语义；不要再把 DONE/DONE_RESP 后 release-time reset 当默认推进手段。

### M69 SBL multi-TRE 默认接收与 BL auto-start 收敛

含义：M69 把 SBL boot channel 的 downstream-style multi-TRE completion 接收改为默认启用，并把被跳过的中间 TRE 作为零字节 completion 回收，避免把有效 BL/SAHARA completion 丢给用户态形成全零假包；同时把 `mhi_sahara_diag.bl_auto_start` 默认改为 `N`，避免 BL 只读诊断通道在 probe 阶段自动启动并干扰 SAHARA image-transfer 节拍。

当前影响：M69 boot 后默认 `sbl_accept_multi_tre_completion=Y`、`bl_auto_start=N`，`/dev/mhi_sahara0` 和 `/dev/mhi_bl0` 都存在。no-hold 只读 loader 复测能从干净状态稳定跑到 image34 两段 live `mdmddr`、`END_OF_IMAGE image=34 status=0`、host `DONE`、`DONE_RESP status=0`，随后再次收到 HELLO 并进入 image40/live `msadp`：offset 0/52、52/96、4096/6712、12288/1220。image40 请求范围已和 modem FAT `image/sdx55m/apdp.mbn` 以及 raw `/dev/disk/by-partlabel/msadp` 对齐；这排除了 APDP 数据源错误和 fake zero 包作为当前主阻塞。专门在 image40 offset 12288 后同 fd 等待 180 秒，仍没有 image40 `END_OF_IMAGE` 或后续 image id，请求停在 APDP 尾部之后。

后续处理：M69 后的主问题是 image40/APDP 传完后 SBL 不进入 `END_OF_IMAGE`，而不是 image34、`DONE_RESP`、BL auto-start 或 multi-TRE 伪包。后续实验应优先围绕 APDP 完成条件、ESOC `IMG_XFER_DONE`/`BOOT_DONE` 时序、MDM2AP_STATUS 边沿和受控 SAHARA reset/start cadence；仍只被动响应真实 `READ_DATA`，不补发未请求 bytes。

### M70 APDP tail sideband / MHI 状态采样

含义：M70 在 SDX55M PCI 设备上新增只读 sysfs 属性 `sdx55m_esoc_diag_state`，导出 AP2MDM_STATUS、AP2MDM_ERRFATAL、MDM2AP_STATUS、MDM2AP_ERRFATAL、cached/reg EE/state 和 pm_state；用户态 `lmi-sahara-loader.py` 新增 `--diag-state-path`、`--diag-after-read-data-image`、`--diag-after-read-data-min-offset`，在指定 `READ_DATA` 和 follow timeout 时同步记录该只读状态。

当前影响：M70 boot-only 镜像已刷 `boot` 并验证，设备启动后 `/sys/bus/pci/devices/0002:01:00.0/sdx55m_esoc_diag_state` 存在，基线状态为 `enabled=1 AP2MDM_STATUS=1 AP2MDM_ERRFATAL=0 MDM2AP_STATUS=0 MDM2AP_ERRFATAL=0 cached_ee=SECONDARY BOOTLOADER cached_state=M0 reg_ee=SECONDARY BOOTLOADER reg_state=M0 pm_state=0x4`。首次 APDP 采样命令因未加 `--unsafe-done-restart` 停在 image34 `END_OF_IMAGE` 安全默认；随后用 M69 已验证 cadence 重跑，日志 `/tmp/lmi-sahara-apdp-state-20260324-135444.log` 到达 image40 offset 12288/1220，并在发送后立即采样和 180 秒 follow timeout 后采样，两次状态完全一致：AP2MDM_STATUS 仍高，AP2MDM_ERRFATAL 为 0，MDM2AP_STATUS/ERRFATAL 仍为 0，MHI cached/reg 仍是 `SECONDARY BOOTLOADER` / `M0`，pm_state 仍为 `0x4`。后续只读复核显示 `/dev/disk/by-partlabel/msadp` 前 13508 字节与 modem FAT `image/sdx55m/apdp.mbn` sha256 同为 `aa25d0d47eafffea5e851a08343b5ff1a4c34622e77b035a973504d318d6d715`，四段已请求范围也逐段一致；dmesg 确认 image40 offset 12288/1220 后已有 `SAHARA UL completion status 0 bytes 1220`。本轮没有收到 image40 `END_OF_IMAGE` 或后续 image 请求；外层有界 timeout 终止后 `keep_prepared_on_release=N`，字符设备仍可由后续普通状态检查管理。

后续处理：M70 证明“APDP 最后一段后只是需要再等一会儿”不是当前主分支，且 MDM2AP_STATUS 没有在 image40 tail 或 180 秒窗口内拉高。下一步应对照 Android downstream/`ks` 在完成 APDP 后是否会通知 ESOC `IMG_XFER_DONE`、切换 AP2MDM/MDM2AP 状态、执行特定 SAHARA channel restart，或继续请求其他 image 前需要某个 host-side lifecycle 事件。仍不要把工作扩大到 AMSS/Mission 以外的蜂窝业务栈，也不要进入 EDL/firehose、写 NV、写 modem 分区或提交 firmware blob。

### M71 image40 EOI 与 APDP restart cadence

含义：M71 不改 boot 镜像，直接在 M70 基线上做 no-code cadence 复测，用来区分 APDP 从 offset 0 起长时间同 fd hold、APDP 每段后 release-time RESET/下一次 START，以及短 restart delay 被后续 RX 取消这三种行为。loader 仍只在真实 `READ_DATA` 后被动返回请求范围，image34 只读 live `mdmddr`，image40 只读 live `msadp`，不发送未请求 bytes。

当前影响：干净 SBL/M0 上，`--hold-after-image 40` 会在 image40 offset 0/52 之后停住 240 秒，offset 52/96、4096/6712、12288/1220 均不出现，说明不能从 APDP 首包起保持同一 fd。另一轮把 `DONE_RESP` 后 restart delay 压到 0 会在消费后续 HELLO 前关闭，导致 image40 不再出现，说明 `DONE_RESP` 后的新 HELLO 仍必须被读取并完成 HELLO_RESP。最终有效复测使用 `--unsafe-done-restart`、READ_DATA 后 1 秒 restart delay、`hold_after_image=-1` 和 8 秒短 idle：image34 两段、`END_OF_IMAGE image=34 status=0`、`DONE_RESP status=0` 后收到新 HELLO，HELLO_RESP 完成后短 idle close/reopen；随后 image40 先按 offset 0/52、52/96 分 session 推进，在 offset 4096/6712 后同 session 立即收到 offset 12288/1220，并继续收到 `END_OF_IMAGE image=40 status=0`。dmesg 同步确认 final APDP chunk、image40 `DONE` 和 HELLO_RESP 都有 `SAHARA UL completion status 0`；但 image40 `DONE` 后只收到新的 mode0 HELLO 和一个零包，没有后续 `READ_DATA`，MDM2AP_STATUS 仍为 0，MHI cached/reg 仍停在 `SECONDARY BOOTLOADER` / `M0`。

后续处理：M71 把阻塞点从“APDP 尾段后没有 image40 EOI”推进到“image40 DONE 后的新 HELLO/下一阶段握手无后续请求”。rootfs `lmi-sahara-test` 默认已把 `HOLD_AFTER_IMAGE` 从 40 改回 `-1`，避免从 image40 offset 0 起 hold 阻断后续 APDP 请求；后续应围绕 image40 DONE 后的 HELLO_RESP 语义、是否需要 ESOC `IMG_XFER_DONE` / `BOOT_DONE` 通知、MDM2AP_STATUS 边沿或受控 channel restart/start 继续缩小范围。仍不补发未请求镜像、不进入 EDL/firehose、不写 NV/modem/dtbo/recovery/vbmeta，也不提交 firmware blob。

### M72 image40 DONE 后 HELLO close/reopen 诊断

含义：M72 在 `lmi-sahara-loader.py` 中新增默认关闭的 `--done-hello-close-image` / `--done-hello-close-delay` 诊断开关，并在 rootfs `lmi-sahara-test` 中暴露 `DONE_HELLO_CLOSE_IMAGE` / `DONE_HELLO_CLOSE_DELAY`。该开关只在指定 image 的 `END_OF_IMAGE` 后发送 `DONE`，随后收到下一次 HELLO 并完成 HELLO_RESP 后生效，用来单独测试 image40 DONE 后的新 HELLO 是否需要立刻 close/reopen 才能推进后续阶段。

当前影响：干净 reboot 后按 M71 成功 cadence 复测，并且只额外叠加 `--done-hello-close-image 40 --done-hello-close-delay 1`。本轮稳定复现 image34 两段、`END_OF_IMAGE image=34 status=0`、`DONE_RESP status=0`、第二轮 HELLO、image40 offset 0/52、52/96、4096/6712、12288/1220 和 `END_OF_IMAGE image=40 status=0`。image40 `DONE` 后 er1 上先收到一个 12 字节零包，再收到新的 mode0 HELLO；host 完成 HELLO_RESP，dmesg 确认 image40 `DONE` 和该 HELLO_RESP 均有 `SAHARA UL completion status 0`。随后 M72 按开关在 1 秒后 close/reopen，但接下来的 3 个 session 均为 8 秒 idle、packets=0，最终只因 `EMPTY_LIMIT=3` 停止；只读状态仍是 `AP2MDM_STATUS=1 AP2MDM_ERRFATAL=0 MDM2AP_STATUS=0 MDM2AP_ERRFATAL=0 cached/reg=SECONDARY BOOTLOADER/M0 pm_state=0x4`，未出现 `SYS_ERROR` 或 `INVALID_EE`。

后续处理：M72 排除“image40 DONE 后的新 HELLO_RESP 必须立刻 close/reopen 才会触发下一批 READ_DATA 或 MISSION”这个分支。当前阻塞仍是 image40 DONE 后 SBL 只重新 HELLO 但不再请求镜像、MDM2AP_STATUS 不拉高、MHI 不离开 SBL/M0。下一步应转向最小 ESOC 生命周期通知/状态诊断，特别是 `IMG_XFER_DONE` / `BOOT_DONE` 的 AP-side request-engine 语义是否只影响 host 状态还是还会间接改变 modem 侧等待条件；继续保持被动 READ_DATA、安全只读来源和现有禁写边界。

### `qcom-pcie 1c10000.pcie: supply vdda/vddpe-3v3 not found, using dummy regulator`

含义：PCIe2 host driver 请求可选的 root complex 供电名，但当前 lmi DTS 只给 modem PCIe PHY 建模了 `vdda-phy` 和 `vdda-pll`。

当前影响：非致命；M16-M20 均已验证 PCIe2 Gen3 x2 link up，SDX55M endpoint 能枚举并绑定 MHI。

后续处理：不要为了消日志添加假 fixed regulator。只有后续确认 downstream 中存在可安全映射到主线 regulator 的真实 host supply，或出现 PCIe 稳定性/功耗问题时，再补齐该建模。
