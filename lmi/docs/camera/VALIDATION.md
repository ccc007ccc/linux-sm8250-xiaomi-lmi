# lmi 摄像头验证命令

> 100% AI 编写：本文档由 AI 生成和整理。

本文集中保存摄像头相关验证命令。验证结果需要同步到对应专题文档和 [`../../CAMERA_BRINGUP.md`](../../CAMERA_BRINGUP.md) 主索引。

## 本地静态/构建检查

Kernel repo：

```sh
git diff --check
cargo test --manifest-path lmi/camera-rs/Cargo.toml
```

C helper 交叉编译 smoke check（示例使用 Android NDK clang）：

```sh
NDK_CC=/home/ccc007/Android/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang
$NDK_CC -O2 -Wall -Wextra -o /tmp/lmi-isp-check lmi/scripts/lmi-isp.c -lm
$NDK_CC -O2 -Wall -Wextra -o /tmp/lmi-uvc-gadget-check lmi/scripts/lmi-uvc-gadget.c
$NDK_CC -O2 -Wall -Wextra -o /tmp/lmi-venus-enc-check lmi/scripts/lmi-venus-enc.c
```

局部 CAMSS 构建：

```sh
make O=out/m1-release ARCH=arm64 LLVM=1 drivers/media/platform/qcom/camss/
```

## 实机 RAW camera 检查

```sh
dmesg | grep -Ei 'camss|cci|ov13|camera|csiphy|csid|vfe'
ls /dev/media* /dev/v4l-subdev* /dev/video*
lmi-camera probe
lmi-camera route-summary
lmi-camera setup-route --media /dev/media0 --size 1364x768 --raw /dev/video3
lmi-camera capture-raw --media /dev/media0 --size 1364x768 --raw /dev/video3 --frames 30 --sink null
```

必须确认：

- `/dev/video3` 是 `pgAA`。
- payload size/stride 与目标 mode 匹配。
- 不能出现 fake YUYV/NV12/RGB/MJPEG formats 作为 `/dev/video3` 当前路线输出。

## UVC native-six descriptor/runtime 检查

```sh
systemctl status lmi-camera-uvc.service
ps | grep -E 'lmi-camera|lmi-isp|lmi-uvc-gadget'
/run/lmi-camera/lmi-camera uvc-status --assert-native-six
find /sys/kernel/config/usb_gadget/lmi_uvc/functions/uvc.0/streaming/mjpeg/mjpg \
  -maxdepth 1 -type d -name 'f*' -print
```

期望：

- service 运行 Rust `/run/lmi-camera/lmi-camera`，不是旧 Python helper。
- configfs frame 目录正好六个：`f01_4208x3120` 到 `f06_1364x768`。
- Windows/DirectShow host 枚举同样看到六个 MJPEG 原生模式。
- host 打开某 frame 后，日志显示 `COMMIT frame=N -> OV13B10 mode=M WxH`，并且 `/dev/video3 pgAA`、`lmi-isp` output、UVC frame 尺寸一致。

## UVC 曝光/增益/防闪烁控制检查

设备侧 descriptor 应 advertised：

```sh
cat /sys/kernel/config/usb_gadget/lmi_uvc/functions/uvc.0/control/terminal/camera/default/bmControls
cat /sys/kernel/config/usb_gadget/lmi_uvc/functions/uvc.0/control/processing/default/bmControls
cat /sys/kernel/config/usb_gadget/lmi_uvc/functions/uvc.0/control/header/h/bcdUVC
```

期望当前值：

- Camera Terminal 三字节为 `10`、`0`、`32`：第一字节 `10` 是 `AE_MODE` + `EXPOSURE_TIME_ABSOLUTE`，第三字节 `32` 是 UVC 1.5 `REGION_OF_INTEREST`（index 21）。
- Processing Unit 第一字节为 `0`、第二字节为 `6`：`GAIN` + `POWER_LINE_FREQUENCY`。
- Control header `bcdUVC` 为 UVC 1.5（写入 `0x0150`；configfs 读回可能显示十进制 `336`）；Rust runtime 启动 feeder 时应带 `--control-len 48`，helper fallback 也应默认 48-byte UVC 1.5 PROBE/COMMIT，让 descriptor 与 runtime 长度一致。

Host 写控制项时，设备日志应出现：

```text
[uvc-gadget ...] start ... control_len=48 ...
[uvc] control: unit=1 selector=2 ae_mode=... -> auto_exposure=...
[uvc] control: unit=1 selector=4 exposure_time_absolute=... -> auto_exposure=0,exposure_absolute=...
[uvc] control: unit=2 selector=4 gain=... -> auto_exposure=1,gain=...   # AE active 时作为 ISO-like hint；如果已经 manual，则仍可保持 manual
[uvc] control: unit=2 selector=5 power_line_frequency=... -> flicker=...
[lmi-isp] controls: timing pixel_rate=... hblank=... vblank=... line_time_ns=...
[lmi-isp] control: flicker=auto mode=auto hz=0 quantum=0 source=timing/fallback
[lmi-isp] control: flicker=50 mode=50 hz=50 quantum=... exposure=... source=timing/fallback
[lmi-isp] control: flicker=60 mode=60 hz=60 quantum=... exposure=... source=timing/fallback
[lmi-isp] flicker: mode=auto hz=... detected=... active=... score=... s50=... s60=... bands=... quantum=... source=timing/fallback exposure=... again=... dgain=...
[lmi-isp] control: exposure_absolute=... -> exposure=...
[lmi-isp] control: gain=... -> analogue_gain=...
[lmi-isp] fps=... outmean=... ae{mean=... hi=... exp=... again=... dgain=... ...}
```

Host 如果支持 UVC 1.5 ROI/测光点，点击取景器或设置 ROI 时还应出现；这里的 top/left/bottom/right 是 UVC 0..65535 归一化坐标，right/bottom 按 UVC inclusive endpoint 处理：

```text
ROI top=... left=... bottom=... right=... auto=...
[uvc] ROI: top=... left=... bottom=... right=... auto=0x... -> meter_roi=...
[lmi-isp] control: meter_roi=... enabled=...
```

Windows/DirectShow 侧优先确认 Exposure、Gain、Power Line Frequency 是否可见；ISO 不是标准 UVC 控制项，当前按 `GAIN` 做 ISO-like 映射。DirectShow 手动曝光可能写入 UVC AE mode `0x04`（shutter priority），runtime 应按 manual 处理；`0x01/0x04` 均应转为 `auto_exposure=0`，`0x02/0x08` 均应转为 `auto_exposure=1`。Power Line Frequency 当前默认值应为 `3/auto`；如果 Windows Camera App 不显示手动快门/ISO UI，但 DirectShow 能写 Exposure/Gain/PLF 且设备日志收到控制，记录为 host/app UI 限制。

ROI/测光点已实验性 advertised 为 UVC 1.5 `REGION_OF_INTEREST`，但 Windows Camera 可能仍不会在点击取景器时下发该标准控制。若点击后没有任何 `ROI ...` / `meter_roi=...` 日志，应记录为 host/app 映射限制，而不是把 `/dev/video3` 或 VFE480 processed 路径改掉。

ROI/灯泡测光回归重点：切换测光点后允许短暂重收敛，但后续日志应显示 exposure/gain 收敛而不是持续抽动。典型稳定形态：`flicker: mode=auto hz=50 ... quantum=952 ... exposure=956 ... dgain=1024`，随后 `ae{mean=... exp=956 again=... dgain=1024}` 中 `again` 在少数几次变化后停止；`dgain` 默认保持 `1024`。如果 `again` 或 `exp` 每个日志窗口持续上下交替，继续降低 ROI AE alpha、提高 ROI deadband 或增加连续方向确认次数。

## Network MJPEG opt-in 检查

网络摄像头模式是显式 opt-in 的 MJPEG-over-HTTP 测试路径，不修改默认 UVC native-six，也不把 `/dev/video3` 伪装成 processed node。手工 `/run` 实机测试前先停 UVC service，避免两个 runtime 同时占用 OV13B10/CAMSS：

```sh
systemctl stop lmi-camera-uvc.service
/run/lmi-camera/lmi-camera run \
  --output network \
  --profile native-modes \
  --frame-index 6 \
  --listen 0.0.0.0:8080 \
  --fps-cap 30 \
  --flicker 50 \
  --rtsp --rtsp-listen 0.0.0.0:8554 \
  --onvif --onvif-listen 0.0.0.0:3702 --onvif-name LMI-OV13B10 \
  --isp-bin /run/lmi-camera/lmi-isp \
  --netcam-fifo /run/lmi-camera/lmi-netcam.fifo \
  --isp-control-fifo /run/lmi-camera/lmi-netcam.control
```

设备侧期望日志：

```text
raw /dev/video3 format pgAA 1364x768
raw_invariant /dev/video3 remains truthful pgAA
[isp] command: ... --fps-cap 30 ... --fifo /run/lmi-camera/lmi-netcam.fifo --mjpeg ...
control: flicker=50 mode=50 hz=50
[netcam] HTTP server listening on 0.0.0.0:8080
[netcam] RTSP server listening on 0.0.0.0:8554
[netcam] WS-Discovery listening on 0.0.0.0:3702
```

设备本机或 LAN host 验证：

```sh
curl http://127.0.0.1:8080/status
curl -D /tmp/snapshot.headers -o /tmp/snapshot.jpg http://127.0.0.1:8080/snapshot.jpg
curl --max-time 5 -D /tmp/stream.headers -o /tmp/stream.bin http://127.0.0.1:8080/stream.mjpg
curl -s -H 'Content-Type: application/soap+xml' --data-binary @/tmp/onvif-get-stream-uri.xml \
  http://127.0.0.1:8080/onvif/device_service
# Windows/ONVIF 发现兼容性回归时，再 POST GetDeviceInformation/GetScopes/GetHostname/
# GetNetworkProtocols/GetVideoSources/GetVideoEncoderConfigurations/GetVideoEncoderConfigurationOptions/GetUsers。
ffprobe -rtsp_transport tcp -v error -select_streams v:0 -show_entries stream=codec_name,width,height \
  -of default=noprint_wrappers=1 rtsp://127.0.0.1:8554/stream.mjpg
```

期望：

- `/status` 显示 `output=network`、`profile=native-modes`、frame/mode/width/height 与选择档位一致，`fps_cap=30`（或命令行覆盖值）、`reader_alive=true`，`bad_records=0`，`sequence`/`frames_received` 持续增长。
- `/snapshot.jpg` 是有效 JPEG，SOI 为 `ff d8`，EOI 为 `ff d9`。
- `/stream.mjpg` 或 `/stream.mjpeg` 返回 `multipart/x-mixed-replace; boundary=lmi-mjpeg`，样本中有连续 boundary 和 JPEG SOI/EOI。
- `/onvif/device_service` 对 SOAP POST 返回 XML；GetCapabilities/GetProfiles/GetStreamUri/GetSnapshotUri 至少能取到当前 HTTP/RTSP URI；GetDeviceInformation/Scopes/Hostname/NetworkProtocols/VideoSources/VideoEncoderConfigurations/VideoEncoderConfigurationOptions/GetUsers 也应返回 200，默认 name/model/hostname/scope 含 `LMI-OV13B10`，但不生成账号密码。
- `rtsp://DEVICE_IP:8554/stream.mjpg` 在 frame 6 `1364x768` 下可被 VLC/ffprobe 通过 TCP interleaved RTP/JPEG 打开；frame 1..5 超过 RTP/JPEG 2040px 限制时应看到 runtime 提示禁用 RTSP，HTTP MJPEG 仍可用。
- WS-Discovery 只在 `--onvif` 打开时监听 UDP 3702，Probe/Resolve 返回 XAddrs 指向 `/onvif/device_service`，Scopes 中应包含 `onvif://www.onvif.org/name/LMI-OV13B10`，用于避免 Windows 发现列表显示 `unknown`。
- LAN host 可以用 `http://DEVICE_IP:8080/status`、`/snapshot.jpg`、`/stream.mjpg` 访问；若使用 `127.0.0.1:8080` 默认监听，则只能本机或 SSH tunnel 访问。
- 未触碰 `/etc/systemd` 或普通 rootfs；临时 helper/FIFO/样本文件只放 `/run/lmi-camera`。

2026-06-06 `/run` 临时验证结果：frame 6 `1364x768` 网络流通过，LAN host 可取 `/status` 与有效 JPEG；`/stream.mjpg`、`/stream.mjpeg` 均有 multipart MJPEG 数据；ONVIF `GetStreamUri` 返回 `rtsp://DEVICE_IP:8554/stream.mjpg`；RTSP `OPTIONS/DESCRIBE/SETUP/PLAY` 通过 TCP interleaved RTP/JPEG 收到 RTP packet；WS-Discovery Probe 返回 XAddrs 指向 `/onvif/device_service`；UDP RTSP transport probe 正确返回 `461 Unsupported Transport`。测试后清理 netcam 临时文件并恢复 UVC service，`uvc-status --assert-native-six` 仍 PASS。

2026-06-06 Windows 发现/频闪兼容补丁 `/run` 临时验证结果：frame 6 network runtime 以 `--fps-cap 30 --flicker 50 --onvif-name LMI-OV13B10` 启动，`lmi-isp` 日志确认 `/dev/video3 pgAA 1364x768`、`--fps-cap 30` 和 `control: flicker=50 mode=50 hz=50` 生效；LAN `/status` 显示 `fps_cap=30`、`reader_alive=true`、`bad_records=0`，`/snapshot.jpg` 是有效 JPEG，`/stream.mjpg` 样本含连续 multipart JPEG。ONVIF `GetDeviceInformation`、`GetScopes`、`GetHostname`、`GetNetworkProtocols`、`GetVideoSources`、`GetVideoEncoderConfigurations`、`GetVideoEncoderConfigurationOptions`、`GetUsers` 均返回 HTTP 200；WS-Discovery Probe 返回 name scope `LMI-OV13B10` 和 XAddr `http://DEVICE_IP:8080/onvif/device_service`。Windows 添加设备 GUI 是否仍弹凭据框需刷新 Windows 侧 discovery 后确认；本路径仍无认证/无 TLS，不保存凭据。

开机 opt-in 验证 `lmi.netcam=1` / 裸 `lmi.netcam`，以及可选 `lmi.netcam.rtsp=1`、`lmi.netcam.onvif=1` 时，release 当前 `CONFIG_CMDLINE_FORCE=y`，测试 boot image 需要临时把这些参数加进 release forced cmdline，打包后再恢复默认 cmdline。initramfs 默认会给 netcam unit 传 `--fps-cap 30 --flicker 50`；可用 `lmi.netcam.fps`/`lmi.netcam.fps_cap` 和 `lmi.netcam.flicker`/`lmi.netcam.power_line_frequency` 覆盖。Fresh boot 后检查：

```sh
tr '\n' ' ' </proc/cmdline
systemctl is-active lmi-camera-netcam.service
systemctl is-active lmi-camera-uvc.service
systemctl is-active lmi-usb-gadget.service
systemctl show lmi-usb-gadget.service -p ActiveState -p DropInPaths -p ConditionResult
find /sys/kernel/config/usb_gadget -maxdepth 2 -name UDC -type f -exec sh -c 'for f do printf "%s=" "$f"; cat "$f"; done' sh {} +
curl http://127.0.0.1:8080/status
```

期望：netcam service active；如果打开 RTSP/ONVIF，`ExecStart` 含 `--rtsp --rtsp-listen ... --onvif --onvif-listen ...`；默认 UVC unit 不存在或 inactive；rootfs 旧 `lmi-usb-gadget.service` inactive，`DropInPaths` 指向 `/run/systemd/system/lmi-usb-gadget.service.d/10-lmi-camera.conf` 且 `ConditionResult=no`；旧 `lmi_ubuntu/UDC` 为空；HTTP/RTSP/ONVIF/WS-Discovery endpoint 正常。

默认回归验证：恢复没有 `lmi.netcam` 的 cmdline 后 fresh boot，期望 `lmi-camera-uvc.service` active、`lmi-camera-netcam.service` not-found/inactive、`lmi-usb-gadget.service` 仍因同一个 `/run` drop-in 被跳过，`/run/lmi-camera/lmi-camera uvc-status --assert-native-six` PASS，Windows/host 可枚举 `UVC Camera`。

清理/回归：

```sh
# 停止手动启动的 network runtime 后
rm -f /run/lmi-camera/*netcam* /run/lmi-camera/netcam-*
systemctl start lmi-camera-uvc.service
/run/lmi-camera/lmi-camera uvc-status --assert-native-six
```

## H.264 / Venus 检查

```sh
dmesg | grep -Ei 'venus|vidc|hfi|video-codec|firmware'
find /dev -maxdepth 1 -name 'video*' -print
```

确认 mem2mem encoder node 后，再做 generated NV12 或 OV13B10 RAW 派生 software NV12 -> `/dev/video15` H.264。注意：

- firmware 必须从 ignored/local blobs 临时 staged，不能提交。
- H.264 UVC 默认 no-keyframe-flags / no-STI。
- ffmpeg stream copy 裸 H.264 使用 `-copyinkf -c:v copy`。
- H.264 仍是实验路径，不替代默认 MJPEG native-six。

## VFE480 RAW_DUMP 诊断

只在明确要诊断 `/dev/video6` 时执行：

```sh
lmi-camera setup-route \
  --route lmi-ov13b10 \
  --vfe msm_vfe1_pix \
  --video-entity msm_vfe1_video3 \
  --csid-source-pad 4 \
  --size 2080x1170 \
  --media /dev/media0
```

期望仍是诊断语义：

- `/dev/video6` 只能写作 `BA10` RAW_DUMP diagnostic。
- timeout / no comp_done 不是功能成功。
- 只有真实 dequeued frame 且 payload 验证通过，才可以写为新进展。

## fastboot 实机流程

当前 lmi 流程优先临时 boot，不写 boot 分区：

```text
systemctl --no-wall --reboot-argument=bootloader reboot
Windows fastboot.exe boot <boot-linux-copydown-lmi.img>
```

遵守边界：不写 persist/modemst/fsg/fsc/NV/modem/dtbo/recovery/vbmeta，不进 EDL，不发送 firehose。

## 清理

实机验证后清理：

- 临时 `/tmp` / `/run` helper、FIFO、ready files。
- 手动启动的 feeder、ISP、manager 进程。
- 临时 firmware staging（如果本次放置过）。
- Windows/host 抓帧和本地 scratch 产物按是否需要留证据决定，留证据也应放 ignored/local 路径。
