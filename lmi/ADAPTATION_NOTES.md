# lmi 主线适配备注

本文件记录已经能稳定工作的模块里仍会出现的非致命提示。后续新增驱动时，如果功能验证通过但日志仍有可解释的 warning，也按同样格式追加到这里，避免把已知提示和真正回退混在一起。

## 判定规则

- 已验证核心功能可用、不会阻塞启动或当前目标场景的问题，记录为非致命提示。
- 记录内容需要包含：日志关键字、含义、当前影响、后续处理条件。
- 如果提示开始影响启动、设备枚举、挂载、显示、输入、网络或稳定性，必须从本文件移出并当作待修复问题处理。

## UFS / userdata

当前状态：UFS 已作为主线 Ubuntu 控制台阶段的稳定基线使用。`ufshcd-qcom` 能绑定，UFS PHY 正常，`sda1` 到 `sda34` 能枚举；当前 `/dev/sda34` 已被替换为 Ubuntu rootfs，label 为 `ubuntu-rootfs`，initramfs 能挂载后 `switch_root` 进入 Ubuntu 24.04。

### `freq-table-hz property not specified`

含义：UFS 节点没有提供每个 UFS clock 的推荐频率范围，驱动使用当前 clock provider/default 频率继续运行。

当前影响：非致命；不影响 UFS 枚举、分区扫描、Ubuntu rootfs 挂载和 `switch_root`。

后续处理：等确认 stock/downstream DTS 中与当前 `clocks` 顺序匹配的频率表后再补 `freq-table-hz`，不要猜测填写。

### `vdd-hba-supply regulator ... assuming enabled`

含义：UFS host controller 的 `vdd-hba-supply` 还没有在 DTS 中建模，驱动假设该供电已经由 bootloader/PMIC 保持开启。

当前影响：非致命；当前 UFS probe 和 userdata 挂载稳定。

后续处理：确认 stock/downstream 中 `vdd-hba-supply` 对应的 RPMh regulator 后再补齐，避免错误 regulator 影响 probe 顺序或电源开关行为。

### `WB buf lifetime is exhausted 0x0B`

含义：UFS 设备上报 WriteBooster buffer lifetime 状态。它来自存储器件自身状态，不是 DTS 或主线适配错误。

当前影响：非致命；不影响当前读写和挂载验证。

后续处理：除非后续压力测试证明 WriteBooster 相关行为导致稳定性或性能问题，否则不处理。

## Display / Samsung AMS667UU01

当前状态：AMS667UU01 已能通过 MSM DRM/DPU/DSI 显示 fbcon 控制台。当前默认使用大字体和横向 fbcon：`fbcon=font:TER16x32 fbcon=rotate:1`。DRM 节点、DSI connector、backlight 节点均能出现。启动早期白屏/条纹已通过 lmi 专用 bootshim 在跳入 Linux 前拉低 GPIO46 panel reset 消除，随后 panel 驱动仍按正常流程重新初始化屏幕。

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

当前状态：FT3518 已能在 Ubuntu 中枚举为 `/dev/input/event0`，并确认真实触摸会上报 evdev 坐标事件。当前稳定路径是禁用硬件 `i2c13`，用 GPIO36/GPIO37 建立 `i2c-gpio` bitbang 总线；GPIO38 为 reset，GPIO39 为 IRQ，GPIO72 必须和 reset/IRQ 一起进入 active pinctrl，否则 0x38 不 ACK。

### `input: generic ft5x06 (48)`

含义：`edt-ft5x06` 能通过 FT3518 的通用 FocalTech 寄存器完成 probe，但该芯片的 ID 寄存器没有返回 EDT 风格型号字符串，所以驱动以 `generic ft5x06 (48)` 命名输入设备。

当前影响：非致命；`/dev/input/event0` 存在，ABS/MT 坐标和 BTN_TOUCH 事件可用。

后续处理：如果后续需要更准确的设备名、手势、固件管理或高级参数，再补 FT3518 专用识别和寄存器表；当前不要为改名增加不必要的协议分支。

### 硬件 `i2c13` 暂时禁用

含义：SM8250 GENI I2C13 在当前 DTS/GPI DMA 配置下会卡在 GPI RX DMA channel 分配，日志表现为 `EV ALLOCATE completion timeout` 和 `Failed to get rx DMA ch`。为先完成触摸功能验证，当前使用 `i2c-gpio` 绕开 GENI/GPI 路径。

当前影响：非致命；bitbang I2C 足够支撑触摸枚举和事件上报，但不是最终性能/架构最优路径。

后续处理：后续再单独修复 SM8250 GPI DMA/GENI I2C13，修复前不要恢复硬件 `i2c13` 触摸节点。
