# linux-sm8250-xiaomi-lmi

> 100% AI 编写：本文档由 AI 生成和整理。

这是 Redmi K30 Pro / POCO F2 Pro（代号 `lmi`，Qualcomm SM8250 / Snapdragon 865）的主线 Linux 内核适配仓库。

目标是让普通 Linux 发行版 rootfs 尽量保持纯净，把硬件适配放在内核、DTS、initramfs 和必要支持层中完成，而不是长期依赖发行版 rootfs hack。

## 当前状态

当前适配已经能从手机 UFS 上的 Linux rootfs 启动，通过 initramfs 菜单选择系统或维护入口，并进入 Ubuntu 26.04 Server。release 配置面向日常发行版和小型服务器场景，已经补齐 Docker、nftables/iptables、cgroup v2、overlayfs、zram、常见文件系统和网络能力。

当前主要验证组合：

- 设备：Redmi K30 Pro / POCO F2 Pro，代号 `lmi`。
- 系统：Ubuntu 26.04 Server arm64 rootfs。
- 启动：Android boot image + copydown bootshim + 内嵌 initramfs + UFS rootfs。
- rootfs：推荐 `/dev/sda34`，标签 `ubuntu-rootfs`。
- 刷写：使用 fastboot 刷入 boot 分区。

这仍是进行中的主线适配项目，不是完整量产手机 ROM。硬件状态以实机验证为准。

## 先读什么

| 需求 | 文档 |
| --- | --- |
| 只想使用现有 boot 跑 Linux rootfs | [`lmi/docs/USAGE.md`](lmi/docs/USAGE.md) |
| 想自己编译内核并打包 boot | [`lmi/docs/BUILD_AND_PACKAGING.md`](lmi/docs/BUILD_AND_PACKAGING.md) |
| 想看哪些硬件能用 | [`lmi/HARDWARE_SUPPORT.md`](lmi/HARDWARE_SUPPORT.md) |
| 想看适配细节和历史诊断 | [`lmi/ADAPTATION_NOTES.md`](lmi/ADAPTATION_NOTES.md) |
| 想看 lmi 文档目录 | [`lmi/docs/README.md`](lmi/docs/README.md) |

上游 Linux 通用说明仍保留在仓库根目录的 `README` 和 `Documentation/` 中；本仓库新增的 lmi 文档只描述这台设备的编译、启动和硬件适配。

## 仓库内容

| 路径 | 用途 |
| --- | --- |
| `arch/arm64/boot/dts/qcom/` | SM8250 与 lmi 设备树。 |
| `drivers/` | lmi bring-up 中涉及的主线驱动改动。 |
| `lmi/configs/` | lmi 内核配置片段，包含 debug/release 配置。 |
| `lmi/scripts/build-kernel.sh` | lmi 内核构建入口。 |
| `lmi/HARDWARE_SUPPORT.md` | 当前硬件支持状态矩阵。 |
| `lmi/ADAPTATION_NOTES.md` | 深度适配记录、非致命日志说明和历史诊断。 |
| `lmi/docs/` | 面向使用者和构建者的教程。 |

## 本地工具输入

主流程文档集中放在本仓库，但完整启动链路会引用同一工作区中的相邻工具仓库：

| 本地仓库 | 用途 |
| --- | --- |
| `sm8250-xiaomi-lmi-initramfs` | 早期启动、rootfs 自动发现、启动菜单、维护入口和 USB ACM 交接。 |
| `sm8250-xiaomi-lmi-boot` | Android boot image 打包、copydown bootshim、manifest 生成和 fastboot 辅助脚本。 |
| `sm8250-xiaomi-lmi-rootfs` | Ubuntu/Fedora rootfs 构建和 ext4 镜像辅助脚本。 |

这些仓库是本地构建输入和工具来源；面向用户的主教程以本仓库 `lmi/docs/` 为准。

## 快速编译

假设工作区路径为 `/home/ccc007/Android/Kernel/lmi`：

```sh
KERNEL_PROFILE=release /home/ccc007/Android/Kernel/lmi/linux-sm8250-xiaomi-lmi/lmi/scripts/build-kernel.sh
```

主要输出：

```text
/home/ccc007/Android/Kernel/lmi/linux-sm8250-xiaomi-lmi/out/m1-release/arch/arm64/boot/Image.gz
/home/ccc007/Android/Kernel/lmi/linux-sm8250-xiaomi-lmi/out/m1-release/arch/arm64/boot/dts/qcom/sm8250-xiaomi-lmi.dtb
```

完整编译、initramfs 更新、boot 打包和 manifest 检查见 [`lmi/docs/BUILD_AND_PACKAGING.md`](lmi/docs/BUILD_AND_PACKAGING.md)。

## 使用边界

本仓库只保存内核源码、DTS、配置片段、适配脚本和文档，不保存以下内容：

- 生成的 boot image。
- rootfs 镜像或 rootfs tar 包。
- 原厂 MIUI 镜像。
- 从原厂 boot/vendor/firmware 中提取的 stock DTB 或 firmware blob。
- 设备分区备份、抓取日志、本地 Wi-Fi/SSH 配置、密钥或密码。
- 本地构建缓存和临时输出。

boot 打包流程会引用本地捕获的 stock DTB / stock kernel DTB。包含这类输入的生成 boot image 不默认具备公开再分发条件；如果要发布二进制，必须先单独核对 manifest 和许可边界。
