# lmi 相机运行时重构记录

> 100% AI 编写：本文档由 AI 生成和整理。

本文保存 Rust/C 相机运行时收敛方向。当前状态总览见 [`RUNTIME_OVERVIEW.md`](RUNTIME_OVERVIEW.md)。

## 重构目标

生产/default 相机编排从 Python 收敛到 Rust/C：

- Rust 负责控制面、生命周期、media route、UVC descriptor 和 sensor mode policy。
- C 负责 data-plane、software ISP、JPEG/H.264 feeder 等性能路径。
- initramfs 默认入口运行 `/run/lmi-camera/lmi-camera run --output uvc --profile native-modes`。
- 不修改普通发行版 rootfs；开机 support layer 只落在 `/run` tmpfs。

## 非协商边界

- `/dev/video3` remains truthful RAW10 `pgAA`。
- `/dev/video3` 不 advertised fake YUYV/NV12/RGB/MJPEG。
- VFE480 processed YUV/RGB 不是短期目标，不能写成已支持。
- Android HAL/CHI/tuning blobs 只能作为参考；不能复制或要求目标 rootfs 携带。
- 默认/public UVC profile 只能是 native-six；不能加隐式 fallback。
- teardown 顺序固定：先停 manager/feeder/ISP，再解绑 UDC，再清 configfs。

## 性能设计

推荐线程/进程模型：

```text
control thread
  - CLI/config/status/recovery

capture thread or C helper
  - owns RAW /dev/video3 fd and MMAP buffers
  - never blocks indefinitely on slow consumers

ISP workers
  - current default: external C lmi-isp process
  - future: split C core / Rust scalar / NEON tile workers

sink thread / feeder
  - owns v4l2loopback OUTPUT fd or UVC OUTPUT fd
  - latest-frame/backpressure policy

metrics/status tick
  - fps, dropped frames, latency, queue depth, exposure/gain
```

Backpressure policy：

- RAW ring 3-4 frames。
- processed ring 2-3 frames。
- slow consumer 时保留最新完整 frame，丢弃旧 frame。
- 不让 RAW capture 因输出端阻塞而死锁。
- 不无限增长内存。

## Rust workspace 入口

当前 workspace：

```text
lmi/camera-rs/
  Cargo.toml
  src/
    main.rs
    config.rs
    devices.rs
    fourcc.rs
    isp.rs
    capture.rs
    controls.rs
    media.rs
    native_modes.rs
    pipeline.rs
    route.rs
    uvc.rs
    uvc_runtime.rs
    v4l2.rs
```

常用命令：

```text
lmi-camera probe
lmi-camera media
lmi-camera route-summary
lmi-camera formats /dev/video3
lmi-camera route-check
lmi-camera setup-route --size 1364x768
lmi-camera capture-raw --frames 30 --sink null
lmi-camera uvc-status --assert-native-six
lmi-camera run --output uvc --profile native-modes
```

## 迁移阶段

### Phase 0：文档和骨架

- 写下 runtime 边界和目标。
- 建立 Rust workspace。
- 增加 probe/status/route skeleton。

### Phase 1：只读发现

- 定位 `/dev/media*`、`/dev/video*`、OV13B10/CAMSS entities。
- 枚举 media graph、formats、controls。
- 确认 RAW candidate 支持 `pgAA`。
- 明确报告 kernel processed YUV/RGB unsupported。

### Phase 2：RAW smoke capture

- 配置 OV13B10 -> CSIPHY1 -> CSID1 -> VFE1 RDI0 route。
- 设置 `/dev/video3` 为 `pgAA`。
- MMAP stream 固定帧数并 clean STREAMOFF。

### Phase 3：UVC native-six runtime

- 创建/清理 UVC configfs gadget。
- 写入六个原生 MJPEG frame descriptor。
- 处理 host `COMMIT` -> sensor `mode_index`。
- `STREAMON` 后启动 RAW route 和 `lmi-isp`。
- `STREAMOFF`/`DISCONNECT` 后停止 ISP/RAW，保留枚举。

### Phase 4：本机 loopback 方向

- `/dev/video20` v4l2loopback 作为本机 standard colour node 目标。
- PipeWire/browser/getUserMedia 完整验证另开阶段记录。

### Phase 5：未来 ISP 加速

现实顺序：

1. 保持当前 C ISP baseline。
2. 将 C ISP 拆成 context/core functions。
3. 如有收益，再暴露 C ABI 给 Rust。
4. Rust 先做 RAW validation/luma meter，再做 RAW10 unpack、低分辨率 YUYV/NV12。
5. 加 tile processing 和 AArch64 NEON。
6. GPU 只有在 CPU/NEON 不够后再考虑。

## 打包状态

initramfs 当前打包 Rust/C runtime：

- `bin/lmi-camera`
- `bin/lmi-isp`
- `bin/lmi-uvc-gadget`
- `bin/lmi-venus-enc`

默认 unit 使用 Rust runtime，旧 `lmi-camera-web-preview.py` 不再是 production/default path。
