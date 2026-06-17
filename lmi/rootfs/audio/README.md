# lmi rootfs 音频配置包

这个目录是给 Redmi K30 Pro / POCO F2 Pro（`lmi`）其它 rootfs 分发用的音频支持层。它不依赖当前调试 rootfs 的私有状态，安装后通过 PipeWire-Pulse/PulseAudio 兼容接口暴露稳定的逻辑设备，让 Phosh/GNOME 声音设置能正常识别 lmi 的麦克风、主扬声器和听筒。

## 暴露的逻辑设备

输出：

- `LMI-Speaker` / `lmi_speaker`：mono，后台复制为 2ch 后走 `MultiMedia1 -> PRI_MI2S_RX -> TFA9874`。
- `LMI-Earpiece` / `lmi_earpiece`：mono，走 `MultiMedia1 -> RX_CODEC_DMA_RX_0 -> WCD9380 RX3/AUX -> EAR`。
- `LMI-Virtual-Speaker` / `lmi_virtual_speaker`：stereo，左声道走听筒，右声道走主扬声器；用于临时把两个物理输出同时暴露给桌面环境。

输入：

- `LMI-Bottom-Mic` / `lmi_bottom_mic`：mono，`AMIC1 -> ADC1 -> TX DEC0 -> TX_CODEC_DMA_TX_3 -> MultiMedia3`。
- `LMI-Top-Mic` / `lmi_top_mic`：mono，`AMIC5 -> ADC4(INP5) -> TX DEC0 -> TX_CODEC_DMA_TX_3 -> MultiMedia3`。
- `LMI-Stereo-Mic` / `lmi_stereo_mic`：stereo，左 `AMIC1`，右 `AMIC5`。

## 依赖

rootfs 需要安装：

- `pipewire-pulse` 或可用的 PulseAudio 服务；
- `pulseaudio-utils`（提供 `pactl`）；
- `alsa-utils`（提供 `amixer`、`aplay`、`arecord`）；
- `python3`。

内核侧前提：

- sound card 名称为 `Xiaomi lmi`，通常是 ALSA card 0；如果发行版不是 card 0，可通过环境变量 `LMI_AUDIO_CARD=<card>` 覆盖。
- WCD capture DAI link 必须使用 TX SoundWire capture DAI `&swr2 1`，否则录音可能只剩启动瞬态。
- 主扬声器目标路径是 `PRI_MI2S_RX -> TFA9874`；听筒和麦克风走 WCD9380。

## 安装到 rootfs

在目标 rootfs 内以 root 执行：

```sh
cd /path/to/linux-sm8250-xiaomi-lmi/lmi/rootfs/audio
./install.sh
```

安装内容：

- `/usr/local/bin/lmi-audio-pulse-setup`
- `/etc/xdg/autostart/lmi-audio-pulse-setup.desktop`

桌面会话启动后，autostart 会运行 `lmi-audio-pulse-setup --setup`。也可以手动执行：

```sh
export XDG_RUNTIME_DIR=/run/user/0
export PULSE_SERVER=unix:$XDG_RUNTIME_DIR/pulse/native
/usr/local/bin/lmi-audio-pulse-setup --setup
/usr/local/bin/lmi-audio-pulse-setup --status
```

如果需要停止本包创建的 bridge/watch 进程和 Pulse 模块：

```sh
/usr/local/bin/lmi-audio-pulse-setup --stop
```

脚本只会按 pidfile 且校验 `/proc/$pid/cmdline` 后停止自己创建的进程，不会广泛 kill `aplay`/`arecord`/PipeWire；若进程在等待期间已退出并发生 PID 复用，二次校验失败时不会发送 `SIGKILL`。

## 验证

```sh
export XDG_RUNTIME_DIR=/run/user/0
export PULSE_SERVER=unix:$XDG_RUNTIME_DIR/pulse/native
pactl get-default-sink
pactl get-default-source
pactl list short sinks | grep lmi_
pactl list short sources | grep lmi_
```

期望默认输出/输入为：

```text
lmi_speaker
lmi_bottom_mic
```

用户可见输出应包含 `lmi_speaker`、`lmi_earpiece`、`lmi_virtual_speaker`；用户可见输入应包含 `lmi_bottom_mic`、`lmi_top_mic`、`lmi_stereo_mic`。

## 注意

- 主扬声器必须用实际听感或强声学相关性验证；PCM、DAPM、TFA 寄存器活跃不能单独当作“听到声音”。
- 当前这台调试机的物理主扬声器已确认损坏，因此它不能再作为主扬声器声学验证参考；配置包仍保留正确的 TFA9874 目标路由，供扬声器硬件正常的机器/rootfs 使用。
- 听筒测试请先用低振幅、短时长，`EAR_PA Volume=16` 是当前保守增益。
- 两路麦克风共享 `hw:0,2`，切换默认 source 时脚本会先关闭旧 capture bridge，再切换 mixer route。
