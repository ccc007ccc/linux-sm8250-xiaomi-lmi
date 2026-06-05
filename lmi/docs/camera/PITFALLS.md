# lmi 摄像头适配坑点和规则

> 100% AI 编写：本文档由 AI 生成和整理。

## 禁止事项

- 不要启用前置升降摄像头，除非升降机构和位置安全已单独验证。
- 不要直接复制 Android downstream `qcom,cam-sensor` 节点到主线 DTS。
- 不要提交 vendor blobs、stock DTB、extracted firmware、signed camera/video blobs。
- 不要把 stock HAL/CHI/tuning/CDM/DMI 输出值复制进内核。
- 不要把 `/dev/video3` 伪装成 YUYV/NV12/RGB/MJPEG。
- 不要把 software-ISP/UVC 写成内核 ISP。
- 不要把 `/dev/video6` 写成 supported YUV/RGB。
- 不要绕过 `vfe480_yc_pp_chain_configured()`。
- 不要在默认/public UVC profile 中加入非原生 fallback。
- 不要为了测试相机把工具或服务永久安装进普通 rootfs；临时测试用 `/run`、`/tmp` 或 `.local/tmp`。

## 常见坑

- CCI mapping 和 I2C address 必须以实机 probe 为准。
- Direct subdev ioctl 测试必须用 `V4L2_SUBDEV_FORMAT_ACTIVE`；TRY formats 不配置 live path。
- 64-bit V4L2 ioctl 结构体布局要准确，尤其 Python/C/Rust 混用时。
- V4L2 controls 会跨运行保留状态；AE 测试前要 reset controls，除非明确 preserve。
- 长 VBLANK 会降低帧率；视频/预览 profile 应优先 video-friendly AE。
- 不要在 CAMSS video-device registration 期间查询 remote media pads；media graph 当时还未完整链接。
- sensor subdev frame-size enumeration 是 OV13B10 mode list 权威来源；CAMSS video node generic sizes 不代表真实 sensor mode。
- unfiltered CAMSS format table 不代表真实 ISP 输出；活动 media-bus format 才是当前边界。
- Venus 是 video codec，不是 camera ISP。
- UVC teardown 必须先停 feeder/manager/ISP，再解绑 UDC，否则 configfs 可能卡死。

## 下一步规则

1. 任意 CAMSS/VFE 改动后，先回归 `/dev/video3 pgAA` RAW。
2. `/dev/video6` 继续只作为 RAW_DUMP 诊断，除非真实 frame dequeued 且 payload 验证通过。
3. Android evidence 只能用于理解结构和比对，不作为可复制配置。
4. VFE480 true YUV 下一步必须是可解释 common-path model，不是更多 fake format 或几何扫参。
5. UVC/用户态方向优先补齐曝光、增益、防闪烁、ROI/测光和稳定性。
6. 文档更新时同步主索引、硬件矩阵和专题文档，避免过度宣传。

## fastboot / 实机流程提示

lmi camera 测试优先临时启动 boot image：

```text
systemctl --no-wall --reboot-argument=bootloader reboot
Windows fastboot.exe boot <boot-linux-copydown-lmi.img>
```

不写 `dtbo`、`recovery`、`vbmeta`、NV/modem 分区，不进 EDL，不发送 firehose。
