# OV08A10 后置长焦

> 100% AI 编写：本文档由 AI 生成和整理。

## 当前状态

OV08A10 是 lmi 后置长焦，当前未接入主线 Linux。已知 stock/downstream 线索：

- module hint：Sunny OV08A10 / `lmi_sunny_ov08a10_tele`
- stock sensor blob：`com.qti.sensor.ov08a10_lmi.so`

## 暂缓原因

- 当前第一阶段只保证 OV13B10 RAW RDI。
- 长焦还需要单独确认供电、CCI/I2C、CSI route、mode table、对焦/actuator 相关依赖和 controls。
- 不应把 stock HAL 支持等同于主线 Linux 已支持。

## 下一步条件

后续若推进，先以 probe + RAW capture 为最小目标；不启用或宣传 VFE processed YUV/RGB。
