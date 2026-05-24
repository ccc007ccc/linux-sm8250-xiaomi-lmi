# lmi 内核编译与 boot 打包教程

> 100% AI 编写：本文档由 AI 生成和整理。

本文说明如何在本地编译 `linux-sm8250-xiaomi-lmi` release 内核，并用相邻 boot 工具仓库打包 lmi 可刷入的 Android boot image。

## 工作区约定

本文使用以下本地路径作为示例：

```text
/home/ccc007/Android/Kernel/lmi/linux-sm8250-xiaomi-lmi
/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-initramfs
/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-boot
/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-rootfs
```

其中：

- `linux-sm8250-xiaomi-lmi`：内核源码、DTS、配置和主文档。
- `sm8250-xiaomi-lmi-initramfs`：早期启动 initramfs 输出。
- `sm8250-xiaomi-lmi-boot`：Android boot image 打包工具。
- `sm8250-xiaomi-lmi-rootfs`：rootfs 构建和镜像辅助脚本。

主教程集中放在内核仓库；相邻仓库只是构建输入和工具来源。

## 构建 initramfs

release 内核会嵌入当前 lmi initramfs。修改 initramfs 菜单、rootfs 发现逻辑、USB ACM 交接或早期 firmware staging 后，需要先构建 initramfs。

```sh
/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-initramfs/scripts/build-initramfs.sh
```

主要输出：

```text
/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-initramfs/out/initramfs-sm8250-xiaomi-lmi.cpio.gz
/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-initramfs/out/initramfs-sm8250-xiaomi-lmi.manifest
```

内核配置片段 `lmi/configs/m1.config` 通过 `CONFIG_INITRAMFS_SOURCE` 引用该 cpio.gz，因此 initramfs 改动不会在不重编内核的情况下自动进入 boot image。

## 编译 release 内核

内核构建入口：

```sh
KERNEL_PROFILE=release /home/ccc007/Android/Kernel/lmi/linux-sm8250-xiaomi-lmi/lmi/scripts/build-kernel.sh
```

`build-kernel.sh` 默认 profile 是 `debug`；日常发行版和服务器场景应显式使用 `KERNEL_PROFILE=release`。

release 构建主要合并：

```text
/home/ccc007/Android/Kernel/lmi/linux-sm8250-xiaomi-lmi/lmi/configs/m1.config
/home/ccc007/Android/Kernel/lmi/linux-sm8250-xiaomi-lmi/lmi/configs/m1-release.config
```

主要输出：

```text
/home/ccc007/Android/Kernel/lmi/linux-sm8250-xiaomi-lmi/out/m1-release/arch/arm64/boot/Image.gz
/home/ccc007/Android/Kernel/lmi/linux-sm8250-xiaomi-lmi/out/m1-release/arch/arm64/boot/dts/qcom/sm8250-xiaomi-lmi.dtb
```

构建完成后至少确认这两个文件存在。

## 打包 Android boot image

当前 lmi 走 copydown bootshim：Android boot image 先进入 bootshim，bootshim 再把内嵌 Linux Image 复制到目标位置，并用运行时 DTB 跳入 Linux。

打包命令：

```sh
OUT_DIR=/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-boot/builds/lmi-release \
LINUX_GZIP=/home/ccc007/Android/Kernel/lmi/linux-sm8250-xiaomi-lmi/out/m1-release/arch/arm64/boot/Image.gz \
RUNTIME_DTB=/home/ccc007/Android/Kernel/lmi/linux-sm8250-xiaomi-lmi/out/m1-release/arch/arm64/boot/dts/qcom/sm8250-xiaomi-lmi.dtb \
/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-boot/scripts/mkboot-linux-copydown-lmi.sh
```

生成目录中会出现：

```text
boot-linux-copydown-lmi.img
boot-linux-copydown-lmi.manifest
```

## manifest 必查项

打包后先读 manifest，不要只看是否生成了 `.img`。

关键字段应满足：

```text
stage=M2j
payload=linux-copydown-shim-embedded-runtime-dtb
x0=embedded_runtime_dtb
linux_source_alignment_ok=True
copy_entry_outside_destination=True
copy_overlap_safe=True
boot_size_ok=True
```

还应核对：

```text
boot_img_sha256=...
boot_img_size=...
ramdisk_sha256=...
runtime_dtb_sha256=...
embedded_linux_image_sha256=...
```

`boot_size_ok=True` 只说明当前 image 没超过 boot 分区大小，不代表适用于其他机型。

## 刷入 boot

刷写前进入 recovery fastbootd，并确认：

```sh
fastboot getvar is-userspace
```

期望：

```text
is-userspace: yes
```

刷入：

```sh
fastboot flash boot /home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-boot/builds/lmi-release/boot-linux-copydown-lmi.img
fastboot reboot
```

bootloader fastboot 在当前 lmi 分区状态下不作为主要刷写路径。

## 生成物公开边界

源码和教程可以公开，但生成的 boot image 不默认可以公开再分发。

原因：当前 `mkboot-linux-copydown-lmi.sh` 打包流程会引用本地捕获的 stock DTB / stock kernel DTB，用于兼容 Android ABL/boot 链路。这些输入来自设备或原厂镜像，不应提交到公开源码仓库，也不应在未确认许可前随 release asset 上传。

公开发布二进制前至少要确认：

- boot image 是否包含 stock DTB、stock kernel DTB 或其他原厂 blob。
- initramfs 是否包含不可再分发 firmware。
- manifest 中的输入路径和 sha256 是否可追溯。
- release note 是否明确适用设备、回滚方式和已知限制。

如果无法确认许可边界，只发布源码、教程和本地构建方法，不发布 boot 二进制。

## 推荐完整顺序

```text
1. 准备或更新 rootfs。
2. 构建 initramfs。
3. 构建 release 内核。
4. 使用 boot 工具打包 copydown boot image。
5. 检查 manifest。
6. 在 recovery fastbootd 中刷 boot。
7. 启动后通过菜单进入 rootfs。
8. 用 SSH、USB ACM 串口或本机 console 验证系统。
```

使用教程见 [`USAGE.md`](USAGE.md)。硬件状态见 [`../HARDWARE_SUPPORT.md`](../HARDWARE_SUPPORT.md)。
