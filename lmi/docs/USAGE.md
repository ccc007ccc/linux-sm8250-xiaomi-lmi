# lmi 主线 Linux 使用教程

> 100% AI 编写：本文档由 AI 生成和整理。

本文说明如何在 Redmi K30 Pro / POCO F2 Pro（`lmi`）上使用本项目的主线 Linux boot 启动手机 UFS 中的 Linux rootfs。

## 适用设备

- Redmi K30 Pro / POCO F2 Pro。
- 设备代号：`lmi`。
- SoC：Qualcomm SM8250 / Snapdragon 865。
- bootloader：必须已解锁。
- 推荐刷写入口：recovery fastbootd。

不要把本文步骤用于其他 SM8250 设备，也不要用于未确认分区布局的设备。

## 启动链路

本项目的当前启动链路是：

```text
Android boot image
  -> copydown bootshim
  -> Linux Image.gz + sm8250-xiaomi-lmi.dtb
  -> 内嵌 initramfs
  -> 扫描 UFS 上的 Linux rootfs
  -> switch_root 进入发行版系统
```

boot image 只提供内核、DTB、initramfs 和启动支持，不内置完整 Linux 发行版。手机 UFS 上必须提前准备 rootfs。

## rootfs 基本要求

initramfs 只会把满足以下条件的分区列为可启动系统：

- 文件系统类型为 `ext2`、`ext3`、`ext4`、`btrfs` 或 `xfs`。
- rootfs 根目录中存在 `/etc/os-release`。
- rootfs 根目录中存在 `/sbin/init`。

btrfs rootfs 可以直接位于文件系统根目录，也可以位于以下子卷/目录：

```text
root
@root
@
```

Ubuntu rootfs 中建议至少准备常规挂载点：

```sh
mkdir -p proc sys dev run tmp boot
```

## 推荐分区和标签

当前主要验证目标是 Ubuntu 26.04 Server arm64 rootfs。推荐分区布局：

```text
/dev/sda34 或 /dev/block/sda34
LABEL=ubuntu-rootfs
```

兼容路径：

```text
/dev/sda35 或 /dev/block/sda35
PARTNAME=linuxroot
```

initramfs 的基础默认 rootfs label 是 `ubuntu-rootfs`。当前 release profile 逻辑会优先尝试 `fedora-rootfs`，但如果没有严格指定 rootfs，它仍会扫描并显示符合条件的 rootfs 候选；实际启动时可以在菜单中选择 Ubuntu 26.04 rootfs。

如果需要显式指定 label，可在 kernel cmdline 使用：

```text
lmi.root_label=ubuntu-rootfs
```

或使用标准 root 参数：

```text
root=LABEL=ubuntu-rootfs
```

## 把 rootfs 放进手机

以下命令会清空目标 rootfs 分区。执行前必须确认设备节点确实是为 Linux rootfs 预留的分区，不要对 Android 系统分区执行。

### 方式一：在 recovery 或已有 Linux 环境中解包 tar

如果环境中能访问 Android 风格 block device：

```sh
mkfs.ext4 -F -L ubuntu-rootfs /dev/block/sda34
mkdir -p /mnt/linuxroot
mount -t ext4 /dev/block/sda34 /mnt/linuxroot
tar --numeric-owner -xpf ubuntu-arm64-rootfs.tar -C /mnt/linuxroot
sync
umount /mnt/linuxroot
```

如果已经在主线 Linux 中操作，设备节点通常是 `/dev/sda34`：

```sh
sudo mkfs.ext4 -F -L ubuntu-rootfs /dev/sda34
sudo mkdir -p /mnt/linuxroot
sudo mount -t ext4 /dev/sda34 /mnt/linuxroot
sudo tar --numeric-owner -xpf ubuntu-arm64-rootfs.tar -C /mnt/linuxroot
sync
sudo umount /mnt/linuxroot
```

解包后建议检查：

```sh
ls /mnt/linuxroot/etc/os-release
ls /mnt/linuxroot/sbin/init
```

### 方式二：在 fastbootd 中写入 ext4 镜像

只有在 recovery fastbootd 能看到 `linuxroot` 分区名时，才使用这种方式：

```sh
fastboot getvar is-userspace
fastboot flash linuxroot ubuntu-rootfs.ext4.img
```

`is-userspace` 必须返回 `yes`。不同设备或不同分区表不一定存在 `linuxroot` 这个 fastboot 分区名；不确定时不要执行 `fastboot flash linuxroot`。

## 刷入 boot image

进入 recovery fastbootd 后先确认：

```sh
fastboot getvar is-userspace
```

期望输出包含：

```text
is-userspace: yes
```

然后刷入 boot：

```sh
fastboot flash boot boot-linux-copydown-lmi.img
fastboot reboot
```

在 WSL 中操作时，建议调用 Windows platform-tools 中的 `fastboot.exe`。

## 首次启动菜单

启动后 initramfs 会显示 `LMI Boot Menu`：

- 音量上 / 音量下：选择 rootfs 或维护入口。
- 电源键：确认启动。
- 没有按键操作时：短倒计时后启动默认候选。

菜单中会显示检测到的发行版名称、分区标签和设备节点。即使只有一个 rootfs，也会显示菜单，方便进入 recovery、fastbootd 或 bootloader fastboot。

## 进入系统后

当前已验证 Ubuntu 26.04 Server 的基础能力包括：

- UFS rootfs 启动。
- fbcon/DRM 显示。
- 触摸和实体按键输入。
- Wi-Fi 和 SSH。
- USB ACM 串口。
- Docker bridge、端口映射、overlayfs、cgroup v2、nftables/iptables。

具体硬件状态见 [`../HARDWARE_SUPPORT.md`](../HARDWARE_SUPPORT.md)。

## 回滚

发布版 boot 只刷写 `boot` 分区。回滚时刷回原厂或备份 boot image：

```sh
fastboot getvar is-userspace
fastboot flash boot stock-boot.img
fastboot reboot
```

首次刷入前建议备份当前 boot 分区，并确保自己仍能进入 recovery fastbootd。

## 限制和公开发布边界

- 本教程不提供 rootfs 镜像，只说明 rootfs 应该如何放进手机。
- 本教程不提供 stock DTB、MIUI 镜像、firmware blob 或设备分区备份。
- 当前 boot 打包流程可能引用本地捕获的 stock DTB / stock kernel DTB；包含这些内容的 boot image 不默认可公开再分发。
- 当前适配不是完整稳定手机 ROM。调制解调器、摄像头、传感器、私有快充等仍未达到可宣传为日常可用的状态。
