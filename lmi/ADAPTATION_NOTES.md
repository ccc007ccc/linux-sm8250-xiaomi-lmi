# lmi 主线适配备注

本文件记录已经能稳定工作的模块里仍会出现的非致命提示。后续新增驱动时，如果功能验证通过但日志仍有可解释的 warning，也按同样格式追加到这里，避免把已知提示和真正回退混在一起。

## 判定规则

- 已验证核心功能可用、不会阻塞启动或当前目标场景的问题，记录为非致命提示。
- 记录内容需要包含：日志关键字、含义、当前影响、后续处理条件。
- 如果提示开始影响启动、设备枚举、挂载、显示、输入、网络或稳定性，必须从本文件移出并当作待修复问题处理。

## UFS / userdata

当前状态：UFS 已作为第一阶段稳定基线使用。`ufshcd-qcom` 能绑定，UFS PHY 正常，`sda1` 到 `sda34` 能枚举，`sda34` 能以 F2FS 挂载，initramfs 能写入 `/data/adb/lmi-mainline-logs/` 并自动重启回 Android。

### `freq-table-hz property not specified`

含义：UFS 节点没有提供每个 UFS clock 的推荐频率范围，驱动使用当前 clock provider/default 频率继续运行。

当前影响：非致命；不影响 UFS 枚举、分区扫描、F2FS 挂载和日志写入。

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

当前状态：AMS667UU01 已能通过 MSM DRM/DPU/DSI 显示 fbcon 控制台。当前默认使用大字体和横向 fbcon：`fbcon=font:TER16x32 fbcon=rotate:1`。DRM 节点、DSI connector、backlight 节点均能出现。

### `arm-smmu 15000000.iommu: Unhandled context fault ... iova=0x9c... SID=0x820/0xc20`

含义：显示接管期间，MDSS/DPU 相关 stream 在访问 bootloader splash framebuffer 所在的 `0x9c000000` 附近地址时触发 SMMU fault。

当前影响：非致命；当前 DRM fbdev、DSI panel、backlight 和自动回 Android 均正常。

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
