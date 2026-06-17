# lmi 摄像头适配索引

> 100% AI 编写：本文档由 AI 生成和整理。

本文是 Redmi K30 Pro / POCO F2 Pro（`lmi`）主线 Linux 摄像头适配的主索引。详细记录已拆到 `lmi/docs/camera/`，避免主文档继续堆积长日志。

## 当前结论

- 当前唯一进入主线适配第一阶段的传感器是后置超广角 **OV13B10**。
- 已验证链路是标准 V4L2/media RAW RDI：

  ```text
  OV13B10 -> CSIPHY1 -> CSID1 -> VFE1 RDI0 -> /dev/video3
  ```

- `/dev/video3` 必须保持真实 RAW10 Bayer 节点，fourcc 为 `pgAA` / `V4L2_PIX_FMT_SGRBG10P`。
- 当前内核仍没有可用的 SM8250 VFE480 processed YUV/RGB 输出；`vfe480_yc_pp_chain_configured()` 必须继续关闭。
- `lmi-camera` Rust runtime + C `lmi-isp` / `lmi-uvc-gadget` 是用户态 software-ISP/UVC/network-MJPEG 支持层，不把 `/dev/video3` 伪装成 YUV/RGB/MJPEG。
- 默认/public UVC profile 是 `native-modes`，只暴露 OV13B10 六个原生 MJPEG frame。
- UVC 控制项 advertised 标准 AE mode、exposure time absolute、gain/ISO-like、power-line frequency（默认 auto 防频闪），并实验性 advertised UVC 1.5 ROI/测光矩形；Windows Camera 是否展示手动快门/ISO 或下发 ROI 仍必须看 DirectShow/设备日志确认。
- H.264 UVC 是 Venus-gated 实验链路，低分辨率 frame 6 已验证动态画面，但默认/public 仍是 MJPEG native-six。
- Network camera 是显式 opt-in 的测试输出，frame 6 `1364x768` 已验证 HTTP MJPEG、RTSP RTP/JPEG、ONVIF SOAP 和 WS-Discovery；默认网络预览限制到 30fps，initramfs netcam 默认传 50Hz 防频闪，ONVIF/WS-Discovery 默认设备名/scopes 为 `LMI-OV13B10` 以改善 Windows 发现列表；`lmi.netcam=1 lmi.netcam.rtsp=1 lmi.netcam.onvif=1` 开机路径可用，但默认 UVC service 不因此改变。

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [`docs/camera/README.md`](docs/camera/README.md) | 摄像头专题文档目录和阅读顺序。 |
| [`docs/camera/sensors/OV13B10_ULTRAWIDE.md`](docs/camera/sensors/OV13B10_ULTRAWIDE.md) | 已验证 OV13B10 路由、模式、metadata、控制项和 bring-up 结论。 |
| [`docs/camera/sensors/IMX686_WIDE.md`](docs/camera/sensors/IMX686_WIDE.md) | 后置主摄 IMX686 当前证据与暂缓状态。 |
| [`docs/camera/sensors/OV08A10_TELE.md`](docs/camera/sensors/OV08A10_TELE.md) | 后置长焦 OV08A10 当前证据与暂缓状态。 |
| [`docs/camera/sensors/S5K5E9YX04_MACRO.md`](docs/camera/sensors/S5K5E9YX04_MACRO.md) | 后置微距 S5K5E9YX04 当前证据与暂缓状态。 |
| [`docs/camera/sensors/GC02M1_DEPTH.md`](docs/camera/sensors/GC02M1_DEPTH.md) | 后置景深 GC02M1 当前证据与暂缓状态。 |
| [`docs/camera/sensors/S5K3T2_FRONT_POPUP.md`](docs/camera/sensors/S5K3T2_FRONT_POPUP.md) | 前置升降 S5K3T2 当前证据、机械边界与暂缓状态。 |
| [`docs/camera/RUNTIME_OVERVIEW.md`](docs/camera/RUNTIME_OVERVIEW.md) | Rust/C camera runtime 当前职责、数据流和边界。 |
| [`docs/camera/RUNTIME_REFACTOR.md`](docs/camera/RUNTIME_REFACTOR.md) | Rust 控制面、C data-plane、性能/迁移计划。 |
| [`docs/camera/SOFTWARE_ISP_UVC.md`](docs/camera/SOFTWARE_ISP_UVC.md) | software-ISP、native-six UVC、MJPEG、network MJPEG、按需启动和画质边界。 |
| [`docs/camera/VENUS_H264.md`](docs/camera/VENUS_H264.md) | Venus codec、H.264 UVC 实验链路、Windows 兼容结论。 |
| [`docs/camera/VFE480_STATUS.md`](docs/camera/VFE480_STATUS.md) | VFE480 processed YUV/RGB 当前状态和必须关闭的 gate。 |
| [`docs/camera/VFE480_DIAGNOSTICS.md`](docs/camera/VFE480_DIAGNOSTICS.md) | `/dev/video6` RAW_DUMP 诊断历史、r24-r47 结论和主线边界。 |
| [`docs/camera/VFE480_ANDROID_EVIDENCE.md`](docs/camera/VFE480_ANDROID_EVIDENCE.md) | Android HAL/CHI/CDM/DMI/post-CDM 证据和不可复制边界。 |
| [`docs/camera/VALIDATION.md`](docs/camera/VALIDATION.md) | 本地、实机 RAW、UVC、Venus 和 VFE 诊断验证命令。 |
| [`docs/camera/PITFALLS.md`](docs/camera/PITFALLS.md) | 摄像头适配坑点、禁止事项和下一步规则。 |

## 传感器矩阵

| 位置 / 用途 | 模组线索 | 当前状态 | 文档 |
| --- | --- | --- | --- |
| 后置超广角 | Sunny OV13B10 / `lmi_sunny_ov13b10_ultra` | 第一阶段目标；RAW `pgAA` 已验证，UVC native-six 支持层可用。 | [`OV13B10_ULTRAWIDE.md`](docs/camera/sensors/OV13B10_ULTRAWIDE.md) |
| 后置主摄 | Sunny IMX686 / `lmi_sunny_imx686_wide` | 暂缓；仅有 stock blob/DTS 线索，未接入主线。 | [`IMX686_WIDE.md`](docs/camera/sensors/IMX686_WIDE.md) |
| 后置长焦 | Sunny OV08A10 / `lmi_sunny_ov08a10_tele` | 暂缓；未接入主线。 | [`OV08A10_TELE.md`](docs/camera/sensors/OV08A10_TELE.md) |
| 后置微距 | Sunny S5K5E9YX04 / `lmi_sunny_s5k5e9yx04_macro` | 暂缓；未接入主线。 | [`S5K5E9YX04_MACRO.md`](docs/camera/sensors/S5K5E9YX04_MACRO.md) |
| 后置景深 | OFilm GC02M1 / `lmi_ofilm_gc02m1_depth` | 暂缓；未接入主线。 | [`GC02M1_DEPTH.md`](docs/camera/sensors/GC02M1_DEPTH.md) |
| 前置升降 | Sunny S5K3T2 / `lmi_sunny_s5k3t2_front` | 暂缓；必须先处理升降机构和位置安全。 | [`S5K3T2_FRONT_POPUP.md`](docs/camera/sensors/S5K3T2_FRONT_POPUP.md) |

## OV13B10 原生六档

默认 UVC/照片 `native-modes` profile 只暴露以下六个原生尺寸。UVC frame、OV13B10 `mode_index`、`/dev/video3 pgAA` RAW 尺寸和 `lmi-isp` 输出尺寸必须一一对应。

| UVC frame | OV13B10 mode_index | 原生尺寸 | 名义帧率 |
| ---: | ---: | --- | ---: |
| 1 | 0 | `4208x3120` | 29.799 fps |
| 2 | 1 | `4160x3120` | 29.799 fps |
| 3 | 2 | `4160x2340` | 29.799 fps |
| 4 | 3 | `2104x1560` | 59.598 fps |
| 5 | 4 | `2080x1170` | 59.598 fps |
| 6 | 5 | `1364x768` | 120.069 fps |

不允许在默认/public profile 中重新加入 `640x480`、`1280x720`、`1920x1080` 或其它非原生 fallback。

## 当前运行时边界

```text
/dev/video3 RAW pgAA
  -> lmi-camera Rust control plane
  -> lmi-isp C software ISP
  -> MJPEG/YUYV/NV12/FIFO/loopback/UVC/network-MJPEG helper
```

- Rust `lmi-camera` 负责 media route、sensor mode、UVC configfs、COMMIT 到 sensor mode 映射、按需启动和 helper 生命周期。
- C `lmi-isp` 负责 RAW10 unpack、AE/AWB/statistics、demosaic、tone/gamma、YUYV/NV12/MJPEG packing。
- C `lmi-uvc-gadget` 负责 f_uvc OUTPUT feeder、PROBE/COMMIT、FIFO record、UVC queue。
- C `lmi-venus-enc` 只用于手工 H.264/Venus 实验链路。
- 旧生产 Python helper 已删除；未来 Python 只能是明确不打包的 debug 工具。

## 公开说明口径

可以说明：

- OV13B10 后置超广角 RAW RDI 可用。
- `/dev/video3` 能按六个原生模式输出真实 `pgAA` RAW。
- UVC demand-start + software-ISP 用户态支持层可让 Windows/host 枚举 `UVC Camera`，默认 MJPEG native-six。Opt-in network camera 模式可通过 HTTP `/status`、`/snapshot.jpg`、`/stream.mjpg` 在可信局域网提供测试流，默认 30fps，并可显式打开 RTSP RTP/JPEG、ONVIF SOAP 和 WS-Discovery 让 VLC/ONVIF 工具更容易取到 stream URI；默认 ONVIF 名称为 `LMI-OV13B10`，但没有鉴权/TLS/账号管理；当前 RTSP 不是 H.264。
- Windows/DirectShow 可通过标准 UVC 控制项尝试调节自动曝光、快门/曝光时间、Gain/ISO-like 和工频防闪烁；software-ISP 的 auto 防闪烁会先做 row/band 检测再锁定 50/60Hz，并让曝光与 gain 协同。AE 对 ROI 测光做低通、deadband 和连续方向确认，小幅偏暗优先 analogue gain（ISO-like）到软噪声上限，随后才慢动真实快门，digital gain 最后使用。ISO 没有通用 UVC 标准控制，只能先按 Gain/ISO-like 映射；ROI/测光点已实验性按 UVC 1.5 advertised，但 Windows Camera 是否在点击取景器时下发控制不可靠，必须以 `ROI ...` / `meter_roi=...` 日志为准。
- H.264 UVC 已在低分辨率实验链路验证动态画面，但仍依赖 Venus、firmware 和 host 兼容性。

不能说明：

- 不能把 `/dev/video3` 写成 YUYV/NV12/RGB/MJPEG 或 browser-ready 节点。
- 不能把 software-ISP 写成内核 ISP。
- 不能把 `/dev/video6` 写成可用 YUV/RGB 摄像头。
- 不能暗示 `vfe480_yc_pp_chain_configured()` 可以打开。
- 不能把 H.264 UVC 写成默认/public 可用路径。
- 不能把 Android HAL/CHI/tuning/CDM/DMI 值复制到内核或 rootfs。
- 不能提交 vendor blobs、signed firmware、stock DTB、extracted camera/video firmware。

## 下一步

1. 保持 `/dev/video3` RAW `pgAA` 路径稳定，任何 CAMSS/VFE 改动后先回归 OV13B10 RAW。
2. 默认 UVC 继续保持 native-six，不加入非原生 fallback。
3. 优先改进 userspace software-ISP/Rust runtime：曝光、增益、防闪烁、ROI/测光、UVC 控制映射和稳定性。
4. VFE480 true YUV 只有在出现可公开解释、可最小化、可实机验证的 DEMUX + DEMOSAIC + color + terminal 模型后才能推进；否则 gate 继续关闭。
5. 新测试结果写入对应专题文档，主索引只保留状态变化和链接。
