# IMX686 后置主摄

> 100% AI 编写：本文档由 AI 生成和整理。

## 当前状态

IMX686 是 lmi 后置主摄，当前主线 Linux 摄像头第一阶段未启用它。已知证据主要来自 stock/downstream 只读参考：

- module hint：Sunny IMX686 / `lmi_sunny_imx686_wide`
- stock sensor blob：`com.qti.sensor.imx686_lmi.so`

## 暂缓原因

- 当前可稳定验证的主线路径集中在 OV13B10 -> `/dev/video3 pgAA`。
- IMX686 需要单独确认 CCI bus、I2C 地址、reset/power rails、MCLK、CSI lane、CSIPHY/CSID/VFE route、mode table 和 controls。
- 不应复制 Android downstream `qcom,cam-sensor` 节点或 stock tuning/blob 内容到主线 DTS/内核。

## 下一步条件

只有在 OV13B10 RAW/UVC/runtime 基线稳定后，再为 IMX686 做独立 bring-up：先只做 probe 和真实 RAW path，不宣传 processed YUV/RGB。
