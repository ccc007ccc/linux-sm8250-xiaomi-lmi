# Software ISP、UVC native-six 与网络 MJPEG

> 100% AI 编写：本文档由 AI 生成和整理。

本文记录 OV13B10 RAW 上方的用户态 software-ISP / UVC 支持层，以及显式 opt-in 的 MJPEG-over-HTTP 网络摄像头模式。

## 默认链路

```text
OV13B10 /dev/video3 pgAA
  -> lmi-camera Rust demand-start manager
  -> lmi-isp C software ISP
  -> MJPEG FIFO records
  -> lmi-uvc-gadget f_uvc OUTPUT feeder
  -> external USB host webcam
```

Opt-in 网络摄像头链路复用同一个 RAW/software-ISP 基础，但不经过 USB UVC：

```text
OV13B10 /dev/video3 pgAA
  -> lmi-camera Rust network runtime
  -> lmi-isp --fifo --mjpeg
  -> LMVC MJPEG records
  -> Rust HTTP multipart server
  -> browser / VLC / ffmpeg / LAN host
  -> optional RTSP RTP/JPEG + ONVIF/WS-Discovery registration
```

这两条链路都不改变内核边界：`/dev/video3` 仍是 RAW `pgAA`。网络模式默认只是无认证、未加密的测试流，应只在可信局域网或 SSH tunnel 中使用。

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

UVC 与网络 MJPEG 模式都使用这张 native-six 表。Host UVC `COMMIT` frame selection 是 sensor mode selection；网络模式则用 `--frame-index 1..6` 显式选择对应 OV13B10 mode：

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
- `CTRL unit=... selector=...`：接收 host 写入的标准 UVC 控制项，并通过 `/run/lmi-camera/lmi-isp.control` 转发到运行中的 `lmi-isp`。
- `STREAMOFF`/`DISCONNECT`：按 idle grace 停止 `lmi-isp`/RAW，保留 UVC gadget 枚举。
- 退出或切换：先停 manager/feeder/ISP，再解绑 UDC，再删除 configfs，避免 configfs 卡死。

## Network MJPEG opt-in 模式

`lmi-camera run --output network --profile native-modes` 是独立于默认 UVC service 的网络摄像头模式。基础流是 MJPEG-over-HTTP；注册阶段可显式打开轻量 RTSP RTP/JPEG 和 ONVIF/WS-Discovery，使 VLC/ONVIF 工具更容易发现或取得 stream URI。当前仍不把它当作 H.264 网络摄像头：H.264/RTSP 需要另行接入 Venus encoder 与 RTP/H.264 packetization。

典型 `/run` 临时测试命令：

```sh
/run/lmi-camera/lmi-camera run \
  --output network \
  --profile native-modes \
  --frame-index 6 \
  --listen 127.0.0.1:8080 \
  --fps-cap 30 \
  --flicker 50 \
  --isp-bin /run/lmi-camera/lmi-isp \
  --netcam-fifo /run/lmi-camera/lmi-netcam.fifo \
  --isp-control-fifo /run/lmi-camera/lmi-netcam.control
```

默认监听 `127.0.0.1:8080`，需要 LAN host 访问时必须显式改成 `--listen 0.0.0.0:8080`。network runtime 默认帧率上限为 30fps，避免 frame 6 原生 120fps 在浏览器/LED 环境下产生明显 alias/flicker；50Hz 照明下手工测试建议显式加 `--flicker 50`，也可改成 `--flicker auto|60|off`。端点：

| endpoint | 作用 |
| --- | --- |
| `/` | 纯文本说明、RAW invariant、stream/snapshot URI 和 endpoint 列表。 |
| `/status` | JSON 状态：frame、分辨率、sequence、frame size、client 数、reader 状态、bad record 计数。 |
| `/snapshot.jpg` | 返回最新一张 JPEG。 |
| `/stream.mjpg` / `/stream.mjpeg` | `multipart/x-mixed-replace` MJPEG 连续流。 |
| `/onvif/device_service` | 轻量 ONVIF SOAP device/media service，支持 GetDeviceInformation、GetServices、GetCapabilities、GetProfiles、GetStreamUri、GetSnapshotUri、GetScopes、GetHostname、GetNetworkProtocols、GetVideoSources、GetVideoEncoderConfigurations 等常见发现/取流请求。 |

可选注册参数：

```sh
--rtsp --rtsp-listen 0.0.0.0:8554 --rtsp-path /stream.mjpg
--onvif --onvif-listen 0.0.0.0:3702 --onvif-uuid <uuid> --onvif-name "LMI-OV13B10"
```

RTSP 第一版是 std-only 的 RTP/JPEG over RTSP interleaved TCP，只支持 `OPTIONS`、`DESCRIBE`、`SETUP`、`PLAY`、`TEARDOWN`，用于 VLC/ffplay 这类客户端验证 MJPEG 注册流。RFC 2435 的 RTP/JPEG width/height 字段最多覆盖约 `2040x2040`，因此 runtime 会在 frame 1..5 这类超大原生档位上自动禁用 RTSP，只保留 HTTP MJPEG 和 ONVIF 报告的 HTTP URI；默认 frame 6 `1364x768` 可用于 RTSP/JPEG bring-up。ONVIF/WS-Discovery 默认设备名为 `LMI-OV13B10`，Probe/Resolve 和 `GetScopes` 会带 `name/LMI-OV13B10`、`location/LMI-OV13B10`、`hardware/SM8250-OV13B10`、`type/NetworkVideoTransmitter`、`type/video_encoder` 与 `Profile/Streaming` scopes，`GetDeviceInformation` 返回 Manufacturer `LMI`、Model `LMI-OV13B10`。这些响应只用于发现/取流兼容性，不提供鉴权、TLS、云注册或摄像头 PTZ/事件能力，也不会写入或生成账号密码。

当前网络 runtime 启动后即配置 route 并启动 `lmi-isp --fifo --mjpeg`，FIFO reader 只保存最新完整 JPEG 帧；慢 client 不会形成无界队列。第一版不与默认 UVC 同时运行：手工 `/run` 测试前应先停止 `lmi-camera-uvc.service`；正式开机 opt-in 则通过 kernel cmdline `lmi.netcam=1` 让 initramfs 只生成 `/run/systemd/system/lmi-camera-netcam.service`，不生成默认 UVC unit。initramfs 默认传 `--fps-cap 30 --flicker 50`，可用 `lmi.netcam.fps`/`lmi.netcam.fps_cap` 和 `lmi.netcam.flicker`/`lmi.netcam.power_line_frequency` 覆盖；这是为了让 LAN/browser preview 更接近稳定 30fps，并在 50Hz 市电环境下降低频闪。initramfs 同时用 `/run/lmi-camera/rootfs-usb-gadget.disabled` + `lmi-usb-gadget.service.d/10-lmi-camera.conf` runtime drop-in 跳过普通 rootfs 的旧 USB gadget，避免抢 UDC；这些文件都只在 `/run`，不写普通 rootfs。

## UVC 控制项

当前 advertised 的标准 UVC 控制项覆盖 Windows/DirectShow 更可能识别的基础项目，并额外暴露 UVC 1.5 ROI 供支持的 host/app 尝试测光点；Processing Unit `bmControls` 按 UVC 位序只打开 Gain 和 Power Line Frequency，不继续 advertised 未实现的亮度/背光等项目：

| UVC 单元 | selector | host 语义 | runtime 映射 |
| --- | ---: | --- | --- |
| Camera Terminal | `AE_MODE` / `0x02` | 自动曝光模式；兼容 Windows DirectShow 下发的 manual/shutter-priority 值 | `0x01/0x04 -> auto_exposure=0`，`0x02/0x08 -> auto_exposure=1` |
| Camera Terminal | `EXPOSURE_TIME_ABSOLUTE` / `0x04` | 快门/曝光时间，100us 单位 | `exposure_absolute=N` 后换算到 sensor exposure lines |
| Processing Unit | `GAIN` / `0x04` | Gain/ISO-like 增益 | `gain=N` 后映射到 analogue gain；AE active 时 runtime 保持/恢复 `auto_exposure=1`，把它当 ISO-like hint，不再因为 host 写 Gain 就强制关 AE |
| Processing Unit | `POWER_LINE_FREQUENCY` / `0x05` | 关闭/50Hz/60Hz/auto 防闪烁，默认 auto | `flicker=off/50/60/auto`；auto 先做 row/band 频闪检测并锁定 50/60Hz，再让 AE 按安全半周期协同曝光和 gain |
| Camera Terminal | `REGION_OF_INTEREST` / `0x14` | UVC 1.5 测光/ROI 矩形 | `meter_roi=top,left,bottom,right,auto_controls`；坐标按 UVC 0..65535 归一化且 right/bottom 为 inclusive endpoint，`lmi-isp` 内部再换算到 RAW 像素测光窗口；当前只用 auto exposure bit 决定 AE 是否按 ROI 测光 |

注意：UVC 没有通用标准 ISO 控制项，host 侧“ISO”只能先用 `PU_GAIN` 近似；Windows Camera App 也可能不把标准 Exposure/Gain/Power Line Frequency 显示成手动快门/ISO UI，验证仍以 DirectShow/IAMCameraControl/IAMVideoProcAmp 和设备日志为准。ROI/测光点已按 UVC 1.5 `REGION_OF_INTEREST` advertised，但 Windows Camera 是否会在点击取景器时下发该标准控制不可靠，必须以设备日志中的 `ROI ...` / `meter_roi=...` 为准。

## 防频闪与 AE/增益协同

- `flicker=auto` 不再等价于固定 50Hz；`lmi-isp` 会在 RAW unpack 后按垂直 row/band green 均值检测 banding，分别给 50Hz/60Hz 候选打分，并用 pending lock + hysteresis 锁定或清除当前频闪状态，避免 LED 场景里 50/60Hz 分数接近时来回跳。
- 有 sensor timing 时优先用 `PIXEL_RATE + HBLANK` 算真实 line time，再把 50Hz 的 10ms 半周期、60Hz 的 8.33ms 半周期换算成 exposure lines；timing 不可用时才退回原来的 fps/range 近似。
- AE 参考 libcamera/Raspberry Pi AEC/AGC 的常见做法：测光均值先低通滤波，目标附近设置 deadband/hysteresis，只有连续同方向误差才调整，避免 ROI 切到灯泡后因为单帧波动反复“亮一下暗一下”。
- 自动曝光偏暗的小修正优先提高 analogue gain（ISO-like）到软噪声上限；超过软上限或误差较大时再慢慢动真实曝光/快门；digital gain 仍是最后手段，默认 UVC 路径通过 `--max-digital-gain 1024` 基本禁用额外数字放大。
- 频闪 active 时真实曝光仍吸附到 50/60Hz half-cycle safe quantum；如果防闪需要改变曝光，会按比例补偿 analogue/digital gain，避免“快门”和 ISO-like gain 互相打架。
- 自动曝光偏亮时优先降低 digital gain，再降 analogue gain；只有高光/过亮持续存在时才降低曝光，且尽量不破坏已经对齐的防频闪 shutter。
- 手动 exposure 仍由 host 写入后关闭 `auto_exposure`；Gain 在 AE active 时只作为 ISO-like hint 并保持 AE；PLF 关闭/50/60/auto 仍通过标准 UVC `POWER_LINE_FREQUENCY` 控制。

## 已验证状态

- fastboot-booted copydown release image 中，默认 service 运行 Rust `lmi-camera run --output uvc --profile native-modes`。
- production/default path 不再运行 `lmi-camera-web-preview.py`。
- `lmi-camera uvc-status --assert-native-six` 在设备侧通过，frame 目录为 `f01_4208x3120` 到 `f06_1364x768`。
- Windows DirectShow 能看到六个 MJPEG 原生模式，没有旧 fallback。
- frame 1 会切到 OV13B10 mode 0，并配置 `/dev/video3 pgAA 4208x3120`。
- frame 6 会切到 OV13B10 mode 5，并配置 `/dev/video3 pgAA 1364x768`。
- Windows DirectShow 能通过 `IAMCameraControl` / `IAMVideoProcAmp` 看到 Exposure、Gain、Power Line Frequency，并能写入标准 UVC control。
- DirectShow 手动 Exposure 实测会写入 AE mode `0x04`；runtime 已按 manual 兼容，并在 UVC streaming 中转发到 `lmi-isp` control FIFO。
- UVC descriptor 现在使用 `bcdUVC=0x0150` 并在 Camera Terminal `bmControls` 第三字节打开 ROI bit；feeder 对 PROBE/COMMIT 使用 48-byte UVC 1.5 control length，避免 descriptor 与 runtime 长度不一致。
- 实机 DirectShow 写控制项已确认能到达设备：手动 Exposure 下发 `ae_mode=4` 与 `exposure_time_absolute=625`，Gain 下发 `gain=1/0`，Power Line Frequency 下发 `power_line_frequency=1`。新防频闪实现上线后，设备侧应进一步看到 `control: flicker=... mode=...` 与 `flicker: mode=auto hz=... active=... score=... s50=... s60=... quantum=...` 这类检测/锁定日志。
- 2026-06-05 临时 `/run` v5 实机验证：`lmi-isp` 在 2104x1560/60fps UVC stream 中读取 timing `pixel_rate=448000000 hblank=2600 line_time_ns=10500`，auto flicker 锁定 50Hz，真实曝光保持在 956 lines safe quantum，AE 从 `again=1530/1418` 收敛到 `again=1334` 后停止来回调整，`dgain=1024` 保持最低值。若用户点选灯泡 ROI，仍需以 `ROI ...` / `meter_roi=...` 后续日志确认同样收敛。
- 2026-06-06 临时 `/run` network MJPEG/RTSP/ONVIF 实机验证：`lmi-camera.netcam-rtsp-test run --output network --profile native-modes --frame-index 6 --listen 0.0.0.0:8080 --rtsp --rtsp-listen 0.0.0.0:8554 --onvif --onvif-listen 0.0.0.0:3702` 成功配置 `/dev/video3 pgAA 1364x768`，启动 `lmi-isp --fifo --mjpeg`；`/status` 显示 `reader_alive=true`、`bad_records=0`、sequence 持续增长；`/snapshot.jpg` 返回有效 JPEG（SOI `ffd8`、EOI `ffd9`）；`/stream.mjpg` 与 `/stream.mjpeg` 均返回 multipart MJPEG；ONVIF `GetStreamUri` 返回 `rtsp://DEVICE_IP:8554/stream.mjpg`；RTSP `OPTIONS/DESCRIBE/SETUP/PLAY` 通过 TCP interleaved RTP/JPEG 收到 RTP packet；WS-Discovery Probe 返回 XAddrs 指向 `/onvif/device_service`。UDP RTSP transport probe 正确返回 `461 Unsupported Transport`。测试后已清理 `/run` netcam 临时文件并恢复 `lmi-camera-uvc.service`，`uvc-status --assert-native-six` 仍通过。
- 2026-06-06 Windows 发现/频闪兼容补丁临时 `/run` 验证：`lmi-camera.netcam-compat run --output network --profile native-modes --frame-index 6 --listen 0.0.0.0:8080 --fps-cap 30 --rtsp --rtsp-listen 0.0.0.0:8554 --onvif --onvif-listen 0.0.0.0:3702 --onvif-name LMI-OV13B10 --flicker 50` 成功启动，`lmi-isp` 命令含 `--raw /dev/video3`、`--out-width 1364 --out-height 768`、`--fps-cap 30`、`--fifo /run/lmi-camera/lmi-netcam.fifo --mjpeg`，日志确认 `/dev/video3` 仍是 `pgAA` 且 control FIFO 接受 `flicker=50 mode=50 hz=50`。LAN `/status` 显示 `fps_cap=30`、`reader_alive=true`、`bad_records=0`，`/snapshot.jpg` 与 `/stream.mjpg` 均返回有效 JPEG/MJPEG；ONVIF `GetDeviceInformation`、`GetScopes`、`GetHostname`、`GetNetworkProtocols`、`GetVideoSources`、`GetVideoEncoderConfigurations`、`GetVideoEncoderConfigurationOptions`、`GetUsers` 均 HTTP 200；WS-Discovery Probe 返回 `onvif://www.onvif.org/name/LMI-OV13B10` 和 `http://DEVICE_IP:8080/onvif/device_service`。该验证只证明 LAN/ONVIF 响应和 30fps/50Hz 运行态生效，Windows 图形添加流程仍需用户侧刷新后确认。
- 2026-06-06 initramfs `lmi.netcam=1 lmi.netcam.rtsp=1 lmi.netcam.onvif=1` 开机验证：release copydown 临时 boot image（netcam/RTSP/ONVIF forced cmdline）启动后 `lmi-camera-netcam.service` 为 `active/running`，`ExecStart` 含 `--rtsp --rtsp-listen 0.0.0.0:8554 --onvif --onvif-listen 0.0.0.0:3702`，`lmi-camera-uvc.service` 未生成，`lmi-usb-gadget.service` 因 `/run` drop-in `ConditionPathExists=!/run/lmi-camera/rootfs-usb-gadget.disabled` 被跳过（`ConditionResult=no`，旧 `lmi_ubuntu/UDC` 为空）；LAN `/status`、`/snapshot.jpg`、ONVIF `GetStreamUri`、RTSP TCP interleaved RTP/JPEG 和 WS-Discovery Probe 均可用。随后恢复默认 cmdline 重新 boot，默认路径生成 `lmi-camera-uvc.service`、不生成 netcam service，`uvc-status --assert-native-six` PASS。

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
