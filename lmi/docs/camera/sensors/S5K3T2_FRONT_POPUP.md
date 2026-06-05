# S5K3T2 前置升降摄像头

> 100% AI 编写：本文档由 AI 生成和整理。

## 当前状态

S5K3T2 是 lmi 前置升降摄像头，当前未接入主线 Linux。已知 stock/downstream 线索：

- module hint：Sunny S5K3T2 / `lmi_sunny_s5k3t2_front`
- stock sensor blob：`com.qti.sensor.s5k3t2_lmi.so`

## 机械边界

前摄不是普通固定传感器。启用传感器前必须先理解并验证：

- 升降机构供电和 motor/driver 控制。
- 上/下限位或位置检测。
- 异常阻塞、跌落、关机和 reboot 场景的安全策略。
- 相机打开/关闭与机械动作之间的时序。

## 暂缓原因

在机械控制和位置安全没有完成前，不启用前摄 sensor stream，避免卡住或损坏升降机构。

## 下一步条件

先单独建立升降机构安全控制与状态检测，再考虑 sensor probe/RAW capture；不能跳过机械安全直接开 camera stream。
