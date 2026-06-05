# S5K5E9YX04 后置微距

> 100% AI 编写：本文档由 AI 生成和整理。

## 当前状态

S5K5E9YX04 是 lmi 后置微距，当前未接入主线 Linux。已知 stock/downstream 线索：

- module hint：Sunny S5K5E9YX04 / `lmi_sunny_s5k5e9yx04_macro`
- stock sensor blob：`com.qti.sensor.s5k5e9yx04_lmi.so`

## 暂缓原因

- 当前主线摄像头工作集中在 OV13B10 RAW path 和用户态 runtime。
- 微距需要独立确认供电、CCI/I2C、CSI route、mode table 和 controls。
- 不能因为 Android HAL 有对应模块就写成主线已支持。

## 下一步条件

后续如推进，先做只读证据整理和 probe，再做真实 RAW capture；不宣传 processed output。
