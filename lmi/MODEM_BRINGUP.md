# lmi SDX55M / X55 基带适配记录

## Scope

本文件保存 Redmi K30 Pro / POCO F2 Pro (`lmi`) 上 SDX55M / X55 5G modem 的主线 Linux bring-up 摘要。它从 `ADAPTATION_NOTES.md` 抽离出来，避免主适配记录继续膨胀。

当前边界：

- 只做内核、DTS、initramfs/local firmware 与诊断节点验证。
- 不进入 EDL，不发送 firehose，不写 NV，不刷写 modem/dtbo/recovery/vbmeta/boot 分区。
- signed modem firmware 只允许放在 ignored/local firmware 路径，不提交到源码仓库。
- 当前目标是把 SDX55M 从 PCIe/SBL/Sahara bring-up 推到 Mission/AMSS；蜂窝数据、SIM、语音、MBIM/QMI 还未支持。

## Current status

| Area | Status | Summary |
| --- | --- | --- |
| PCIe endpoint | Partially working | PCIe2 + modem PHY 可枚举 SDX55M endpoint `17cb:0306` / subsystem `17cb:010c`，链路可到 Gen3 x2。 |
| MHI firmware boot | Partially working | `mhi-pci-generic` 改用 `qcom-sdx55m` profile 后能请求并加载 `qcom/sdx55m/sbl1.mbn`，modem 进入 SBL / `SECONDARY_BOOTLOADER` / M0。 |
| BL diagnostic channel | Working as diagnostic | downstream SBL `BL` channel 25 可生成 `/dev/mhi_bl0`，能读到 SBL boot log。 |
| SAHARA diagnostic channel | Working as diagnostic | `/dev/mhi_sahara0` 可读 Sahara HELLO，并能写 HELLO_RESP；后续 image transfer 仍未形成稳定 Mission boot。 |
| Event handling | Diagnostic workaround only | 主线正常 event MSI 计数长期为 0；主动 drain 能处理 completion/RX event，但不能作为最终通用修复。 |
| ESOC / sideband | Incomplete | AP2MDM/MDM2AP GPIO、PON warm reset、ESOC_REQ_IMG 等多轮诊断未让 modem 进入 AMSS/Mission。 |
| Mission / QMI / MBIM | Not working | 还没有 AMSS/Mission、QMI、MBIM、SIM、蜂窝数据或语音能力。 |

Firmware baseline:

- SDX55M SBL path: `qcom/sdx55m/sbl1.mbn`.
- Current local SBL evidence: size `548056`, SHA-256 `0fd2fdaf19831c8ff482ca77ca236ee282101c9bdf5ab7b46dc4e24a484e84dc`.
- Firmware must remain in ignored/local paths such as `sm8250-xiaomi-lmi-initramfs/local/firmware/qcom/sdx55m/`.

## Key conclusions

- The blocker is no longer basic PCIe, PERST/CLKREQ/WAKE, signed SBL loading, MHI channel table size, BL/Sahara data path, HELLO_RESP response latency, or simple HELLO_RESP mode selection.
- SDX55M reaches SBL/M0 and can expose BL/Sahara channels, but SBL does not reliably progress through the expected image transfer / Mission transition sequence.
- Android downstream behavior depends on more than generic MHI: ESOC lifecycle, request-image state, modem PON reset timing, `ks`/Sahara image-loader semantics, and channel lifecycle all matter.
- Many RX-ring/restart/doorbell experiments produced only negative or diagnostic results; do not keep adding random MHI quirks without new evidence from downstream or protocol traces.

## Milestone summary

### M16-M18 — PCIe endpoint and SBL load

- Enabled SM8250 PCIe2 and modem PCIe PHY.
- SDX55M endpoint enumerated as `17cb:0306` / subsystem `17cb:010c`.
- Mainline initially matched `foxconn-sdx55`, which had no firmware path; lmi needs `qcom-sdx55m` so the driver requests `qcom/sdx55m/sbl1.mbn`.
- Staging signed `sbl1.mbn` in ignored initramfs/local firmware allowed BHI firmware load and moved the device to `SECONDARY_BOOTLOADER` / M0.

### M19-M24 — sideband GPIO and first Sahara visibility

- Static AP/MDM sideband GPIO checks did not make modem enter Mission.
- Adding SBL `SAHARA` channel 2/3 alone did not create `mhi0_SAHARA`; the host still lacked a natural SBL EE transition.
- Temporary EE polling and forced M0 diagnostics proved hardware had reached SBL and could instantiate `mhi0_SAHARA`.
- `/dev/mhi_sahara0` became usable only as a diagnostic endpoint, not as full modem support.

### M27-M32 — event ring drain and HELLO

- START completion and a 48-byte Sahara HELLO were present in the event ring.
- Mainline IRQ/tasklet path did not process SDX55M SBL events promptly; forced shared MSI was not sufficient.
- Active event drain proved event parsing, callbacks, and the read path work, but SDX55M event MSI counters stayed at 0.
- This narrowed the issue to SBL-stage interrupt/lifecycle behavior, not basic event-ring memory layout.

### M39-M47 — BL channel, warm reset, MSI mapping, HELLO_RESP

- Added downstream-like SBL `BL` channel 25 and `/dev/mhi_bl0`; reading it produced SBL boot logs including secure boot and SBL version strings.
- PON modem warm reset via PMIC USID8 did not disturb AP boot and preserved BL/Sahara baselines, but did not trigger Mission or MDM2AP sideband changes.
- Event context/MSI mapping looked valid (`er1` mapped to a Linux IRQ), while SDX55M event MSI counters still did not increment.
- Kernel-generated HELLO_RESP eliminated userspace response latency as the cause; UL completion was visible, but SBL still did not continue to stable image transfer.

### M48-M58 — Sahara open timing and first READ_DATA

- Probe-time early SAHARA auto-start caused SYS_ERROR and BL timeout, so it is not a valid fix.
- Delayed SAHARA start after BL restored the baseline but still did not produce stable follow-up data.
- Closer downstream UCI-like behavior, open-time SAHARA start, prequeued RX, and close-time unprepare narrowed the problem.
- A later channel restart finally produced a real Sahara `READ_DATA` packet for image id 34 (`TRDATA` / `mdmddr` path), proving SBL can request image data under the right channel lifecycle.

### M59-M67 — minimal image-loader and restart semantics

- Added read-only minimal Sahara image response experiments for image 34 and related image ids.
- Tested live `mdmddr` / `msadp` multi-image behavior, image40 handling, keep-prepared semantics, fallback polling, and restart/default policies.
- These tests improved protocol visibility but did not reach Mission.
- Important result: avoid firehose/EDL/NV-style payloads; this path is about signed AMSS image loading only.

### M69-M78 — APDP, image40, ESOC and Android `ks` reverse-engineering

- Multi-TRE receive and BL auto-start were consolidated.
- APDP tail sideband and MHI state sampling did not explain the stall.
- image40 end-of-image, close/reopen cadence, command-mode HELLO_RESP override, ESOC_REQ_IMG zero-wait, and Android `ks` pending/DONE_RESP semantics were checked.
- Reverse-engineering Android `ks -a 9:mdmddr` and CMD_READY behavior suggested that request-image state and image-gated pending semantics are load-bearing.

### M79-M121 — RX ring, BAD_TRE, async requeue and restart negative results

- Tested skipped RX TRE handling, continuous reads on the same fd, active drain, ring/doorbell diagnostics, channel-lock doorbell behavior, restart-after-UL, delayed/cancellable restart, invalid zero RX drop, large READ_DATA handling, full READ_DATA tracking, grouped transfer, doorbell resync, HELLO_RESP restart suppression, UCI-style async requeue, event-ring DB re-ring, runtime PM hold, and ESOC state notifications.
- Most changes were negative diagnostics: they did not move the modem to Mission and should not be treated as final fixes.
- Final summarized state: BL and Sahara diagnostics are useful, but stable AMSS loading likely needs a cleaner model of downstream ESOC/request-image lifecycle plus correct Sahara image response sequencing, not more blind RX/restart heuristics.

## Known pitfalls

- Do not leave `17cb:0306` / `17cb:010c` matched to a firmware-less `foxconn-sdx55` profile on lmi.
- Do not commit `sbl1.mbn`, `qdsp6sw.mbn`, AMSS images, or any extracted modem blobs.
- Do not treat `/dev/mhi_sahara0` or `/dev/mhi_bl0` as user-facing modem support; they are bring-up diagnostics.
- Do not make active event drain, forced M0, direct PON writes, or EE polling generic fixes without a narrow lmi-specific justification.
- Do not enter EDL, send firehose, erase/write NV, or perform destructive modem flashing while debugging this path.
- If a test reaches SYS_ERROR or invalid EE, preserve logs and revert the timing/lifecycle change instead of stacking more workarounds.

## Next useful work

1. Re-check downstream Android ESOC/request-engine state transitions around `ESOC_REQ_IMG`, `ESOC_IMG_XFER_DONE`, `ESOC_BOOT_DONE`, and MHI power-up timing.
2. Keep `BL` and `SAHARA` diagnostic clients minimal and clearly marked as diagnostics.
3. Build a bounded, read-only Sahara image-loader experiment that only serves the signed local images SBL asks for, with explicit image-id gating and no firehose/EDL/NV path.
4. Verify whether Mission transition depends on sideband state changes that the current mainline path does not model.
5. Only document modem as supported after AMSS/Mission, QMI/MBIM enumeration, SIM/network behavior, suspend/resume, and cleanup are verified.

## Validation commands

Representative checks after a modem test boot:

```sh
dmesg | grep -Ei 'mhi|sdx55|sahara|modem|esoc|bhi|pcie|mdm|ap2mdm|mhi_bl'
lspci -nn
ls -l /dev/mhi* /sys/bus/mhi/devices/ 2>/dev/null
cat /proc/interrupts | grep -Ei 'mhi|sdx55|pcie'
```

Diagnostic reads, only when the matching test build intentionally exposes the nodes:

```sh
# SBL log diagnostic; read-only.
dd if=/dev/mhi_bl0 bs=4096 count=1 of=/tmp/lmi-sdx55m-bl.bin

# Sahara diagnostic; do not write firehose/EDL/NV payloads.
dd if=/dev/mhi_sahara0 bs=48 count=1 of=/tmp/lmi-sdx55m-sahara-hello.bin
```

Cleanup after tests should remove temporary logs, firmware staging, scripts, and modules from the device if they were copied under `/tmp` or `/run/firmware`.
