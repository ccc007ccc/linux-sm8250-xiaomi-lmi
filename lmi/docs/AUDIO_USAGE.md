# lmi 音频使用方法

> 100% AI 编写：本文档由 AI 生成和整理。

本文记录 Redmi K30 Pro / POCO F2 Pro（`lmi`）当前主线 Linux 内核下的音频路径、已验证状态和调试方法。命令面向当前 Ubuntu/rootfs 调试环境的 ALSA mixer；Phosh/PipeWire 侧可安装 [`../rootfs/audio/`](../rootfs/audio/) 分发包来暴露可选择的逻辑播放/录音设备，ALSA UCM 还没有整理成完整发行版配置。

## 当前状态

- 主扬声器：目标路径是 `plughw:0,0` / `MultiMedia1 -> PRI_MI2S_RX -> TFA9874`，用户侧按 mono 暴露，helper 会把 mono 输入复制成 2ch 后送入 PCM；前期曾验证过路线，但当前这台调试机的物理主扬声器已在 Android 下确认损坏，不能再用它做声学成功判定。
- 听筒：单独听筒走 `plughw:0,0` / `RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1`，RX3/AUX 路径，当前有声但音量小且有炸音；临时保留 `EAR_PA Volume=16` 的保守增益。
- 虚拟扬声器：PipeWire 逻辑 stereo 设备，左声道经 `plughw:0,1` / `RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia2` 送听筒，右声道经 `plughw:0,0` / `PRI_MI2S_RX Audio Mixer MultiMedia1` 送主扬声器；其主扬声器侧同样取决于 TFA9874 主扬声器修复。
- 底部麦克风：`AMIC1`，可持续录制 48 kHz S16_LE mono。
- 顶部麦克风：`AMIC5`，可持续录制 48 kHz S16_LE mono。
- 双麦录音：`AMIC1 + AMIC5`，可持续录制 48 kHz S16_LE stereo。
- 第三个机身麦克风尚未定位；3.5mm 耳麦麦克风也尚未验证。
- Phosh/PipeWire 声音设置中已验证 3 个输出和 3 个输入：`LMI-Speaker`、`LMI-Earpiece`、`LMI-Virtual-Speaker`；`LMI-Bottom-Mic`、`LMI-Top-Mic`、`LMI-Stereo-Mic`。单独扬声器、听筒、底麦和顶麦均按 mono 暴露；只有虚拟扬声器和双麦按 stereo 暴露。

内核前提：WCD capture DAI link 必须使用 TX SoundWire capture DAI `&swr2 1`。如果 DTS 误写成 `&swr2 0`，录音可能只有 startup transient，丢弃首秒后会静音。

## 通用注意事项

1. 测试声音前先从低振幅、短时长开始，避免主扬声器或听筒突然大声。
2. 当前已验证的 PCM 参数是：
   - `S16_LE`
   - `48000 Hz`
   - `period-size=480`
   - `buffer-size=960`
3. 下面命令默认 ALSA card 是 `0`。如果环境不同，先用 `aplay -l` / `arecord -l` 查看 card/device。
4. `EAR_PA Volume` 的数值方向容易误解：
   - `0` 是最大听筒 PA 增益（约 `+6 dB`）
   - `16` 是最小听筒 PA 增益（约 `-18 dB`）
5. 测试结束要关闭 mixer route，避免后续录音/播放互相串扰。

## 分发用 rootfs 配置包

仓库内提供可直接复制到其它 rootfs 的 PipeWire-Pulse 配置包：[`../rootfs/audio/`](../rootfs/audio/)。它安装 `/usr/local/bin/lmi-audio-pulse-setup` 和 XDG autostart 文件，负责创建 3 个输出、3 个输入，并在默认 sink/source 切换时切换 ALSA mixer route。

```sh
cd lmi/rootfs/audio
sudo ./install.sh
export XDG_RUNTIME_DIR=/run/user/0
export PULSE_SERVER=unix:$XDG_RUNTIME_DIR/pulse/native
/usr/local/bin/lmi-audio-pulse-setup --setup
/usr/local/bin/lmi-audio-pulse-setup --status
```

可先定义一个简写函数：

```sh
CARD=0
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }
```

## Phosh / PipeWire 逻辑设备

Ubuntu debug rootfs 中的 `/usr/local/bin/lmi-audio-pulse-setup` 会在 Phosh session 启动后配置 PipeWire Pulse 兼容层，给声音设置暴露稳定的逻辑设备：

- 输出：`LMI-Speaker`（`lmi_speaker`，mono）、`LMI-Earpiece`（`lmi_earpiece`，mono）、`LMI-Virtual-Speaker`（`lmi_virtual_speaker`，stereo，左听筒/右扬声器）
- 输入：`LMI-Bottom-Mic`（`lmi_bottom_mic`，mono）、`LMI-Top-Mic`（`lmi_top_mic`，mono）、`LMI-Stereo-Mic`（`lmi_stereo_mic`，stereo，底麦+顶麦）

主扬声器使用 `plughw:0,0` / `MultiMedia1`，PipeWire 中仍按 mono 暴露，helper 在 FIFO bridge 中复制成 2ch 后送硬件。单独听筒按用户可见 mono 暴露，底层使用 `plughw:0,0` / `RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1` 和 RX3/AUX route。注意：这里描述的是目标路由和用户可见模型，主扬声器侧还需要以实际听到声音为准。

虚拟扬声器是唯一的 stereo 输出。它不能把扬声器和听筒都挂到 `MultiMedia1`：`PRI_MI2S_RX Audio Mixer MultiMedia1` 与 `RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1` 会互相顶掉。当前 helper 使用 split bridge：左声道抽出为 mono 后写入 `plughw:0,1` / `RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia2` 听筒，右声道复制成 2ch 后写入 `plughw:0,0` / `PRI_MI2S_RX Audio Mixer MultiMedia1` 主扬声器。

两路麦克风共享 `hw:0,2` / `MultiMedia3`。helper 只保留 3 个用户可见输出和 3 个用户可见输入：PipeWire-Pulse `module-pipe-sink/source` 端点通过 FIFO 与后台 `aplay` / `arecord` bridge 相连；watcher 轮询默认 sink/source，在切换时关闭旧桥、设置 ALSA mixer route、再打开对应桥。这样 GNOME/Phosh 只需要选择逻辑设备，同时避免 Qualcomm DPCM 后端在旧 route 下保持打开导致听筒或麦克风切换卡住。

常用检查命令：

```sh
export XDG_RUNTIME_DIR=/run/user/0
export PULSE_SERVER=unix:$XDG_RUNTIME_DIR/pulse/native
pactl get-default-sink
pactl get-default-source
pactl list short sinks
pactl list short sources
wpctl status
```

默认设备应为 `lmi_speaker` 和 `lmi_bottom_mic`。`pactl list short sinks` 应只看到 3 个用户输出：`lmi_speaker`、`lmi_earpiece`、`lmi_virtual_speaker`；`pactl list short sources` 除这 3 个 sink 的 monitor 外，应只看到 3 个用户输入：`lmi_bottom_mic`、`lmi_top_mic`、`lmi_stereo_mic`。不再暴露 `LMI-Hardware-*`、mic bus 或虚拟左右声道等内部节点。

## 主扬声器播放

主扬声器走 `MultiMedia1 -> PRI_MI2S_RX -> TFA9874`。底层 PCM 使用 `plughw:0,0`、2ch；helper 的 `lmi_speaker` 对用户暴露 mono，并在后台复制成 stereo 送入硬件。下面是直接 ALSA 参考命令，用于排除听筒/WCD/WSA 串扰并验证 TFA9874 路径；只有实际听到主扬声器才算成功。

```sh
CARD=0
RATE=48000
RAW=/tmp/lmi-speaker-440.raw
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }

# 先关闭其它常用播放 route。
ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia2' 0 2>/dev/null || true
ctl 'WSA_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'WSA_CODEC_DMA_RX_1 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'AUX_RDAC Switch' 0 2>/dev/null || true
ctl 'EAR_RDAC Switch' 0 2>/dev/null || true
ctl 'RX_EAR Mode Switch' 0 2>/dev/null || true

python3 - <<'PY'
import math, struct
rate = 48000
dur = 1
amp = 3000
freq = 440
with open('/tmp/lmi-speaker-440.raw', 'wb') as f:
    for n in range(rate * dur):
        fade = min(1.0, n / (rate * 0.03), (rate * dur - n) / (rate * 0.03))
        v = int(amp * fade * math.sin(2 * math.pi * freq * n / rate))
        # 参考路线要求 2ch；左右写同一采样值即可得到 mono 主扬声器效果。
        f.write(struct.pack('<hh', v, v))
PY

ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 1
aplay -q -D plughw:0,0 -f S16_LE -r "$RATE" -c 2 \
  --period-size=480 --buffer-size=960 -t raw "$RAW"
ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 0
rm -f "$RAW"
```

如果需要更明显的测试音，可以把 `amp` 临时调到 `3000` 左右；不要一开始就使用很大的振幅。

## 听筒播放

听筒单独播放走 `MultiMedia1 -> RX_CODEC_DMA_RX_0 -> WCD9380 RX3/AUX -> EAR`。当前实测 `EAR_PA Volume=16` 有声且比最大增益更不容易炸音；如果还要比较音质，先保持短音频和较低振幅。

```sh
CARD=0
RATE=48000
RAW=/tmp/lmi-earpiece-1k.raw
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }

# 先关闭其它常用播放 route。
ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia2' 0 2>/dev/null || true
ctl 'WSA_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'WSA_CODEC_DMA_RX_1 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'AUX_RDAC Switch' 0 2>/dev/null || true
ctl 'EAR_RDAC Switch' 0 2>/dev/null || true
ctl 'RX_EAR Mode Switch' 0 2>/dev/null || true

# WCD9380 RX3/AUX EAR route；EAR_PA Volume=16 是当前较保守的实测值。
ctl 'RX_RX0 Digital Volume' 124 2>/dev/null || true
ctl 'RX_RX0 Mix Digital Volume' 124 2>/dev/null || true
ctl 'RX_RX1 Digital Volume' 124 2>/dev/null || true
ctl 'RX_RX1 Mix Digital Volume' 124 2>/dev/null || true
ctl 'RX_RX2 Digital Volume' 124 2>/dev/null || true
ctl 'RX_RX2 Mix Digital Volume' 124 2>/dev/null || true
ctl 'EAR_PA Volume' 16 2>/dev/null || true
ctl 'RX_MACRO RX0 MUX' AIF1_PB 2>/dev/null || true
ctl 'RX_MACRO RX3 MUX' AIF1_PB 2>/dev/null || true
ctl 'RX INT2_1 MIX1 INP0' RX0 2>/dev/null || true
ctl 'RX INT2_1 MIX1 INP1' ZERO 2>/dev/null || true
ctl 'RX INT2_1 MIX1 INP2' ZERO 2>/dev/null || true
ctl 'RX INT2_1 INTERP' 'RX INT2_1 MIX1' 2>/dev/null || true
ctl 'RDAC3_MUX' RX3 2>/dev/null || true
ctl 'AUX_RDAC Switch' 1 2>/dev/null || true
ctl 'EAR_RDAC Switch' 1 2>/dev/null || true
ctl 'RX_EAR Mode Switch' 1 2>/dev/null || true
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 1

python3 - <<'PY'
import math, struct
rate = 48000
dur = 1
amp = 1000
freq = 1000
with open('/tmp/lmi-earpiece-1k.raw', 'wb') as f:
    for n in range(rate * dur):
        fade = min(1.0, n / (rate * 0.05), (rate * dur - n) / (rate * 0.05))
        v = int(amp * fade * math.sin(2 * math.pi * freq * n / rate))
        f.write(struct.pack('<h', v))
PY

aplay -q -D plughw:0,0 -f S16_LE -r "$RATE" -c 1 \
  --period-size=480 --buffer-size=960 -t raw "$RAW"

ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'AUX_RDAC Switch' 0 2>/dev/null || true
ctl 'EAR_RDAC Switch' 0 2>/dev/null || true
ctl 'RX_EAR Mode Switch' 0 2>/dev/null || true
rm -f "$RAW"
```

## 底部麦克风 AMIC1 录音

底部麦克风当前定位为 `AMIC1`，录音路径是 `AMIC1 -> ADC1 -> TX DEC0 -> TX_CODEC_DMA_TX_3 -> MultiMedia3`。

```sh
CARD=0
OUT=/tmp/lmi-bottom-amic1.raw
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }

# 关闭常用旧 route，减少串扰。
ctl 'MultiMedia3 Mixer TX_CODEC_DMA_TX_3' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC0' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC1' 0 2>/dev/null || true
ctl 'ADC1 Switch' 0 2>/dev/null || true
ctl 'ADC1_MIXER Switch' 0 2>/dev/null || true
ctl 'ADC4 Switch' 0 2>/dev/null || true
ctl 'ADC4_MIXER Switch' 0 2>/dev/null || true

ctl 'ADC1 Volume' 20 2>/dev/null || true
ctl 'TX DEC0 MUX' SWR_MIC 2>/dev/null || true
ctl 'TX SMIC MUX0' ADC0 2>/dev/null || true
ctl 'ADC1 Switch' 1 2>/dev/null || true
ctl 'ADC1_MIXER Switch' 1 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC0' 1 2>/dev/null || true
ctl 'MultiMedia3 Mixer TX_CODEC_DMA_TX_3' 1

arecord -q -D hw:0,2 -f S16_LE -r 48000 -c 1 \
  --period-size=480 --buffer-size=960 -d 5 -t raw "$OUT"

ctl 'MultiMedia3 Mixer TX_CODEC_DMA_TX_3' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC0' 0 2>/dev/null || true
ctl 'ADC1 Switch' 0 2>/dev/null || true
ctl 'ADC1_MIXER Switch' 0 2>/dev/null || true
```

## 顶部麦克风 AMIC5 录音

顶部麦克风当前定位为 `AMIC5`，录音路径是 `AMIC5 -> ADC4(INP5) -> TX DEC0 -> TX_CODEC_DMA_TX_3 -> MultiMedia3`。

```sh
CARD=0
OUT=/tmp/lmi-top-amic5.raw
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }

# 关闭常用旧 route，减少串扰。
ctl 'MultiMedia3 Mixer TX_CODEC_DMA_TX_3' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC0' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC1' 0 2>/dev/null || true
ctl 'ADC1 Switch' 0 2>/dev/null || true
ctl 'ADC1_MIXER Switch' 0 2>/dev/null || true
ctl 'ADC4 Switch' 0 2>/dev/null || true
ctl 'ADC4_MIXER Switch' 0 2>/dev/null || true

ctl 'ADC4 Volume' 20 2>/dev/null || true
ctl 'ADC4 MUX' INP5 2>/dev/null || true
ctl 'TX DEC0 MUX' SWR_MIC 2>/dev/null || true
ctl 'TX SMIC MUX0' ADC3 2>/dev/null || true
ctl 'ADC4 Switch' 1 2>/dev/null || true
ctl 'ADC4_MIXER Switch' 1 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC0' 1 2>/dev/null || true
ctl 'MultiMedia3 Mixer TX_CODEC_DMA_TX_3' 1

arecord -q -D hw:0,2 -f S16_LE -r 48000 -c 1 \
  --period-size=480 --buffer-size=960 -d 5 -t raw "$OUT"

ctl 'MultiMedia3 Mixer TX_CODEC_DMA_TX_3' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC0' 0 2>/dev/null || true
ctl 'ADC4 Switch' 0 2>/dev/null || true
ctl 'ADC4_MIXER Switch' 0 2>/dev/null || true
```

## 双麦立体声录音

双麦录音使用 `AMIC1` 作为左声道、`AMIC5` 作为右声道：`ADC1 -> TX DEC0`，`ADC4(INP5) -> TX DEC1`，最后从 `hw:0,2` 以 2ch 采集。

```sh
CARD=0
OUT=/tmp/lmi-stereo-mic.raw
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }

# 关闭常用旧 route，减少串扰。
ctl 'MultiMedia3 Mixer TX_CODEC_DMA_TX_3' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC0' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC1' 0 2>/dev/null || true
ctl 'ADC1 Switch' 0 2>/dev/null || true
ctl 'ADC1_MIXER Switch' 0 2>/dev/null || true
ctl 'ADC4 Switch' 0 2>/dev/null || true
ctl 'ADC4_MIXER Switch' 0 2>/dev/null || true

ctl 'ADC1 Volume' 20 2>/dev/null || true
ctl 'ADC4 Volume' 20 2>/dev/null || true
ctl 'ADC4 MUX' INP5 2>/dev/null || true
ctl 'TX DEC0 MUX' SWR_MIC 2>/dev/null || true
ctl 'TX DEC1 MUX' SWR_MIC 2>/dev/null || true
ctl 'TX SMIC MUX0' ADC0 2>/dev/null || true
ctl 'TX SMIC MUX1' ADC3 2>/dev/null || true
ctl 'ADC1 Switch' 1 2>/dev/null || true
ctl 'ADC1_MIXER Switch' 1 2>/dev/null || true
ctl 'ADC4 Switch' 1 2>/dev/null || true
ctl 'ADC4_MIXER Switch' 1 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC0' 1 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC1' 1 2>/dev/null || true
ctl 'MultiMedia3 Mixer TX_CODEC_DMA_TX_3' 1

arecord -q -D hw:0,2 -f S16_LE -r 48000 -c 2 \
  --period-size=480 --buffer-size=960 -d 5 -t raw "$OUT"

ctl 'MultiMedia3 Mixer TX_CODEC_DMA_TX_3' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC0' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC1' 0 2>/dev/null || true
ctl 'ADC1 Switch' 0 2>/dev/null || true
ctl 'ADC1_MIXER Switch' 0 2>/dev/null || true
ctl 'ADC4 Switch' 0 2>/dev/null || true
ctl 'ADC4_MIXER Switch' 0 2>/dev/null || true
```

## 把麦克风录音经主扬声器回放

录音验证不要只看完整文件 peak。启动瞬间可能产生 transient，必须丢弃首秒后检查后段是否持续非零，再回放确认。下面把 mono raw 丢弃首秒、适度缩放后经主扬声器播放。

```sh
export IN=/tmp/lmi-bottom-amic1.raw      # 或 /tmp/lmi-top-amic5.raw
export PLAY=/tmp/lmi-mic-playback.raw

python3 - <<'PY'
import array, math, os, struct
inp = os.environ.get('IN', '/tmp/lmi-bottom-amic1.raw')
out = os.environ.get('PLAY', '/tmp/lmi-mic-playback.raw')
rate = 48000
with open(inp, 'rb') as f:
    data = f.read()
samples = array.array('h')
samples.frombytes(data)
# 丢弃首秒，只使用后续录音判断和回放。
tail = samples[rate:]
if not tail:
    raise SystemExit('recording too short')
peak = max(abs(x) for x in tail)
nonzero = sum(1 for x in tail if x)
rms = math.sqrt(sum(int(x) * int(x) for x in tail) / len(tail))
print(f'tail_after_1s: samples={len(tail)} peak={peak} rms={rms:.1f} nonzero={nonzero}/{len(tail)}')
if peak == 0 or nonzero == 0:
    raise SystemExit('tail is silent')
scale = min(1.0, 7000.0 / peak)
with open(out, 'wb') as f:
    for x in tail:
        v = int(max(-32768, min(32767, x * scale)))
        # 主扬声器参考路线使用 2ch PCM；mono 录音复制到左右声道。
        f.write(struct.pack('<hh', v, v))
print(f'wrote speaker playback raw: {out}, scale={scale:.2f}')
PY

CARD=0
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia2' 0 2>/dev/null || true
ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 1
aplay -q -D plughw:0,0 -f S16_LE -r 48000 -c 2 \
  --period-size=480 --buffer-size=960 -t raw "$PLAY"
ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 0
```

## 快速清理

```sh
CARD=0
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }

ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia2' 0 2>/dev/null || true
ctl 'WSA_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'WSA_CODEC_DMA_RX_1 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'AUX_RDAC Switch' 0 2>/dev/null || true
ctl 'EAR_RDAC Switch' 0 2>/dev/null || true
ctl 'RX_EAR Mode Switch' 0 2>/dev/null || true
ctl 'MultiMedia3 Mixer TX_CODEC_DMA_TX_3' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC0' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC1' 0 2>/dev/null || true
ctl 'ADC1 Switch' 0 2>/dev/null || true
ctl 'ADC1_MIXER Switch' 0 2>/dev/null || true
ctl 'ADC4 Switch' 0 2>/dev/null || true
ctl 'ADC4_MIXER Switch' 0 2>/dev/null || true
rm -f /tmp/lmi-speaker-440.raw /tmp/lmi-speaker-1k.raw /tmp/lmi-earpiece-1k.raw \
  /tmp/lmi-bottom-amic1.raw /tmp/lmi-top-amic5.raw /tmp/lmi-stereo-mic.raw \
  /tmp/lmi-mic-playback.raw
```
