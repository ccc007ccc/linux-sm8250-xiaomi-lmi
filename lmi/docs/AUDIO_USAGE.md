# lmi 音频使用方法

> 100% AI 编写：本文档由 AI 生成和整理。

本文记录 Redmi K30 Pro / POCO F2 Pro（`lmi`）当前主线 Linux 内核下已经实机验证过的扬声器、听筒和两路机身麦克风使用方法。命令面向当前 Ubuntu/rootfs 调试环境的 ALSA mixer；普通应用层音频策略、PulseAudio/PipeWire/ALSA UCM 还没有整理成完整发行版配置。

## 当前已验证状态

- 主扬声器：`PRI_MI2S_RX -> TFA9874`，可播放 48 kHz S16_LE stereo。
- 听筒：`RX_CODEC_DMA_RX_0 -> WCD9380 RX -> EAR`，可播放 48 kHz S16_LE；实测听筒本身音量偏小，需要把 `EAR_PA Volume` 调到最大端才容易听见。
- 底部/主麦克风：`AMIC1`，可持续录制 48 kHz S16_LE mono。
- 顶部麦克风：`AMIC5`，可持续录制 48 kHz S16_LE mono。
- 第三个机身麦克风尚未定位；3.5mm 耳麦麦克风也尚未验证。

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

可先定义一个简写函数：

```sh
CARD=0
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }
```

## 主扬声器播放

主扬声器走 `MultiMedia1 -> PRI_MI2S_RX -> TFA9874`。下面例子生成 2 秒 1 kHz 低振幅测试音并播放。

```sh
CARD=0
RATE=48000
RAW=/tmp/lmi-speaker-1k.raw
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }

# 先关闭其它常用播放 route。
ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 0 2>/dev/null || true

python3 - <<'PY'
import math, struct
rate = 48000
dur = 2
amp = 1000
freq = 1000
with open('/tmp/lmi-speaker-1k.raw', 'wb') as f:
    for n in range(rate * dur):
        v = int(amp * math.sin(2 * math.pi * freq * n / rate))
        f.write(struct.pack('<hh', v, v))
PY

ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 1
aplay -q -D hw:0,0 -f S16_LE -r "$RATE" -c 2 \
  --period-size=480 --buffer-size=960 -t raw "$RAW"
ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 0
rm -f "$RAW"
```

如果需要更明显的测试音，可以把 `amp` 临时调到 `3000` 左右；不要一开始就使用很大的振幅。

## 听筒播放

听筒走 `MultiMedia1 -> RX_CODEC_DMA_RX_0 -> WCD9380 RX -> EAR`。当前实测听筒需要最大 EAR PA 增益才容易听见，所以示例使用 `EAR_PA Volume = 0`。

```sh
CARD=0
RATE=48000
RAW=/tmp/lmi-earpiece-1k.raw
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }

# 先关闭其它常用播放 route。
ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'EAR_RDAC Switch' 0 2>/dev/null || true
ctl 'RX_EAR Mode Switch' 0 2>/dev/null || true

# WCD9380 EAR route。注意 EAR_PA Volume=0 是最大增益。
ctl 'RX_RX0 Digital Volume' 124 2>/dev/null || true
ctl 'RX_RX1 Digital Volume' 124 2>/dev/null || true
ctl 'RX_RX2 Digital Volume' 124 2>/dev/null || true
ctl 'EAR_PA Volume' 0 2>/dev/null || true
ctl 'RX_MACRO RX0 MUX' AIF1_PB 2>/dev/null || true
ctl 'RX INT0_1 MIX1 INP0' RX0 2>/dev/null || true
ctl 'RX INT0_2 MUX' ZERO 2>/dev/null || true
ctl 'RX INT0 DEM MUX' NORMAL_DSM_OUT 2>/dev/null || true
ctl 'RDAC3_MUX' RX3 2>/dev/null || true
ctl 'EAR_RDAC Switch' 1 2>/dev/null || true
ctl 'RX_EAR Mode Switch' 1 2>/dev/null || true
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 1

python3 - <<'PY'
import math, struct
rate = 48000
dur = 3
amp = 3000
freq = 1000
with open('/tmp/lmi-earpiece-1k.raw', 'wb') as f:
    for n in range(rate * dur):
        fade = min(1.0, n / (rate * 0.05), (rate * dur - n) / (rate * 0.05))
        v = int(amp * fade * math.sin(2 * math.pi * freq * n / rate))
        f.write(struct.pack('<hh', v, v))
PY

aplay -q -D hw:0,0 -f S16_LE -r "$RATE" -c 2 \
  --period-size=480 --buffer-size=960 -t raw "$RAW"

ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'EAR_RDAC Switch' 0 2>/dev/null || true
ctl 'RX_EAR Mode Switch' 0 2>/dev/null || true
rm -f "$RAW"
```

## 底部/主麦克风 AMIC1 录音

底部/主麦克风当前定位为 `AMIC1`，录音路径是 `AMIC1 -> ADC1 -> TX DEC0 -> TX_CODEC_DMA_TX_3 -> MultiMedia3`。

```sh
CARD=0
OUT=/tmp/lmi-main-amic1.raw
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }

# 关闭常用旧 route，减少串扰。
ctl 'MultiMedia3 Mixer TX_CODEC_DMA_TX_3' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC0' 0 2>/dev/null || true
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

## 把麦克风录音经主扬声器回放

录音验证不要只看完整文件 peak。启动瞬间可能产生 transient，必须丢弃首秒后检查后段是否持续非零，再回放确认。下面把 mono raw 丢弃首秒、适度缩放并转成 stereo，再走主扬声器播放。

```sh
export IN=/tmp/lmi-main-amic1.raw      # 或 /tmp/lmi-top-amic5.raw
export PLAY=/tmp/lmi-mic-playback.raw

python3 - <<'PY'
import array, math, os, struct
inp = os.environ.get('IN', '/tmp/lmi-main-amic1.raw')
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
        f.write(struct.pack('<hh', v, v))
print(f'wrote stereo playback raw: {out}, scale={scale:.2f}')
PY

CARD=0
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 1
aplay -q -D hw:0,0 -f S16_LE -r 48000 -c 2 \
  --period-size=480 --buffer-size=960 -t raw "$PLAY"
ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 0
```

## 快速清理

```sh
CARD=0
ctl() { amixer -q -c "$CARD" cset name="$1" "$2" >/dev/null; }

ctl 'PRI_MI2S_RX Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'RX_CODEC_DMA_RX_0 Audio Mixer MultiMedia1' 0 2>/dev/null || true
ctl 'EAR_RDAC Switch' 0 2>/dev/null || true
ctl 'RX_EAR Mode Switch' 0 2>/dev/null || true
ctl 'MultiMedia3 Mixer TX_CODEC_DMA_TX_3' 0 2>/dev/null || true
ctl 'TX_AIF1_CAP Mixer DEC0' 0 2>/dev/null || true
ctl 'ADC1 Switch' 0 2>/dev/null || true
ctl 'ADC1_MIXER Switch' 0 2>/dev/null || true
ctl 'ADC4 Switch' 0 2>/dev/null || true
ctl 'ADC4_MIXER Switch' 0 2>/dev/null || true
rm -f /tmp/lmi-speaker-1k.raw /tmp/lmi-earpiece-1k.raw \
  /tmp/lmi-main-amic1.raw /tmp/lmi-top-amic5.raw /tmp/lmi-mic-playback.raw
```
