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
$NDK_CC -O2 -Wall -Wextra -o /tmp/lmi-isp-check lmi/scripts/lmi-isp.c
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
