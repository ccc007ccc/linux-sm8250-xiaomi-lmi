# GC02M1 后置景深

> 100% AI 编写：本文档由 AI 生成和整理。

## 当前状态

GC02M1 是 lmi 后置景深传感器，当前未接入主线 Linux。已知 stock/downstream 线索：

- module hint：OFilm GC02M1 / `lmi_ofilm_gc02m1_depth`
- stock sensor blob：`com.qti.sensor.gc02m1_lmi.so`

## 暂缓原因

- 景深传感器通常依赖多摄同步、HAL 组合策略和应用层融合；当前主线阶段不需要它来验证基础 RAW/UVC。
- 仍需独立确认 CCI/I2C、供电、CSI route、mode table 和 controls。

## 下一步条件

除非未来需要多摄/景深功能，否则优先级低于 OV13B10 runtime、UVC 控制和本机标准相机节点。
