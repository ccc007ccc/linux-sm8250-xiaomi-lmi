# lmi camera runtime refactor

> 100% AI 编写：本文档由 AI 生成和整理。

本文保留旧链接兼容。相机运行时详细记录已拆分到 `docs/camera/`：

- 当前 Rust/C 运行时总览：[`camera/RUNTIME_OVERVIEW.md`](camera/RUNTIME_OVERVIEW.md)
- 重构计划和迁移阶段：[`camera/RUNTIME_REFACTOR.md`](camera/RUNTIME_REFACTOR.md)
- software-ISP 与 UVC native-six：[`camera/SOFTWARE_ISP_UVC.md`](camera/SOFTWARE_ISP_UVC.md)
- Venus / H.264 实验链路：[`camera/VENUS_H264.md`](camera/VENUS_H264.md)
- 主索引：[`../CAMERA_BRINGUP.md`](../CAMERA_BRINGUP.md)

核心边界不变：`/dev/video3` 必须保持真实 RAW `pgAA`；software-ISP/UVC 是用户态支持层；VFE480 processed YUV/RGB 仍未支持；默认/public UVC profile 仍只暴露 OV13B10 native-six MJPEG frames。
