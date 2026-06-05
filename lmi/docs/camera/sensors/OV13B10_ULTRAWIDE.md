# OV13B10 后置超广角

> 100% AI 编写：本文档由 AI 生成和整理。

OV13B10 是 lmi 当前第一阶段摄像头目标，也是唯一已验证的主线 Linux 相机传感器路径。

## 当前状态

已验证链路：

```text
OV13B10 -> CSIPHY1 -> CSID1 -> VFE1 RDI0 -> /dev/video3
```

运行时事实：

- sensor node：`ov13b10 21-0010`
- CCI bus：`&cci0_i2c1`
- I2C 7-bit address：`0x10`
- media bus code：`MEDIA_BUS_FMT_SGRBG10_1X10` / `0x300a`
- video node：`/dev/video3` / `msm_vfe1_video0`
- fourcc：`pgAA` / packed 10-bit GRBG Bayer
- 最小模式示例：`1364x768`，stride `1712`，payload `1314816` bytes

重要边界：`/dev/video3` 是真实 RAW Bayer RDI 节点，不能 advertised fake YUYV/NV12/RGB/MJPEG。

## 原生模式

| Mode index | 尺寸 | 名义帧率 | RAW 状态 |
| ---: | --- | ---: | --- |
| 0 | `4208x3120` | 29.799 fps | captures `pgAA` |
| 1 | `4160x3120` | 29.799 fps | captures `pgAA` |
| 2 | `4160x2340` | 29.799 fps | captures `pgAA` |
| 3 | `2104x1560` | 59.598 fps | captures `pgAA` |
| 4 | `2080x1170` | 59.598 fps | captures `pgAA` |
| 5 | `1364x768` | 120.069 fps | captures `pgAA` |

UVC default/public `native-modes` profile 把这些模式一一映射为 frame 1..6；详见 [`../SOFTWARE_ISP_UVC.md`](../SOFTWARE_ISP_UVC.md)。

## Bring-up 关键修复

- downstream `cci-master = <1>` 对应主线 `&cci0_i2c1`。
- 实际 ACK/chip-id 在 7-bit `0x10`；早期 `0x36` 候选地址 NACK。
- reset GPIO 是 TLMM GPIO91，`GPIO_ACTIVE_LOW`，初始保持 asserted，power-on sequence 再释放。
- MCLK 是 MCLK2 / GPIO96，19.2 MHz。
- VANA enable 使用 GPIO63，并保留下游兼容 rail/load votes。
- `CONFIG_SM_CAMCC_8250=y` 对 built-in CAMSS/CCI/sensor 是必要条件。
- OV13B10 reset 初始态修复后才避免传感器过早脱离 reset。
- 临时 powered CCI scan 找到 `0x560d42` chip-id；scan 诊断在 DTS 修复后删除。
- 直接 ioctl helper 必须使用 ACTIVE subdev formats；TRY formats 不会配置 live CAMSS stream。
- OV13B10 active-state 自锁通过不把 `sd.state_lock` 绑定到同一 mutex 解决。
- CAMSS format enumeration 已收紧，活动 OV13B10 route 只报告真实 `pgAA`，避免通用 YUV 表误导。

## 标准 metadata 和 controls

OV13B10 当前已暴露：

- 六个 frame-size enumeration。
- frame-interval enumeration 和基于模式 timing 的 `G/S_FRAME_INTERVAL`。
- crop/native-size selection metadata。
- CSI-2 frame descriptor 和 mbus config metadata。
- orientation/rotation：当前候选是 rear-facing `orientation = 1`、`rotation = 90`。
- `VIDIOC_QUERY_EXT_CTRL` 控制发现，包含 integer64 和 compound controls。
- `V4L2_CTRL_TYPE_AREA` unit cell size：`1120nm x 1120nm`。
- exposure、VBLANK/HBLANK、analogue gain、digital gain、link frequency、pixel rate、H/V flip、test pattern、orientation、rotation 等控制项。

## Stock 参考输入

有用的 stock 路径只作为只读参考：

- `/vendor/lib64/camera/com.qti.sensor.ov13b10_lmi.so`
- `/vendor/lib64/camera/com.qti.eeprom.lmi_sunny_ov13b10_gt24p64.so`
- `/vendor/lib64/camera/com.qti.sensormodule.lmi_sunny_ov13b10_ultra.bin`
- `/vendor/lib64/camera/com.qti.tuned.lmi_sunny_ov13b10_ultra.bin`
- `/vendor/lib64/camera/com.qti.tuned.lmi_sunny_ov13b10_ultra_pro.bin`

这些文件说明 Android 栈依赖 HAL/CHI/tuning 生成 IFE/CDM 程序；不能复制 blob 内容，不能要求目标 Linux rootfs 携带它们，也不能把它们当作主线 V4L2/YUV 支持证据。
