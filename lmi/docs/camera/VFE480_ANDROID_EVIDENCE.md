# VFE480 Android 运行时证据

> 100% AI 编写：本文档由 AI 生成和整理。

本文记录用于理解 VFE480 PP/IFE module chain 的 Android-side evidence。所有 evidence artifacts 都是本地只读/临时测试产物，不提交。

## 证据边界

Android HAL/CHI/tuning/CDM/DMI 证据只能用于：

- 判断哪些 PP/IFE block 会被 Android runtime 编程。
- 比对 mainline 哪些窗口保持空白。
- 设计 clean-room、可解释、可最小化的主线模型。

不能用于：

- 把 register values、DMI tables、tuning constants 复制进主线内核。
- 要求 Linux rootfs 携带 Android HAL/tuning blobs。
- 把 Android working preview 写成 mainline V4L2/YUV 已支持。

## Stock 用户态线索

OV13B10 相关 stock 文件位于 `vendor.img`：

- `/vendor/lib64/camera/com.qti.sensor.ov13b10_lmi.so`
- `/vendor/lib64/camera/com.qti.eeprom.lmi_sunny_ov13b10_gt24p64.so`
- `/vendor/lib64/camera/com.qti.sensormodule.lmi_sunny_ov13b10_ultra.bin`
- `/vendor/lib64/camera/com.qti.tuned.lmi_sunny_ov13b10_ultra.bin`
- `/vendor/lib64/camera/com.qti.tuned.lmi_sunny_ov13b10_ultra_pro.bin`
- `/vendor/lib64/hw/camera.qcom.so`
- `/vendor/lib64/hw/com.qti.chi.override.so`
- `/vendor/etc/camera/camxoverridesettings.txt`

String-level inspection shows IFE/3A modules such as `linearization34_ife`、`demux13_ife`、`demosaic36_ife`、`demosaic37_ife`、`cc12_ife`、`cc13_ife`、`cst12_ife`、`gamma16_ife`、`dsx10_ife_video_full_dc4`、`lsc*`、`pdpc*`、`abf*`、`AECParamExtension`、`AWBExtensionParam`。这支持当前判断：Android 依赖 HAL/CHI/tuning userspace 生成 IFE/CDM module program。

## Android Camera ID 2 / OV13B10 事实

crDroid/Android 16 Aperture `0.7x` 运行时证据确认：

- Android Camera ID `2` 是 OV13B10 ultrawide。
- logcat 报告 `SensorCaps sensorId=3 positionType=3 activitysize(8,8,4208,3120) ov13b10_lmi`。
- `dumpsys media.camera` 显示 active streams 包括 `1600x1200` SurfaceView format `0x22` 和 `4000x3000` ImageReader format `0x21`。
- 这说明 Android 使用 HAL pipeline，不是 raw-only V4L2 browser node。

## Downstream runtime 关键状态

临时 Android release kernel instrumentation 捕获到：

- `core_blob`: `ctx=1`, `mux_pp=0`, `vid_ds16=0`, `vid_ds4=0`, `disp_ds16=1`, `disp_ds4=1`, `ihist=1`, `hdr_be=1`, `hdr_bhist=1`。
- `camif_start`: `vfe=1`, `core_cfg=0x60002f00`, active window `4208x3120`, `mux_pp=0`。
- 这解释了 mainline mux sweep 中 mux `0` 首次让 VFE1 PIX 看到 BUS/RUP activity。

Camera ID `2` acquired outputs 包括：

- `FULL_DISP`：WMs 4/5，comp group 1，`1600x1200` UBWC display processed output。
- `DS4_DISP` / `DS16_DISP`：display downscale。
- FD、HDR/TL/BF/AWB/BHIST/RS/IHIST stats。
- RDI0/RDI1 raw resources。

注意：Android sample 中 useful preview 是 `FULL_DISP`，不是 mainline `/dev/video6` RAW_DUMP client10，也不是简单线性 NV12 证明。

## CDM payload / DMI 证据

Active OV13B10 session 中，CDM IQ command buffers 会写大量 PP module windows：

- PP CAMIF CLC：`0x2660`、`0x2668`、`0x2670`、`0x2678`。
- PP CLC Modules：`0x2800..0x8ffc` 范围内多组 module windows。
- 重点窗口包括 `0x3090`、`0x3058/0x3068/0x30ac`、`0x3860..0x3e58`、`0x4060..0x6268`、`0x7e60..0x8e68`。
- DMI loads 目标包括 `0x3008`、`0x3408`、`0x3608`、`0x3c08`、`0x3e08`、`0x8208`、`0x8808`、`0xa608` 等。

Mapped DMI table dump 证明外部 DMI table 可以被临时 Android instrumentation 读出，但这些 table 是 HAL/CHI/tuning-generated 数据，不能复制。

## Post-CDM register 证据

Post-CDM snapshot 证明 CDM/DMI payload 反映到 live VFE registers：

- `ctx=1` PP CAMIF windows 非零。
- `0x2e00`、`0x3000`、`0x3400`、`0x3600`、`0x3c00`、`0x3e00`、`0x8000`、`0x8200`、`0x8800`、`0x8a00` 等 module windows 非零。
- display/FD/stats/RDI BUS clients 非零。
- 追加 missing-window pass 覆盖 `0x3260`、`0x3860`、`0x3a60`、`0x4060`、`0x4460`、`0x4660`、`0x4860`、`0x4a60`、`0x4c60`、`0x4e60`、`0x5060`、`0x5260`、`0x6060`、`0x6260`、`0x7e60`、`0x8460`、`0x8660`、`0x8890`、`0x8e60` 等窗口。

这使 mainline 对比更清楚：当前 mainline 只足够驱动 RAW_DUMP client10 诊断，缺少 Android-style PP/IFE module chain。

## Clean-room 反编译边界

只读反汇编/语义核对得到的结构事实：

- `IFEDemux13Titan480` 写 `0x3090` 7-word BPC/PDPC DEMUX block。
- `IFEDemosaic36Titan480` 写 `0x3860` 和 `0x3878`。
- `IFEWB13Titan480` 写 `0x3868` 4-word WB gain/offset block。
- `IFEPDPC30Titan480` 写 `0x3058`、`0x3068`、`0x30ac` 和 `0x3008` DMI selectors。
- `IFECC13Titan480` 写 `0x3a60` / `0x3a68`。
- `IFECST12Titan480` 写 `0x4060` / `0x4068`。
- VideoFull Y/C terminal 涉及 MNDS、crop、round/clamp 等窗口。

这些事实能帮助设计模块边界，但 tuning/runtime input 仍来自 HAL，不能静态照搬。

## 当前可用结论

- Android OV13B10 preview 依赖 broad PP/IFE module chain、DMI tables、stats/display/RDI resources。
- Mainline `/dev/video6` RAW_DUMP 不是 Android preview path。
- 仅复制 BUS/WM setup 不会产生 true YUV。
- 下一步需要独立构建 DEMUX + DEMOSAIC + color + terminal 的最小可解释模型，或继续保持 gate 关闭。
