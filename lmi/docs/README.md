# lmi 文档目录

> 100% AI 编写：本文档由 AI 生成和整理。

本目录集中保存 Redmi K30 Pro / POCO F2 Pro（`lmi`）主线 Linux 的使用和编译教程。所有面向用户的主流程说明都放在 `linux-sm8250-xiaomi-lmi` 仓库中；相邻 initramfs、boot、rootfs 仓库只作为构建工具和输入来源引用。

## 阅读顺序

1. [`USAGE.md`](USAGE.md)
   - 适合想把 rootfs 放进手机并启动 Linux 的用户。
   - 覆盖设备前提、rootfs 分区要求、刷 boot、启动菜单、电池保护和回滚。

2. [`BUILD_AND_PACKAGING.md`](BUILD_AND_PACKAGING.md)
   - 适合想自己编译内核和打包 boot image 的用户。
   - 覆盖 release 内核构建、initramfs 顺序、copydown boot 打包、manifest 检查和 stock blob 边界。

3. [`AUDIO_USAGE.md`](AUDIO_USAGE.md)
   - 适合想手动验证或临时使用扬声器、听筒和两路机身麦克风的用户。
   - 覆盖 ALSA mixer route、测试音、录音、录音回放和清理命令。

4. [`../HARDWARE_SUPPORT.md`](../HARDWARE_SUPPORT.md)
   - 快速判断当前哪些硬件已支持、部分支持、待适配或暂不支持。

5. [`../ADAPTATION_NOTES.md`](../ADAPTATION_NOTES.md)
   - 保存更长的 bring-up 记录、非致命 warning 解释和历史诊断过程。

## 不在本目录中的内容

- 上游 Linux 通用说明：仓库根目录 `README`。
- 上游 Linux 内核文档：仓库根目录 `Documentation/`。
- 生成的 boot image、rootfs 镜像、stock DTB、firmware blob 和本地私有配置：不应提交到公开仓库。
