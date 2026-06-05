# VFE480 RAW_DUMP 诊断记录

> 100% AI 编写：本文档由 AI 生成和整理。

本文压缩记录 `/dev/video6` / `msm_vfe1_pix` 的 VFE480 诊断历史。当前结论见 [`VFE480_STATUS.md`](VFE480_STATUS.md)。

## 诊断形态

当前实验 PIX branch 不再 fake NV12。`/dev/video6` 只作为 bounded RAW_DUMP 诊断节点：

- `/dev/video3` 保持 RAW `pgAA` RDI，不受影响。
- `/dev/video6` 暴露 single-plane unpacked GRBG10：`V4L2_PIX_FMT_SGRBG10` / `BA10`。
- RAW_DUMP 使用 VFE480 hardware client 10 和 comp group 3。
- RAW_DUMP 使用 `MODE_QCOM_PLAIN` 和 PLAIN16_10 LSB packer config `0x15`。
- 任何未来成功 `/dev/video6` frame 都只能先记录为 raw diagnostic data，不是 ISP/YUV/RGB 成功。

## 下游事实

- FULL Y/C：bus clients 0/1，comp group 0。
- FULL_DISP Y/C：bus clients 4/5，comp group 1。
- RAW_DUMP / PIXEL RAW：bus client 10，comp group 3。
- RAW_DUMP image-size violation bit：`0x0400`。
- RAW_DUMP 和 FULL 都属于 source group 0；RDI 使用其它 source groups。
- RAW_DUMP plane stride / width / slice height 在下游 UAPI 里按 pixels/lines 解释，不是 byte stride。

## r24-r47 压缩历史

- r24/r25：copydown boot 可启动；`/dev/video6` 切到 `BA10` RAW_DUMP，client10 starts，但 capture timeout，image-size violation `0x400`。
- r25/r27/r28：调整 RAW_DUMP width/stride/frame-incr 单位后，`0x400` 仍存在。
- r29：设置 `TOP_CORE_CFG_INPUTMUX_PP(vfe->id)` 后去掉一类 image-size violation，但仍无 dequeued frame。
- r30-r34：扩展 stop、non-BUS、CSID PXL、reg_update、TOP/CGC/diagnostic snapshots；确认 `/dev/video3` RAW 回归正常，CSID PXL 有 SOF/EOF，但 `/dev/video6` 无 BUS comp_done。
- r35：runtime mux sweep 证明 lmi OV13B10 route 上 `input_mux_sel_pp=0` 是第一个能看到 VFE PIX BUS IRQ 和 RUP ack 的 mux；其它 mux 没有成功 frame。
- r36-r37：RAW_DUMP width/stride sweep 仍无 frame；修复 generic `video_start_streaming()` error rollback，避免同一 boot 后续尝试被 EBUSY 污染。
- r38-r43：加入只读 PP CLC、PP PREPROCESS、PP CAMIF、BUS client10 status 和 BUS top debug；确认 client10 programmed、地址状态有活动、CSID PXL/VFE BUS/RUP 可见，但 PP module window 仍大多未编程，无 completed frame。
- r44：把 VFE480 TOP `core_cfg0` 对齐 Android OV13B10 CAMIF start evidence：`0x60002f00`；`/dev/video3` RAW 正常，`/dev/video6` mode5/mode4 仍 timeout。TOP correction 必要但不充分。
- r45：只增加 DEMUX even/odd 结构化诊断名；不打开 YUV gate。测试中 `/dev/video3` 保持 `pgAA`，`/dev/video6` 保持 `BA10`，mode5/mode4 都 timeout。
- r46：加入 common-path read-only dump，即使无 BUS violation 也读 DEMUX/DEMOSAIC/color 状态；结果显示 DEMUX module/even/odd、BPC/PDPC demux、DEMOSAIC、color-correct、color-xform 全为 0。
- r47：扩大 common-path detail dump 到 `0x3090..0x30a8` 和 `0x3860..0x387c`；mode5/mode4 仍 timeout，mainline 同类窗口仍全 0，证明 Android/OPE-observed common-path windows 未被主线编程。

## 当前诊断结论

已确认：

- `/dev/video3` RAW mode5 regression 可持续通过，`1364x768 pgAA` payload `1314816`。
- `/dev/video6` 仍只枚举 `BA10` 诊断格式。
- 正确 PIX route 是 `msm_csid1:pad4 -> msm_vfe1_pix:pad0`。
- mode5 `1360x768` 和 mode4 `2080x1170` RAW_DUMP attempts 均 timeout。
- CSID1 PXL activity、VFE PIX BUS IRQ 和 RUP ack 可见，但 no comp_done。
- common-path windows 在 mainline 仍为 0。

因此当前失败不是简单几何、TOP、BUS client 或 IRQ wiring 问题，而是缺少 VFE480 PP/IFE common-path module programming。

## 不应继续做的事

- 不要重新 advertised `/dev/video6` 为 NV12/YUYV/RGB。
- 不要继续盲扫 RAW_DUMP width/stride 作为主要方向。
- 不要添加更多 dormant helper 来掩盖 gate 关闭。
- 不要把 Android post-CDM 寄存器值或 DMI 表复制成 mainline 静态配置。

## 可用验证命令

```sh
lmi-camera setup-route \
  --route lmi-ov13b10 \
  --vfe msm_vfe1_pix \
  --video-entity msm_vfe1_video3 \
  --csid-source-pad 4 \
  --size 2080x1170 \
  --media /dev/media0
```

只有在真实 dequeued frame 且 payload 被验证后，才能把新的结果写为功能进展；否则继续写为 RAW_DUMP diagnostic。
