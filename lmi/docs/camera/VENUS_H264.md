# Venus codec 与 H.264 UVC

> 100% AI 编写：本文档由 AI 生成和整理。

本文记录 lmi 上 Venus 视频编解码器和 H.264 UVC 实验链路。

## Venus 状态

lmi 使用 upstream SM8250 Venus codec node：

```text
&venus {
    firmware-name = "qcom/sm8250/xiaomi/lmi/venus.mbn";
    status = "okay";
};
```

当前 dirty 配置把 Venus 作为 built-in release 路径准备，因为项目常用 `Image.gz+dtb/initramfs` 启动不安装模块；但 firmware 仍必须来自本地 ignored 路径，不能提交或发布为源码仓库内容。

设备验证过：

- `/dev/video14`：`qcom-venus-decoder`
- `/dev/video15`：`qcom-venus-encoder`
- encoder 接受 NV12 输入，枚举 H.264 / VP8 / HEVC capture formats。
- generated NV12 -> H.264 Annex-B 可输出 SPS/PPS/IDR。
- OV13B10 RAW `pgAA` 经 software preview-derived NV12 后可送 `/dev/video15` 编成 H.264。

边界：Venus 是视频 codec，不是 camera ISP；它不能把 OV13B10 RAW `pgAA` 直接变成 YUV/RGB。

## H.264 UVC 实验链路

```text
/dev/video3 RAW pgAA
  -> lmi-isp --nv12
  -> lmi-venus-enc --device /dev/video15
  -> lmi-uvc-gadget --format h264
  -> Windows DirectShow / ffmpeg host
```

该链路是 Venus-gated 手工实验路径，不切换默认/public MJPEG native-six。

## 低分辨率动态画面验证

已在 frame 6 `1364x768` 路径验证：

- host Windows DirectShow/ffmpeg 能打开 `UVC Camera` 并解码 H.264。
- ffmpeg 日志显示 `1364x768, 120.07 fps`、`343 packets read (15929816 bytes)`、`329 frames decoded`、`0 decode errors`。
- 抓帧转 JPEG 后，moving overlay 方块中心位置随时间变化，例如约 `(823,427)`、`(1015,471)`、`(1231,527)`、`(211,581)`、`(449,647)`、`(665,65)`。
- 这证明 host 看到的是动态画面，不是缓存静态帧。

## Windows / DirectShow 兼容结论

关键结论：当前 Windows DirectShow / ffmpeg 路径需要 H.264 OUTPUT buffer 默认不设置 `V4L2_BUF_FLAG_KEYFRAME/PFRAME`，从而避免 f_uvc 生成 H.264 payload header 的 `UVC_STREAM_STI`。

- 34-byte vs 48-byte PROBE/COMMIT 不是决定因素。
- 默认 no-STI/no-keyframe-flags 能解码。
- `lmi-uvc-gadget` 默认 `h264_v4l2_frame_flags=0`。
- `--h264-keyframe-flags` 只保留为 host 对照实验开关。
- 因默认不向 V4L2/f_uvc 标记 keyframe，ffmpeg 纯 stream copy 需要：

  ```text
  -copyinkf -c:v copy
  ```

## 发布口径

可以写：低分辨率 H.264 UVC 实验链路已验证动态画面。

不能写：H.264 UVC 是默认可用路径、Venus firmware 已内置公开仓库、相机已有内核 ISP/YUV、或 `/dev/video3` 能直接输出 H.264/NV12。
