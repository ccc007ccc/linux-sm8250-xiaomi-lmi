# lmi 摄像头专题文档

> 100% AI 编写：本文档由 AI 生成和整理。

本目录保存 Redmi K30 Pro / POCO F2 Pro（`lmi`）主线 Linux 摄像头适配的专题记录。主入口见 [`../../CAMERA_BRINGUP.md`](../../CAMERA_BRINGUP.md)。

## 阅读顺序

1. [`../../CAMERA_BRINGUP.md`](../../CAMERA_BRINGUP.md)
   - 当前结论、传感器状态矩阵、公开口径和下一步。

2. [`sensors/OV13B10_ULTRAWIDE.md`](sensors/OV13B10_ULTRAWIDE.md)
   - 当前唯一已验证的传感器路径：OV13B10 -> CAMSS RDI -> `/dev/video3 pgAA`。

3. [`RUNTIME_OVERVIEW.md`](RUNTIME_OVERVIEW.md)
   - Rust/C runtime 当前架构、support layer 和不能跨越的 raw-only 边界。

4. [`SOFTWARE_ISP_UVC.md`](SOFTWARE_ISP_UVC.md)
   - software-ISP、native-six MJPEG UVC、opt-in 网络 MJPEG、按需启动和画质/性能约束。

5. [`VENUS_H264.md`](VENUS_H264.md)
   - Venus codec 与 H.264 UVC 实验链路。

6. [`VFE480_STATUS.md`](VFE480_STATUS.md)、[`VFE480_DIAGNOSTICS.md`](VFE480_DIAGNOSTICS.md)、[`VFE480_ANDROID_EVIDENCE.md`](VFE480_ANDROID_EVIDENCE.md)
   - 真 ISP/YUV 路径为什么仍关闭，以及 Android/CDM/寄存器证据如何使用。

7. [`VALIDATION.md`](VALIDATION.md)
   - 本地、实机、UVC、Venus 和 VFE 诊断命令。

8. [`PITFALLS.md`](PITFALLS.md)
   - 禁止事项、常见坑和下一步规则。

## 按传感器文档

| 传感器 | 文档 | 状态 |
| --- | --- | --- |
| OV13B10 后置超广角 | [`sensors/OV13B10_ULTRAWIDE.md`](sensors/OV13B10_ULTRAWIDE.md) | RAW 已验证，当前第一阶段目标。 |
| IMX686 后置主摄 | [`sensors/IMX686_WIDE.md`](sensors/IMX686_WIDE.md) | 暂缓。 |
| OV08A10 后置长焦 | [`sensors/OV08A10_TELE.md`](sensors/OV08A10_TELE.md) | 暂缓。 |
| S5K5E9YX04 后置微距 | [`sensors/S5K5E9YX04_MACRO.md`](sensors/S5K5E9YX04_MACRO.md) | 暂缓。 |
| GC02M1 后置景深 | [`sensors/GC02M1_DEPTH.md`](sensors/GC02M1_DEPTH.md) | 暂缓。 |
| S5K3T2 前置升降 | [`sensors/S5K3T2_FRONT_POPUP.md`](sensors/S5K3T2_FRONT_POPUP.md) | 暂缓，需先处理机械安全。 |

## 全局硬边界

- `/dev/video3` 保持真实 RAW `pgAA`，不伪装成 YUYV/NV12/RGB/MJPEG。
- VFE480 processed YUV/RGB 不作为当前可用功能；`vfe480_yc_pp_chain_configured()` 保持关闭。
- 用户态 software-ISP/UVC 是支持层，不等于内核 ISP。
- Android HAL/CHI/tuning/CDM/DMI/firmware 是参考证据，不能作为可复制到内核或 rootfs 的静态配置。
- stock blobs、signed firmware、DTB、firmware split 段和本地私有证据不提交。
