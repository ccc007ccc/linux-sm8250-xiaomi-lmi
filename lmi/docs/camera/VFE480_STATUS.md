# SM8250 VFE480 processed YUV/RGB 状态

> 100% AI 编写：本文档由 AI 生成和整理。

本文记录 VFE480 真 processed YUV/RGB 路径的当前结论。详细 RAW_DUMP 诊断历史见 [`VFE480_DIAGNOSTICS.md`](VFE480_DIAGNOSTICS.md)，Android 证据见 [`VFE480_ANDROID_EVIDENCE.md`](VFE480_ANDROID_EVIDENCE.md)。

## 当前状态

- 主线 SM8250 CAMSS/VFE480 在 lmi 上仍没有完整 processed ISP/YUV/RGB 输出。
- `/dev/video3` 已验证且必须保持 RAW RDI `pgAA`。
- 曾有实验 PIX branch 暴露 `msm_vfe1_pix` / `/dev/video6`，但它不是工作 YUV/RGB 节点。
- `vfe480_yc_pp_chain_configured()` 必须继续返回 false。

## r17-r23 证明了什么

这些早期实验让 `/dev/video6` 枚举 NV12 并到达 `STREAMON`，但没有 dequeued processed frame：

- VFE1 PIX media node 可以在不破坏 `/dev/video3` RAW 的情况下暴露。
- CSID source stream 3 可以 route through PXL/IPP。
- VFE480 可以分配两个 Y/C WM 并编程 FULL Y/C bus clients 0/1。
- CAMIF starts、PIX reg-update 和 RUP ack 可见。
- 但 FULL Y/C image-size violation 和缺失 comp_done 证明这不是可用 YUV。

结论：格式协商通过不等于 processed output 可用。

## 当前 blocker

已排除或不足以解释问题的方向：

- 不是单纯 WM allocation。
- 不是普通 BUS client 0/1 或 client10 地址/stride/packer 写漏。
- 不是简单 TOP CGC、CAMIF start、RUP IRQ 或 CSID PXL SOF/EOF。
- 不是一个固定 mux 值就能解决。

真正缺口是 VFE480 PP/IFE common-path module chain：DEMUX、PDPC/BPC、WB、DEMOSAIC、color correct、CST/color transform、MNDS/crop/round-clamp 等。Android 通过 HAL/CHI/tuning/CDM/DMI 在每个 request 生成和提交这些状态，不能作为静态值复制进主线。

## 当前规则

- 不把 `/dev/video6` advertised 为 supported YUV/RGB。
- 不给 `/dev/video3` 添加 processed formats。
- 不用 fake NV12 enumeration 掩盖缺口。
- 不复制 Android tuning/CDM/DMI/post-CDM 寄存器值。
- 后续只有在有可公开解释、可最小化、可实机验证的 common-path model 后，才能考虑打开 Y/C gate。

## 可继续的方向

1. 继续理解 Titan480 PP/IFE block semantics，但必须 clean-room 化，不能复制无授权源码或 tuning。
2. 为 DEMUX + DEMOSAIC + color + terminal 建立小而可解释的模型。
3. 先在代码中保持 gate closed，把诊断和真实功能分开。
4. 优先通过 software-ISP/Rust runtime 提供可用 camera 功能，不阻塞在内核 ISP 上。
