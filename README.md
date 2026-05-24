# linux-sm8250-xiaomi-lmi

这是 Redmi K30 Pro / POCO F2 Pro（代号 `lmi`，Qualcomm SM8250 / Snapdragon 865）的主线 Linux 内核适配仓库。

目标是让普通 Linux 发行版 rootfs 尽量保持纯净，把硬件适配、启动菜单和早期启动支持放在内核、DTS、initramfs 与配套支持层中完成。

## 基本状况

当前系统已经可以从 UFS 上的 Linux rootfs 启动，使用 initramfs 菜单选择系统或维护入口，并进入桌面环境。release 配置面向日常发行版使用，同时启用了 Docker、LXC、nftables/cgroup 等服务器场景需要的内核能力。

更细的进展记录见：

- [`lmi/HARDWARE_SUPPORT.md`](lmi/HARDWARE_SUPPORT.md)：硬件支持状态表
- [`lmi/ADAPTATION_NOTES.md`](lmi/ADAPTATION_NOTES.md)：适配过程与诊断记录
- [`lmi/configs/m1.config`](lmi/configs/m1.config)：通用 lmi 配置片段
- [`lmi/configs/m1-release.config`](lmi/configs/m1-release.config)：release 启动配置片段

## 相关仓库

| 仓库 | 用途 |
| --- | --- |
| `linux-sm8250-xiaomi-lmi` | 主线 Linux 内核、DTS、lmi 配置和硬件适配记录。 |
| `sm8250-xiaomi-lmi-initramfs` | 早期启动、rootfs 自动发现、系统选择菜单、维护入口和 next-boot 支持。 |
| `sm8250-xiaomi-lmi-boot` | Android boot image 打包、bootshim、fastboot/fastbootd 测试和 boot manifest 工具。 |
| `sm8250-xiaomi-lmi-rootfs` | Ubuntu debug rootfs 与 Fedora/KDE rootfs 构建辅助脚本；硬件适配不长期依赖这里。 |

## 已支持或基本可用的硬件

| 硬件 | 当前状态 | 备注 |
| --- | --- | --- |
| UFS 存储 | 已支持 | 已验证分区扫描和 Linux rootfs 启动。 |
| 屏幕 | 已支持 | Samsung AMS667UU01，已验证 fbcon、DRM/KMS、背光和 1080x2400 显示模式。 |
| 触摸屏 | 已支持 | FocalTech FT3518，多点触控基础事件可用。 |
| 实体按键 | 已支持 | 电源键、音量上、音量下可作为 input 事件使用；initramfs 菜单可用音量键选择、电源键确认。 |
| GPU | 已支持 | Adreno 650 / GMU / freedreno 已验证基础 GLES、GBM、KMS 路径。 |
| Wi-Fi | 已支持 | QCA6391 PCIe / ath11k，可联网和 SSH。 |
| 蓝牙 | 已支持 | QCA6391 UART HCI，固件加载和基础 BR/EDR、LE 能力可用。 |
| USB | 部分支持 | USB ACM 调试、Type-C 基础枚举和标准 PD sink 可用；OTG/角色切换仍需继续验证。 |
| 电池 | 部分支持 | PM8150B fuel gauge 可暴露容量、电压、电流、温度等基础信息。 |
| 充电 | 部分支持 | PM8150B charger 与标准 Type-C/PD 路径可用；已验证普通 PD 供电，私有快充未接入。 |
| 音频：主扬声器 | 已支持 | NXP TFA9874 路径已验证可播放。 |
| 音频：听筒 | 已支持 | WCD9380 earpiece 路径已验证可播放。 |
| 音频：3.5mm 耳机 | 部分支持 | HPHL/HPHR 播放、插入检测和阻抗读取已验证；耳麦麦克风未验证。 |
| 音频：机身麦克风 | 部分支持 | 已验证底部 mic1 与顶部 mic2 录音；第三个机身麦克风位置和路由仍待确认。 |
| 启动菜单 | 已支持 | initramfs 内实现，支持单系统也显示菜单、自动识别 rootfs、维护入口和下一次启动选择。 |

## 暂未支持或仍在适配中的硬件

| 硬件 | 当前状态 | 备注 |
| --- | --- | --- |
| 调制解调器 / 蜂窝网络 | 部分支持 | SDX55M 已推进到 PCIe/MHI/Sahara 诊断阶段，但还没有进入 Mission/AMSS；SIM、蜂窝数据、语音、IMS/VoLTE、ModemManager 均未可用。 |
| 摄像头 | 暂未支持 | 后置四摄和前置升降摄像头均未接入主线可用链路；详见下方摄像头支持情况。 |
| 前置升降结构 | 暂未支持 | 升降电机、限位/霍尔状态和安全策略尚未适配。 |
| 闪光灯 LED | 待适配 | PM8150L SPMI flash LED 尚未接入 LED class 测试。 |
| NFC | 待适配 | NQ-NCI I2C 路径尚未接入和验证。 |
| 传感器 | 待适配 | 需要 SDSP remoteproc、签名固件和传感器用户态栈；当前未启用。 |
| 触觉反馈 | 待适配 | AW8697 haptics 尚未接入和验证。 |
| USB-C analog/accessory mux | 待适配 | FSA4480 暂未完成验证。 |
| BQ2597x 充电泵 | 暂不支持 | Xiaomi 33W 私有快充依赖私有策略和测试条件，当前服务器使用目标优先普通 PD 稳定供电。 |

## 摄像头支持情况

当前摄像头整体标记为暂未支持。

lmi 机型硬件上包含四个后置摄像头和一个前置升降摄像头，但本仓库当前还没有完成以下主线 Linux 所需链路：

- Qualcomm CAMSS / CCI / CSIPHY / CSID / VFE 的设备树接入与实机验证。
- 各摄像头 sensor、EEPROM、VCM/actuator、供电、时钟和 reset GPIO 描述。
- 前置升降摄像头的电机控制、位置检测和防夹/超时保护。
- libcamera / V4L2 pipeline 验证。
- 相机闪光灯与拍照同步控制。

因此目前桌面系统中不应期待出现可用的内置摄像头设备。后续适配应先从只读识别、供电时序和单 sensor bring-up 开始，再推进多摄、自动对焦、闪光灯和前置升降结构。

## 构建

项目内的 lmi 构建入口在：

```sh
KERNEL_PROFILE=release ./lmi/scripts/build-kernel.sh
```

release 内核会嵌入当前 lmi initramfs，因此修改启动菜单或早期启动逻辑后，需要重新构建内核镜像并重新打包 boot image。

## 开源边界

本仓库只保存内核源码、DTS、配置片段、适配脚本和文档，不包含 boot image、rootfs 镜像、原厂 MIUI 镜像、设备分区备份、抓取日志或本地构建缓存。

本仓库仍是进行中的主线适配工作，不是完整量产系统。硬件状态以实机验证为准；仅在 downstream Android 中存在或只在代码中出现的硬件，不默认视为已支持。
