# lmi 相机运行时总览

> 100% AI 编写：本文档由 AI 生成和整理。

本文记录当前 Rust/C camera runtime 的职责边界。历史重构计划见 [`RUNTIME_REFACTOR.md`](RUNTIME_REFACTOR.md)。

## 目标路径

当前可信输入仍是 OV13B10 RAW：

```text
OV13B10
  -> CSIPHY1 -> CSID1 -> VFE1 RDI0
  -> /dev/video3 truthful RAW10 pgAA
```

用户态支持层在 RAW 节点之上工作：

```text
/dev/video3 RAW pgAA
  -> lmi-camera Rust control plane
  -> lmi-isp C software ISP
  -> MJPEG/YUYV/NV12/FIFO/loopback/UVC helper
```

## 当前组件

| 组件 | 职责 |
| --- | --- |
| `lmi/camera-rs/` | Rust `lmi-camera` 控制面：media graph、route、native mode table、UVC configfs、demand-start、helper 生命周期。 |
| `lmi/scripts/lmi-isp.c` | C software ISP data-plane：RAW10 unpack、AE/AWB/statistics、demosaic、tone/gamma、YUYV/NV12/MJPEG 输出。 |
| `lmi/scripts/lmi-jpeg.h` | `lmi-isp` 使用的 tiny JPEG encoder，包含 FDCT/量化缩放修正。 |
| `lmi/scripts/lmi-uvc-gadget.c` | f_uvc OUTPUT feeder：PROBE/COMMIT、UVC queue、FIFO record、event FIFO。 |
| `lmi/scripts/lmi-venus-enc.c` | 手工实验用 Venus H.264 bridge：tight NV12 FIFO -> LMVC H.264 records。 |

## Rust 控制面职责

Rust `lmi-camera` 当前负责：

- 发现 media graph、video/subdev 节点和 devnode。
- 配置 OV13B10 -> CAMSS RDI route。
- 维护 OV13B10 native-six mode table。
- 将 UVC frame index 映射到 OV13B10 `mode_index`。
- 执行 RAW capture smoke test。
- 生成和校验 UVC configfs descriptor。
- 管理 UVC demand-start：枚举时空闲，host `STREAMON` 后才启动 RAW/ISP。
- 监督 C helper，并在 `STREAMOFF`/`DISCONNECT` 后按顺序清理。

## C data-plane 职责

C 仍保留性能敏感路径：

- `/dev/video3` MMAP RAW capture。
- packed RAW10 unpack 和 Bayer 采样。
- AE/AWB statistics 与 sensor control update。
- demosaic、downscale、gamma、tone、YUYV/NV12/MJPEG packing。
- UVC FIFO producer 和 f_uvc OUTPUT queue。

## 不可跨越边界

- `/dev/video3` 保持真实 RAW `pgAA`，不伪装成 processed node。
- software-ISP/UVC 是用户态支持层，不等于内核 ISP。
- `/dev/video20` / v4l2loopback 是本机标准节点目标，但 browser/PipeWire `getUserMedia` 完整体验仍需单独验证。
- H.264 UVC 是 Venus-gated 实验路径，不影响默认 MJPEG native-six。
- VFE480 processed YUV/RGB gate 继续关闭，见 [`VFE480_STATUS.md`](VFE480_STATUS.md)。
- 旧 production Python helper 已删除；未来 Python 只能作为明确不打包的 debug 工具。
