# Software ISP 与 UVC native-six

> 100% AI 编写：本文档由 AI 生成和整理。

本文记录 OV13B10 RAW 上方的用户态 software-ISP / UVC 支持层。

## 默认链路

```text
OV13B10 /dev/video3 pgAA
  -> lmi-camera Rust demand-start manager
  -> lmi-isp C software ISP
  -> MJPEG FIFO records
  -> lmi-uvc-gadget f_uvc OUTPUT feeder
  -> external USB host webcam
```

这条链路不改变内核边界：`/dev/video3` 仍是 RAW `pgAA`。

## Native-six profile

默认/public UVC profile 是 `native-modes`。它只 advertised 六个 OV13B10 原生 MJPEG frame：

| UVC frame | OV13B10 mode_index | 尺寸 | 名义帧率 |
| ---: | ---: | --- | ---: |
| 1 | 0 | `4208x3120` | 29.799 fps |
| 2 | 1 | `4160x3120` | 29.799 fps |
| 3 | 2 | `4160x2340` | 29.799 fps |
| 4 | 3 | `2104x1560` | 59.598 fps |
| 5 | 4 | `2080x1170` | 59.598 fps |
| 6 | 5 | `1364x768` | 120.069 fps |

Host UVC `COMMIT` frame selection 是 sensor mode selection：

```text
UVC advertised size == OV13B10 selected RAW size == lmi-isp output size
/dev/video3 == truthful RAW pgAA
```

禁止在默认/public profile 中加入 `640x480`、`1280x720`、`1920x1080` 等非原生 fallback。

## Demand-start 行为

开机默认 UVC service 只创建/绑定 gadget，让 host 能枚举 `UVC Camera`。此时不启动 OV13B10/CAMSS RAW stream，也不启动 `lmi-isp`。

事件行为：

- `COMMIT frame=N`：校验 host 宽高等于 native table，记录目标 sensor mode。
- `STREAMON`：配置 media route、设置 `/dev/video3 pgAA`，启动 `lmi-isp` 和 feeder。
- `STREAMOFF`/`DISCONNECT`：按 idle grace 停止 `lmi-isp`/RAW，保留 UVC gadget 枚举。
- 退出或切换：先停 manager/feeder/ISP，再解绑 UDC，再删除 configfs，避免 configfs 卡死。

## 已验证状态

- fastboot-booted copydown release image 中，默认 service 运行 Rust `lmi-camera run --output uvc --profile native-modes`。
- production/default path 不再运行 `lmi-camera-web-preview.py`。
- `lmi-camera uvc-status --assert-native-six` 在设备侧通过，frame 目录为 `f01_4208x3120` 到 `f06_1364x768`。
- Windows DirectShow 能看到六个 MJPEG 原生模式，没有旧 fallback。
- frame 1 会切到 OV13B10 mode 0，并配置 `/dev/video3 pgAA 4208x3120`。
- frame 6 会切到 OV13B10 mode 5，并配置 `/dev/video3 pgAA 1364x768`。

## MJPEG 画质和性能边界

- `lmi-jpeg.h` 包含 FDCT/量化缩放修正，避免强 8x8 方格回归。
- C ISP 在 DQBUF 后尽快复制 packed RAW10 并 QBUF 归还 camera buffer，后续处理在 `g_raw_copy` 上完成。
- 可在原生六档内部调整 JPEG quality、max-frame、thread count、CPU affinity、Bayer sampling/downscale 和 AE/AWB 参数。
- 大尺寸原生 MJPEG 帧率受 USB、host、CPU 和 JPEG 编码预算限制；不能因此偷偷 advertised 非原生低分辨率 fallback。

## 本机 loopback 方向

`/dev/video20` v4l2loopback 是本机 standard colour node 的目标：

```text
/dev/video3 RAW pgAA -> lmi-isp -> /dev/video20 YUYV/NV12/MJPEG-like userspace output
```

这仍是独立用户态节点，不是 `/dev/video3` processed output。PipeWire、browser `getUserMedia`、OpenCV、ffmpeg 等完整本机体验需要继续单独验证和记录。
