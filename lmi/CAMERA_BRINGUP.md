# lmi camera bring-up notes

## Scope

目标是在 lmi 主线内核中逐步适配摄像头。第一阶段只启用一个后摄传感器，用于验证 SM8250 CAMSS、CCI、CSIPHY、sensor driver、media graph 的基础链路。

第一阶段目标：后置超广角 OV13B10。

暂不启用：

- 前摄 S5K3T2：升降摄像头，机械控制和传感器链路分开处理。
- 主摄 IMX686：目标树暂未确认有可直接复用的主线驱动。
- 长焦 OV08A10、微距 S5K5E9YX04、景深 GC02M1。
- EEPROM、actuator、OIS、flash LED。

## Reference inputs

- Android kernel camera DTS: `android_kernel_xiaomi_sm8250/arch/arm64/boot/dts/vendor/qcom/lmi-sm8250-camera-sensor-mtp.dtsi`
- Android kernel overlay: `android_kernel_xiaomi_sm8250/arch/arm64/boot/dts/vendor/qcom/lmi-sm8250-overlay.dts`
- Stock extracted artifacts: `out/`, especially `out/dtb`, `out/recovery_dtbo`, `out/lmi-firmware-v14/vendor.img`
- User-supplied lens/spec table: `lmi/redmi_k30_pro_camera_specs.md`
- Target mainline tree: `lmi/linux-sm8250-xiaomi-lmi`

## Camera inventory

| Role | Module hint | Android/vendor evidence | First status |
| --- | --- | --- | --- |
| Rear wide | Sunny IMX686 | `com.qti.sensor.imx686_lmi.so`, `lmi_sunny_imx686_wide` | Deferred |
| Rear ultra-wide | Sunny OV13B10 | `com.qti.sensor.ov13b10_lmi.so`, `lmi_sunny_ov13b10_ultra`; user spec table now also lists OmniVision OV13B10 | First target |
| Rear tele | Sunny OV08A10 | `com.qti.sensor.ov08a10_lmi.so`, `lmi_sunny_ov08a10_tele` | Deferred |
| Rear macro | Sunny S5K5E9YX04 | `com.qti.sensor.s5k5e9yx04_lmi.so`, `lmi_sunny_s5k5e9yx04_macro` | Deferred |
| Rear depth | OFilm GC02M1 | `com.qti.sensor.gc02m1_lmi.so`, `lmi_ofilm_gc02m1_depth` | Deferred |
| Front pop-up | Sunny S5K3T2 | `com.qti.sensor.s5k3t2_lmi.so`, `lmi_sunny_s5k3t2_front` | Deferred |

## Android DTS topology notes

### Rear ultra-wide OV13B10 first target

Known downstream hints:

- CSIPHY: 1
- CCI master: 1
- MCLK: MCLK2 / GPIO96
- Reset/control GPIO: GPIO91
- Analog rail: `camera_ultra_vana_ldo`, 2.8 V, enable GPIO63, parent `pm8150a_bob`
- Additional rails referenced from PM8009: L7 and L3

Spec-table reconciliation:

- `lmi/redmi_k30_pro_camera_specs.md` was updated to list the ultra-wide sensor as OmniVision OV13B10, matching the checked stock/vendor `ov13b10_lmi` and `lmi_sunny_ov13b10_ultra` evidence.
- The spec table is also useful for non-probe constraints: ultra-wide is fixed-focus/no AF at the module level even if the sensor has PDAF pixels, macro needs AF, and front S5K3T2 is fixed-focus but still deferred because of the pop-up mechanism.

Mainline mapping candidates:

- Sensor bus first candidate: `&cci0_i2c1`
- CAMSS endpoint first candidate: `&camss` `port@1`
- MCLK first candidate: `CAM_CC_MCLK2_CLK`
- Reset GPIO first candidate: `GPIO_ACTIVE_LOW` on TLMM GPIO91

Verified variables:

- OV13B10 I2C address: 7-bit `0x10`. A temporary powered address scan found chip ID `0x560d42` at `0x10`; the earlier `0x36` candidate NACKed.
- Downstream `cci-master = <1>` mapping: current working mainline mapping is `&cci0_i2c1`, which probes as `ov13b10 21-0010`.
- Reset GPIO polarity: downstream-matching `GPIO_ACTIVE_LOW` on TLMM GPIO91 is the working candidate; an active-high test failed earlier.
- PM8009 L3/L7 rail load votes: matching downstream load votes did not fix the earlier `0x36` NACK, but are kept as downstream power-parity for the working `0x10` path.
- Standard camera metadata: downstream OV13B10 ultra-wide has `sensor-position-roll = <90>`, `sensor-position-pitch = <0>`, and `sensor-position-yaw = <180>`; the current mainline mapping is `orientation = <1>` for rear-facing and `rotation = <90>` as the first rotation candidate.

Verified stream variables:

- Working media route: `ov13b10 21-0010` → `msm_csiphy1` → `msm_csid1` → `msm_vfe1_rdi0` → `/dev/video3` (`msm_vfe1_video0`).
- Working active format: `1364x768`, media bus code `0x300a` / `MEDIA_BUS_FMT_SGRBG10_1X10`, V4L2 fourcc `pgAA` / packed 10-bit GRBG Bayer.
- Working capture parameters: V4L2 mplane capture, MMAP buffers, stride `1712`, frame payload `1314816` bytes.

Open variables after first stream:

- Current lmi-verified V4L2 video node is raw RDI/Bayer only: `/dev/video3` exposes packed RAW10 (`pgAA`) frames from `msm_vfe1_rdi0`, not ISP-processed YUV/RGB.
- Image quality, color, exposure, white balance, and ISP/color-processing path are not solved by the raw RDI stream.
- Browser-like discovery still needs a standard userspace camera stack later; current kernel bring-up exposes a normal media/V4L2 raw sensor path and standard sensor metadata without modifying the rootfs.

## Test status

| Stage | Status | Notes |
| --- | --- | --- |
| DTS compiles | Passed locally | `build-kernel.sh` debug/release produced `sm8250-xiaomi-lmi.dtb`; `git diff --check` passed. |
| OV13B10 driver builds with DT support | Passed locally | debug/release builds include `CONFIG_VIDEO_OV13B10=y`. |
| OV13B10 binding validates | Passed locally | `dt_binding_check` for `ovti,ov13b10.yaml` passed. |
| Target dtbs_check | Tooling blocked | DTC compiled the target DTB, but installed `dt-validate` rejected the kernel `CHECK_DTBS=y` invocation. |
| CAMSS/CCI power domains | Passed after fix | First device boot #74 timed out because `CONFIG_SM_CAMCC_8250=m`; release #75 with `CONFIG_SM_CAMCC_8250=y` produced `/dev/media0` and `/dev/video0..13`. |
| CCI device probes | Passed on device | Working path is `&cci0_i2c1`; OV13B10 now probes as `ov13b10 21-0010`. |
| OV13B10 I2C responds | Passed on device | Temporary powered address scan found `0x560d42` at 7-bit `0x10`; the older `0x36` candidate returned CCI NACK / `-ENXIO`. |
| Chip ID read succeeds | Passed on device | Probe at `21-0010` succeeds with expected chip ID `0x560d42`. |
| Media graph appears | Passed on device | CAMSS media/video nodes appear, OV13B10 is `/dev/v4l-subdev24`, and the working route is through `msm_csiphy1` → `msm_csid1` → `msm_vfe1_rdi0` → `/dev/video3`. |
| Stream test | Passed on device | Direct Python ioctl validation set ACTIVE subdev formats and captured one `pgAA` frame from `/dev/video3` without installing `media-ctl`/`v4l2-ctl`. |
| Web preview script | Passed on device | `lmi/scripts/lmi-camera-web-preview.py` now configures the media route and V4L2 capture path directly; initial BMP polling worked, then the helper was changed to reset controls, emit color PNG previews with preview-side AWB, smooth AE updates, and serve a persistent `/stream` multipart path for lower-latency browser testing. |
| Standard sensor metadata | Passed on device | Boot #86 verified `G/S_FRAME_INTERVAL`, crop selection targets, rear orientation, sensor rotation, and raw RDI stream continuity through direct ioctl validation. |
| OV13B10 active subdev state/events | Passed on device | Boot #87 exposed a `state_lock` self-deadlock; boot #88 after removing that lock binding verified metadata ioctl and raw stream continuity. |
| OV13B10 4-lane mode coverage | Passed on device | Boot #88 validated metadata and raw capture for all exposed 4-lane modes: 4208x3120, 4160x3120, 4160x2340, 2104x1560, 2080x1170, and 1364x768. |
| V4L2/media discovery summary | Passed on device | The helper's `--discover` path now reports media links, sensor modes/metadata, `/dev/video3` querycap/format enumeration, and the raw-RDI output boundary through direct ioctls. |
| Standard mode-index selection | Passed on device | `--mode-index` now selects width/height from `VIDIOC_SUBDEV_ENUM_FRAME_SIZE`; all six OV13B10 mode indexes were verified to configure matching `/dev/video3` `pgAA` formats. |
| V4L2 control discovery | Passed on device | `--controls` and `--discover` now enumerate 17 OV13B10 controls/ranges/defaults/current values, including exposure, H/V flip, orientation/rotation, VBLANK/HBLANK, analogue/digital gain, link frequency, pixel rate, unit cell size, and test pattern. |
| 64-bit control discovery | Passed on device | `VIDIOC_G_EXT_CTRLS` now reads integer64 controls; Pixel Rate reports current value `448000000` on the 1364x768 mode. |
| Extended control metadata | Passed on device | `VIDIOC_QUERY_EXT_CTRL` works for all 17 OV13B10 controls; Pixel Rate now reports 64-bit min/max/default/current metadata as `0..448000000`, default/current `448000000`. |
| Unit cell size metadata | Passed on device | Boot #89 exposes `V4L2_CID_UNIT_CELL_SIZE` as a read-only compound `area` control; `--controls` and `--discover` read `{ width = 1120, height = 1120 }` through `VIDIOC_G_EXT_CTRLS`. |
| Video node frame-size discovery | Passed on device | `/dev/video3` reports continuous `pgAA` frame sizes from 1x1 to 8191x8191; frame intervals are not implemented on the CAMSS video node and must come from the sensor subdev metadata. |
| CAMSS raw-only format discovery | Passed on device | Boot #92 keeps CAMSS booting and makes `/dev/video3` unfiltered `ENUM_FMT`, filtered `ENUM_FMT`, and `TRY_FMT` converge to the active upstream RAW10 `pgAA` format once the media route is linked. |

## Pitfalls log

- Do not enable the pop-up front camera in the first camera patch. It needs separate lift/position handling.
- Do not copy Android downstream `qcom,cam-sensor` nodes directly into mainline DTS. Mainline needs V4L2 sensor nodes and endpoint graph bindings.
- Treat CCI master mapping as a test variable. Android `cci-master = <1>` may not mean mainline `&cci1`.
- Treat I2C address as a test variable until confirmed from real probe logs; OV13B10 looked plausible at `0x36` from early evidence, but the working lmi address is `0x10`.
- Do not add vendor blobs, stock DTB contents, or extracted firmware files to the kernel repository.
- Keep the first iteration limited to one sensor; adding multiple inactive camera nodes makes probe failures harder to isolate.
- When CAMSS, CCI, and sensor drivers are built-in, `CONFIG_SM_CAMCC_8250` must also be built-in; leaving CAMCC as a module caused CAMSS/CCI power-domain deferred probe timeout.
- Treat `redmi_k30_pro_camera_specs.md` as a lens/spec reference and keep kernel sensor identity grounded in stock firmware strings, DTS, and real probe logs.
- Do not modify the rootfs to install camera test tools unless explicitly requested; missing `media-ctl`/`v4l2-ctl` is a test-environment limitation, not a reason to change rootfs during kernel bring-up.
- Adapter numbers alone are not stable evidence for CCI controller identity; check the runtime OF node path when comparing `cci0` and `cci1` tests.
- The visible probe error `-5` hides the more useful lower-level result; temporary diagnostics showed raw CCI NACK `-6` at address `0x36`.
- A powered in-kernel CCI address scan can be useful for one-off bring-up, but it must be removed after finding the real sensor address.
- For direct V4L2 subdev ioctl tests, `VIDIOC_SUBDEV_S_FMT` must use `V4L2_SUBDEV_FORMAT_ACTIVE`; using TRY formats leaves CAMSS active pads mismatched and `STREAMON` fails with `-EPIPE`.
- Python ioctl helpers must match 64-bit UAPI struct layout exactly; `struct v4l2_format` has the mplane union aligned at offset 8, and packed mplane structs need explicit ctypes packing/layout.
- V4L2 exposure/gain/VBLANK controls can retain values across temporary preview-script runs; reset them to driver defaults before starting AE tests, otherwise a stale max-gain/max-VBLANK state can make the next run look broken.
- Long VBLANK is effectively long exposure and lowers video frame rate; browser preview should default to a video-priority AE mode and only use long exposure in explicit low-light tests.
- Repeated `/frame.bmp` polling is useful for proof-of-life but not for judging video smoothness; use a persistent multipart stream path for browser preview testing.
- Standard `orientation`/`rotation` should be kept in the sensor DT node so V4L2/libcamera-style stacks can expose camera location and mounting information; downstream `sensor-position-*` values still need visual confirmation against a correctly colored preview.
- On SM8250 mainline CAMSS, the current lmi VFE 480 memory path is still RDI/MIPI RAW: `camss-vfe-480.c` programs the write master as `MODE_MIPI_RAW`, and `vfe_formats_pix_845` is still a TODO alias to RDI formats. Do not describe `/dev/video3` as browser-ready YUV/ISP output.
- Do not bind `sd.state_lock` to the same mutex that the OV13B10 pad/control callbacks take; the subdev ioctl path can hold the state lock before calling `.set_fmt`, causing a self-deadlock in `ov13b10_set_pad_format`.
- One #88 rapid mode loop transiently hit a CCI/start-stream failure (`i2c-qcom-cci ... queue 0 timeout`), but isolated retests and a later two-cycle rapid stress run passed; do not add arbitrary sensor delay unless this becomes reproducible.
- For future preview/web-camera code, prefer selecting OV13B10 modes through standard frame-size enumeration (`--mode-index`) and then applying video `S_FMT`, rather than carrying hard-coded width/height tables in userspace.
- `VIDIOC_QUERYCTRL` works on the OV13B10 subdev for control discovery; `VIDIOC_QUERYMENU` can return `ENOTTY` on this subdev path, so menu labels are optional and should not block exposing control IDs/ranges/defaults/current values.
- 64-bit V4L2 controls such as Pixel Rate require `VIDIOC_G_EXT_CTRLS`; `struct v4l2_ext_control` is packed, so ctypes helpers must keep the value union at offset 12 instead of the natural 8-byte-aligned offset 16.
- Prefer `VIDIOC_QUERY_EXT_CTRL` over legacy `VIDIOC_QUERYCTRL` when building manual-control UI metadata; the legacy query path can show incomplete integer64 ranges/defaults, while the extended path reports Pixel Rate max/default/current correctly.
- Compound V4L2 controls such as `V4L2_CID_UNIT_CELL_SIZE` require `VIDIOC_QUERY_EXT_CTRL` enumeration with `V4L2_CTRL_FLAG_NEXT_COMPOUND` and `VIDIOC_G_EXT_CTRLS` with a payload pointer; legacy `QUERYCTRL`/plain `G_CTRL` is not enough for area payloads.
- CAMSS `/dev/video3` implements generic continuous `VIDIOC_ENUM_FRAMESIZES` for `pgAA`, not the sensor's discrete OV13B10 mode table; use the sensor subdev enumeration as the authoritative mode list and video `S_FMT` as the capture-node negotiation step.
- Do not query the remote media pad from `__video_try_fmt()` during CAMSS video-device registration. `msm_video_init_format()` runs before userspace links the media graph, and an early `media_pad_remote_pad_first()` lookup caused the #90 NULL pointer crash.

## Test log

### 2026-05-26 — Initial planning

- Decision: first bring-up target is rear ultra-wide OV13B10.
- Reason: target tree already has `drivers/media/i2c/ov13b10.c`; this avoids writing an IMX686 driver before CAMSS/CCI/CSIPHY basics are validated.
- Current state: no implementation tested yet.

### 2026-05-26 — First kernel wiring

- Added initial OV13B10 DT binding, DT-capable driver path, lmi DTS wiring, and camera config fragments.
- Local debug and release builds both produced `Image.gz` and `sm8250-xiaomi-lmi.dtb`.
- `dt_binding_check` passed for `Documentation/devicetree/bindings/media/i2c/ovti,ov13b10.yaml`.
- Target DTB compiles with DTC; full target schema validation is blocked by the local `dt-validate` invocation mismatch.
- Real-device probe, chip ID, media graph, and stream tests are still pending.

### 2026-05-26 — First real-device camera boot (#74)

- Flashed the release copydown boot image built from the first camera wiring.
- Kernel booted as `7.1.0-rc5-lmi-release+ #74`.
- CAMSS and CCI did not probe: `qcom-camss ac6a000.camss` and `i2c-qcom-cci ac4f000.cci` timed out while configuring power domains.
- No `/dev/media*`, `/dev/v4l-subdev*`, or `/dev/video*` nodes appeared.
- Root cause was the release config keeping `CONFIG_SM_CAMCC_8250=m` while CAMSS/CCI were built-in and needed CAMCC GDSCs during early probe.

### 2026-05-26 — CAMCC built-in fix and second device boot (#75)

- Added `CONFIG_SM_CAMCC_8250=y` to both `lmi/configs/m1.config` and `lmi/configs/m1-release.config`.
- Rebuilt and reflashed release boot; kernel booted as `7.1.0-rc5-lmi-release+ #75`.
- CAMSS/CCI power-domain issue was resolved far enough for `/dev/media0` and `/dev/video0..13` to appear.
- OV13B10 instantiated as `ov13b10 21-0036`, but chip ID read failed with `failed to find sensor: -5` / `probe ... error -5`.
- Current blocker is sensor ACK/chip-ID, likely one of CCI mapping, reset timing/polarity, power sequence, MCLK, or a sensor-identity mismatch.
- `media-ctl` and `v4l2-ctl` were not present on the device rootfs, so media graph printing and the web preview script are blocked until the sensor probe path works and tools are available without violating the rootfs boundary.

### 2026-05-26 — User lens spec table reconciliation

- Added `lmi/redmi_k30_pro_camera_specs.md` as a reference input.
- The updated table lists ultra-wide as OmniVision OV13B10, matching stock/vendor evidence for the lmi ultra-wide path.
- Exact vendor-image string search found `com.qti.sensor.ov13b10_lmi`, `com.qti.sensormodule.lmi_sunny_ov13b10_ultra`, and `com.qti.tuned.lmi_sunny_ov13b10*`; exact S5K3L6 lmi strings did not appear in the checked vendor image.
- Decision for the next test: keep OV13B10 as the first kernel target and continue debugging the current `21-0036` chip-ID failure through reset, power, MCLK, and CCI details.

### 2026-05-26 — Reset initial-state test (#76)

- Found that the OV13B10 reset GPIO is described as `GPIO_ACTIVE_LOW`, but the driver requested it with `GPIOD_OUT_LOW`, which releases XSHUTDN before the sensor power sequence runs.
- Changed the reset GPIO request to `GPIOD_OUT_HIGH` so the sensor stays in reset until `ov13b10_power_on()` releases it after MCLK and regulators are enabled.
- Release build passed and a new copydown boot image was packaged: `boot-linux-copydown-lmi.img` SHA-256 `a70f508942fa102041c59a22a495276abf302fc24e08ed26674b9b4c283d7914`.
- Flashed and booted as `7.1.0-rc5-lmi-release+ #76`.
- Result: CAMSS media/video nodes still appear, but OV13B10 still fails at `ov13b10 21-0036: failed to find sensor: -5`.
- Conclusion: keeping reset asserted before power sequencing is the correct driver state cleanup, but it is not sufficient for chip-ID; next camera variables are power sequencing/load, MCLK/reset delay, and possibly CCI mapping.

### 2026-05-26 — CCI mapping and reset polarity tests (#78/#79)

- Re-tested the downstream-matching `cci0_i2c1` path and still got `ov13b10 21-0036: failed to find sensor: -5`.
- Tried a downstream-like power order of VIO → VDIG → MCLK → VANA → reset release; the sensor still did not ACK.
- Tried reset polarity as active-high; the probe still failed, so the DTS was restored to the downstream-matching `GPIO_ACTIVE_LOW` candidate.
- Tried `cci0_i2c0`; the client moved to `ov13b10 20-0036`, but chip-ID read still failed.
- Tried `cci1_i2c1`; the runtime DT placed the sensor under `cci@ac50000/i2c-bus@1`, but chip-ID read still failed.
- Tried `cci1_i2c0`; the runtime DT placed the sensor under `cci@ac50000/i2c-bus@0`, but chip-ID read still failed.
- Conclusion: the common CCI controller/bus candidates and reset polarity alone do not explain the NACK.

### 2026-05-26 — Raw CCI NACK diagnostic (#80)

- Restored the DTS to the downstream-evidence candidate: `cci0_i2c1`, address `0x36`, MCLK2/GPIO96, reset GPIO91 active-low, VANA GPIO63, PM8009 L7/L3, and CSIPHY1.
- Added temporary driver diagnostics around power-on and chip-ID read.
- Device log showed the expected visible power state before the read:
  - `power on clk 19200000 avdd 2800000 dovdd 1800000 dvdd 1200000 reset 0`
- The chip-ID read still failed at `0x300a`, but the raw CCI result was more specific:
  - `read reg 0x300a len 3 adapter Qualcomm-CCI addr 0x36`
  - `read reg 0x300a i2c_transfer ret -6`
- `-6` is `-ENXIO`; in `i2c-qcom-cci.c`, CCI NACK is reported as `-ENXIO`. The higher-level `failed to find sensor: -5` is only the OV13B10 driver converting the failed two-message read to `-EIO`.
- Current conclusion: at the downstream-evidence address and bus, the sensor is powered enough for regulators/MCLK/reset to look correct from Linux, but it still does not ACK. Next variables are exact vendor power-table behavior, regulator load/current/mode, downstream `pwm-switch`, or an additional shared camera enable.

### 2026-05-26 — Downstream rail load test (#81)

- Compared downstream camera regulator handling with the current mainline path.
- Downstream calls `regulator_set_load()` before enabling each controlled camera rail, using OV13B10 ultra loads from DTS: `cam_vio = 120000`, `cam_vdig = 1056000`, `cam_vana = 80000`, `cam_clk = 0`.
- Added a kernel-only test to vote matching loads for OV13B10 `dovdd`, `dvdd`, and `avdd`, and enabled `regulator-allow-set-load` plus LPM/HPM allowed modes for PM8009 L3/L7.
- Release kernel built, copydown boot image packaged locally, manifest had `boot_size_ok=True`, and the boot image was flashed with Windows fastboot.
- Device booted and still logged:
  - `power on clk 19200000 avdd 2800000 dovdd 1800000 dvdd 1200000 reset 0`
  - `read reg 0x300a len 3 adapter Qualcomm-CCI addr 0x36`
  - `read reg 0x300a i2c_transfer ret -6`
- `/dev/media0` and `/dev/video0..13` still appear; `/dev/v4l-subdev*` remains absent because the sensor does not probe.
- Conclusion: PM8009/VANA rail load voting alone does not fix the OV13B10 ACK. Next useful kernel-side diagnostic is to confirm whether any I2C address ACKs under the same power state or whether a missing GPIO/shared enable is still holding the module off.

### 2026-05-26 — I2C address scan and first sensor probe success (#82/#83)

- Checked downstream shared pinctrl and BOB/PWM handling before changing more board wiring:
  - lmi `qcom,cam-res-mgr` has no `shared-gpios`, so shared pinctrl is not an extra OV13B10 enable.
  - OV13B10 reset/MCLK pinctrl already matches downstream GPIO91/GPIO96.
  - Downstream `pwm-switch` only calls `regulator_set_load()` for a regulator named `cam_bob`; the lmi OV13B10 node uses `cam_vio`, `cam_vdig`, `cam_clk`, and `cam_vana`, so this is not an additional BOB switch for this sensor.
- Added a temporary powered CCI address scan after the `0x36` chip-ID read failed.
- Scan result on `&cci0_i2c1`:
  - `0x10` ACKed and returned chip ID register value `0x560d42`, matching OV13B10.
  - `0x50` and `0x54` also ACKed but returned `0xffffff`; these are not the OV13B10 chip-ID path.
- Changed the lmi DTS sensor node from `camera@36` / `reg = <0x36>` to `camera@10` / `reg = <0x10>`.
- Release DTB compiled, copydown boot image packaged locally, manifest had `boot_size_ok=True`, and the boot image was flashed with Windows fastboot.
- Device booted as `7.1.0-rc5-lmi-release+`; OV13B10 now probes successfully as `ov13b10 21-0010`.
- `/dev/media0`, `/dev/video0..13`, and `/dev/v4l-subdev0..24` appear; `/sys/class/video4linux/v4l-subdev24/name` is `ov13b10 21-0010`.
- `media-ctl` and `v4l2-ctl` are still not present on the rootfs, so graph printing and stream/web-preview tests remain blocked by userspace tooling under the current no-rootfs-modification rule.
- After confirming the address, removed the temporary address-scan diagnostics and noisy probe logs from `ov13b10.c`.

### 2026-05-26 — Clean diagnostic-free boot verification

- Rebuilt and packaged the release copydown boot image after removing temporary OV13B10 address-scan and noisy read/power diagnostics.
- Flashed the cleaned boot image directly with Windows fastboot; boot image SHA-256 was `4865d3bd3f14f5b78eba13edbc9ebb51efd1a88b86eeff9b90ca75a5b4a0cb5f` and manifest still had `boot_size_ok=True`.
- Device booted as `7.1.0-rc5-lmi-release+`.
- OV13B10 still appears as `/sys/class/video4linux/v4l-subdev24/name = ov13b10 21-0010`.
- `/dev/media0`, `/dev/v4l-subdev0..24`, and `/dev/video0..13` are present on the cleaned build.
- No rootfs changes were made; media graph printing and stream/web-preview tests remain blocked by missing `media-ctl`/`v4l2-ctl` under the current kernel-only bring-up boundary.

### 2026-05-26 — OV13B10 route, stream, and web preview validation

- Kept the rootfs unchanged: no `media-ctl`, `v4l2-ctl`, libcamera, PipeWire, or browser package was installed. Validation used direct Python ioctl helpers over SSH and only temporary files under `/tmp`.
- Verified the media route: `ov13b10 21-0010:pad0` → `msm_csiphy1:pad0`, `msm_csiphy1:pad1` → `msm_csid1:pad0`, `msm_csid1:pad1` → `msm_vfe1_rdi0:pad0`, `msm_vfe1_rdi0:pad1` → `msm_vfe1_video0:pad0`.
- Set ACTIVE subdev formats to `1364x768` / `0x300a` on the sensor, CSIPHY1, CSID1, and VFE1 RDI0 pads.
- Configured `/dev/video3` as V4L2 mplane capture with fourcc `pgAA`; the driver returned stride `1712`, sizeimage `1314816`, and one plane.
- `STREAMON` succeeded and one frame was captured: sequence `0`, bytes used `1314816`.
- Reworked `lmi/scripts/lmi-camera-web-preview.py` to configure the media graph and capture path directly through Python ioctl instead of shelling out to missing rootfs tools.
- `python3 lmi-camera-web-preview.py --once --output /tmp/lmi-camera-test.bmp --media-print` wrote a valid BMP preview from the raw Bayer frame: preview `682x384`, raw format detected as `bayer10p`.
- A temporary HTTP server on `127.0.0.1:18080` returned `/status` with `state: streaming`, `video: /dev/video3`, `pixelformat: pgAA`, and `/frame.bmp` returned a valid BMP payload (`786486` bytes, `BM` header).
- After the successful preview test, the checked camera dmesg only contained the two earlier `Failed to start media pipeline: -32` entries from pre-fix TRY-format experiments; no new CAMSS error was logged by the final HTTP preview path.

### 2026-05-26 — Web preview color, stream, and AE cleanup

- User testing found the initial browser preview too dark, black-and-white, and choppy.
- Confirmed that the first web helper was only a raw Bayer grayscale BMP preview and used repeated `/frame.bmp` HTTP polling, so it was not a real video-style stream and did not do demosaic/color processing.
- Added direct V4L2 control access for `V4L2_CID_EXPOSURE`, `V4L2_CID_VBLANK`, `V4L2_CID_ANALOGUE_GAIN`, and `V4L2_CID_DIGITAL_GAIN` through the sensor subdev.
- Found that controls retain state across temporary preview runs; the helper now resets controls to driver defaults before auto-exposure unless explicitly told to preserve them.
- Split AE behavior into modes: default `video` keeps the 1364x768 mode at its default VBLANK (`32`, 120fps timing) and raises exposure/gain without extending frame length; `balanced` and `low-light` can allow longer VBLANK for darker scenes.
- Added a persistent `/stream` multipart path so the browser keeps one connection open instead of polling `/frame.bmp` for every frame.
- Added a pure-Python GRBG 2x2 raw Bayer color preview path and PNG output. The default web preview is now color PNG, not grayscale BMP.
- Device validation without rootfs changes:
  - `--once --auto-exposure --ae-mode video --output /tmp/lmi-camera-color-video.png` produced a valid PNG (`image/png`, PNG signature `89504e470d0a1a0a`, size about 97 KiB) with controls reset to `vblank=32`, `exposure=792`, `analogue_gain=128`, `digital_gain=2560` before the first AE update.
  - A 682x384 color PNG stream was CPU-bound in Python at about 5 fps.
  - Lowering preview output to the default 480-width mode, which maps this sensor mode to `341x192`, improved the temporary Python stream test to about 18 fps over `/stream` while keeping sensor VBLANK at `32`.
- Current limitation: this is still bring-up tooling and not a production camera stack. It provides simple 2x2 GRBG color preview and crude luma-driven AE only; it does not implement proper ISP demosaic, color correction, AE/AWB, libcamera pipeline handling, encoded video, or browser `getUserMedia` discovery.

### 2026-05-26 — Standard camera orientation metadata

- Checked the lmi downstream OV13B10 ultra-wide node in `lmi-sm8250-camera-sensor-mtp.dtsi`: `csiphy-sd-index = <1>`, `cci-master = <1>`, `sensor-position-roll = <90>`, `sensor-position-pitch = <0>`, and `sensor-position-yaw = <180>`.
- Added standard mainline sensor metadata to the OV13B10 DTS node: `orientation = <1>` for rear-facing and `rotation = <90>` as the first rotation candidate.
- Local validation: `git diff --check` passed, and `make ... qcom/sm8250-xiaomi-lmi.dtb` regenerated the target DTB successfully.
- Remaining validation: once the color/3A path is less primitive, visually verify the preview orientation and update `rotation` if the downstream roll mapping does not match the mainline V4L2 rotation convention.

### 2026-05-26 — Preview helper AE/AWB refinement

- Smoothed the temporary helper AE loop by limiting each control update with `--ae-max-step` instead of jumping exposure/gain by large fixed ratios.
- Lowered the default preview capture interval from 100 ms to 30 ms so browser streaming is not artificially capped at 10 fps when Python processing is faster.
- Added preview-side gray-world white balance enabled by default for color PNG output, plus manual `--red-gain`, `--green-gain`, and `--blue-gain` overrides for bring-up testing.
- Fixed `--once --auto-exposure` validation semantics by discarding warmup frames before saving the output image, so the saved file reflects AE updates instead of the reset/default first frame.
- Local validation: `python3 -m py_compile lmi/scripts/lmi-camera-web-preview.py` passed. The service was not restarted for this edit.

### 2026-05-26 — OV13B10 frame interval enumeration

- Added `enum_frame_interval` support to `drivers/media/i2c/ov13b10.c` so standard V4L2 subdev userspace can enumerate the default frame interval for each advertised sensor mode instead of only seeing frame sizes.
- The interval is derived from the selected mode timing: `mode->ppl * mode->vts_def / pixel_rate`, with pixel rate computed from the link frequency and active CSI-2 data lane count.
- Wired the new callback into `ov13b10_pad_ops` next to `enum_frame_size`.
- Local validation: `git diff --check` passed, `make ... ARCH=arm64 LLVM=1 drivers/media/i2c/ov13b10.o` compiled the OV13B10 driver object, and the release `lmi/scripts/build-kernel.sh` flow generated `Image.gz` plus `qcom/sm8250-xiaomi-lmi.dtb`. The first object-only command printed existing oldconfig prompts for unrelated new Kconfig symbols in the reused output directory before compiling the object.
- Device validation on boot #86: `ENUM_FRAME_INTERVAL` and `G_FRAME_INTERVAL` for the 1364x768 mode both reported `3731200 / 448000000` seconds, about `120.069 fps`, with VBLANK still visible as a normal V4L2 control.

### 2026-05-26 — OV13B10 crop selection metadata

- Added per-mode crop rectangles to the OV13B10 mode table and exposed standard V4L2 subdev selection metadata through `get_selection`.
- The sensor now reports `V4L2_SEL_TGT_NATIVE_SIZE`, `V4L2_SEL_TGT_CROP_BOUNDS`, `V4L2_SEL_TGT_CROP_DEFAULT`, and mode-dependent `V4L2_SEL_TGT_CROP`, so camera stacks can infer pixel-array size, aspect-ratio crops, and binned preview modes instead of only seeing output frame sizes.
- Local validation: `git diff --check` passed, `make ... ARCH=arm64 LLVM=1 drivers/media/i2c/ov13b10.o` compiled the driver object, and the release `lmi/scripts/build-kernel.sh` flow generated `Image.gz` plus `qcom/sm8250-xiaomi-lmi.dtb`.
- Device validation on boot #86: direct subdev selection ioctls reported active crop `{ left = 740, top = 792, width = 2728, height = 1536 }` and native/default/bounds `{ left = 0, top = 0, width = 4208, height = 3120 }`.

### 2026-05-26 — OV13B10 frame interval and CSI-2 metadata

- Added `get_frame_interval` and `set_frame_interval` support so standard V4L2 subdev userspace can read the active frame timing and request a target interval through the existing VBLANK control.
- Added `get_frame_desc` with a single CSI-2 stream entry advertising virtual channel 0 and RAW10 data type for `MEDIA_BUS_FMT_SGRBG10_1X10`.
- Added `get_mbus_config` and stored the parsed DT CSI-2 endpoint config, so bridge drivers can query the active D-PHY lane/link-frequency metadata through the sensor subdev.
- Local validation: `git diff --check` passed and `make ... ARCH=arm64 LLVM=1 drivers/media/i2c/ov13b10.o` compiled the OV13B10 driver object after these metadata changes.
- Device validation on boot #86: `G_FRAME_INTERVAL`, `S_FRAME_INTERVAL` round-trip, and selection target queries passed; frame descriptor and mbus config coverage remain kernel-build plus bridge-path metadata because this UAPI does not expose them as normal subdev ioctls.

### 2026-05-26 — CAMSS raw-only boundary

- Reviewed the current SM8250 CAMSS path used by lmi: VFE 480 starts the write master with `MODE_MIPI_RAW`, and the SM8250 `vfe_formats_pix_845` table is still marked as a TODO alias of the RDI raw formats.
- Current `/dev/video3` is a standard V4L2 raw Bayer capture node for bring-up and future camera-stack integration, not a complete browser-ready camera node that emits ISP-processed YUV/RGB frames.
- The next kernel-side work should keep improving sensor/media metadata and investigate whether a usable standard pipeline can be described above the raw node; the temporary Python web preview remains validation tooling and should not be treated as the final camera stack.

### 2026-05-26 — Preview helper metadata query

- Added `--metadata` to `lmi/scripts/lmi-camera-web-preview.py` so the no-rootfs-change validation path can query userspace-visible OV13B10 frame interval, crop selection, orientation, and rotation metadata directly through subdev ioctls.
- Added optional `--metadata-set-interval` to round-trip `S_FRAME_INTERVAL` with the current interval, which verifies the new set callback without changing the requested timing.
- Added `--list-modes` to enumerate `VIDIOC_SUBDEV_ENUM_FRAME_SIZE` and `VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL` directly, so the helper can discover the kernel-exposed mode list instead of relying on hard-coded test notes.
- Local validation: `python3 -m py_compile lmi/scripts/lmi-camera-web-preview.py` and `git diff --check` passed.

### 2026-05-26 — OV13B10 metadata boot validation (#86)

- Built, packaged, flashed, and booted the release copydown image as `7.1.0-rc5-lmi-release+ #86 SMP PREEMPT Tue May 26 07:38:24 CST 2026`.
- OV13B10 still probed as `/dev/v4l-subdev24` with runtime name `ov13b10 21-0010`; device nodes were still present as 1 media node, 25 subdev nodes, and 14 video nodes.
- Direct metadata query through `/tmp/lmi-camera-web-preview.py --metadata --metadata-set-interval --media-print` passed without installing rootfs camera tools.
- `ENUM_FRAME_INTERVAL`, `G_FRAME_INTERVAL`, and `S_FRAME_INTERVAL` round-trip all reported `3731200 / 448000000` seconds for the 1364x768 RAW10 mode, about `120.069 fps`.
- Selection targets were visible from userspace: active crop `{ left = 740, top = 792, width = 2728, height = 1536 }`; crop bounds, crop default, and native size all `{ left = 0, top = 0, width = 4208, height = 3120 }`.
- Standard controls reported `camera_orientation = 1` and `camera_sensor_rotation = 90`, matching the current rear-facing DTS metadata candidate.
- Raw RDI stream continuity was rechecked with `--once --auto-exposure --ae-mode video`: `/dev/video3` still captured `pgAA` / `bayer10p`, stride `1712`, payload `1314816` bytes, sequence `8`, and wrote a valid PNG with signature `89504e470d0a1a0a`.
- Current boundary is unchanged: the validated kernel path is a standard V4L2/media raw Bayer path with better metadata, not yet a browser-ready ISP/YUV camera stack.

### 2026-05-26 — OV13B10 subdev state and CAMSS output audit

- OV13B10 already exposes the key sensor controls expected by standard camera stacks: link frequency, pixel rate, HBLANK, VBLANK, exposure, analogue gain, digital gain, test pattern, HFLIP/VFLIP, and fwnode-derived orientation/rotation.
- Added standard subdev active-state initialization with `init_state`, `v4l2_subdev_init_finalize()`, and `v4l2_subdev_cleanup()` so the default pad format is initialized through the modern V4L2 subdev state path as well as the existing file-handle TRY path.
- Added V4L2 control event subscribe/unsubscribe core ops; `v4l2_subdev_init_finalize()` now marks the sensor subdev as event-capable when controls are present.
- Local validation: `git diff --check` passed, and `make ... ARCH=arm64 LLVM=1 drivers/media/i2c/ov13b10.o` compiled the updated OV13B10 driver object.
- CAMSS audit result is still a hard boundary: `sm8250_resources` uses `vfe_res_8250`, the active VFE hardware ops are `vfe_ops_480`, `camss-vfe-480.c` starts the write master as `MODE_MIPI_RAW`, and `vfe_formats_pix_845` is still a TODO alias of the RDI raw formats.
- Conclusion: the next browser-ready milestone cannot be obtained by just flipping a CAMSS format table to YUV; SM8250 mainline still needs a real processed/ISP path or a standard userspace camera stack above the raw V4L2/media pipeline.

### 2026-05-26 — OV13B10 active-state deadlock and fix (#87/#88)

- Built, packaged, flashed, and booted the active-state test image as `7.1.0-rc5-lmi-release+ #87 SMP PREEMPT Tue May 26 08:12:16 CST 2026`; OV13B10 still probed as `ov13b10 21-0010`, with 1 media node, 25 subdev nodes, and 14 video nodes.
- The direct metadata query `/tmp/lmi-camera-web-preview.py --metadata --metadata-set-interval --media-print` hung after enabling the OV13B10 → CSIPHY1 link; the Python process entered `D` state with `wchan=ov13b10_set_pad_format` and a stack through `call_set_fmt_state` / `subdev_do_ioctl_lock`.
- Root cause: `sd.state_lock` had been pointed at `ov13b->ctrl_handler.lock`, which is the same mutex used inside `ov13b10_set_pad_format`; the V4L2 subdev ioctl framework took the state lock before calling `.set_fmt`, and the driver then tried to lock the same mutex again.
- Removed the `sd.state_lock = ov13b->ctrl_handler.lock` assignment while keeping `init_state`, `v4l2_subdev_init_finalize()`, cleanup, and control-event ops.
- Rebuilt, packaged, flashed, and booted the deadlock-fix image as `7.1.0-rc5-lmi-release+ #88 SMP PREEMPT Tue May 26 08:19:24 CST 2026`; manifest boot image SHA-256 was `834a337d3ff446eceab3e1631afd7346b11209bcb1709c22d347fde33a25dd3a` and `boot_size_ok=True`.
- On #88 the same metadata command completed without timeout: frame interval enum/current/set all reported `3731200 / 448000000` seconds, about `120.069 fps`; crop, crop bounds/default/native size, rear orientation `1`, and sensor rotation `90` were still visible.
- Raw stream continuity also passed on #88: `/dev/video3` captured `pgAA` / `bayer10p`, stride `1712`, payload `1314816` bytes, sequence `8`, and wrote `/tmp/lmi-camera-active-state-test.png` with PNG signature `89504e470d0a1a0a` and size `58010` bytes.
- A post-test process check found no stuck `python3 /tmp/lmi-camera-web-preview.py` process. Validation again used only temporary files under `/tmp` and did not install or modify rootfs camera tooling.

### 2026-05-26 — OV13B10 exposed mode coverage (#88)

- The target driver already contains plain-text OV13B10 mode/register tables for six 4-lane modes; the earlier web-preview default only exercised the smallest 1364x768 mode.
- On boot #88, direct ioctl metadata validation passed for all six exposed 4-lane modes, without installing `media-ctl`, `v4l2-ctl`, libcamera, PipeWire, or browser packages.
- The helper's `--list-modes` path returned all six modes through standard subdev frame-size/frame-interval enumeration: 4208x3120 and 4160x3120/2340 at about `29.799 fps`, 2104x1560 and 2080x1170 at about `59.598 fps`, and 1364x768 at about `120.069 fps`.
- Raw capture also passed for all six modes through the same OV13B10 → CSIPHY1 → CSID1 → VFE1 RDI0 → `/dev/video3` path:
  - `4208x3120`: `29.799 fps`, stride `5264`, payload `16423680`, PNG signature `89504e470d0a1a0a`.
  - `4160x3120`: `29.799 fps`, stride `5200`, payload `16224000`, PNG signature `89504e470d0a1a0a`.
  - `4160x2340`: `29.799 fps`, stride `5200`, payload `12168000`, PNG signature `89504e470d0a1a0a`.
  - `2104x1560`: `59.598 fps`, stride `2640`, payload `4118400`, PNG signature `89504e470d0a1a0a`.
  - `2080x1170`: `59.598 fps`, stride `2608`, payload `3051360`, PNG signature `89504e470d0a1a0a`.
  - `1364x768`: `120.069 fps`, stride `1712`, payload `1314816`, PNG signature `89504e470d0a1a0a`.
- A rapid sequential loop initially tripped the two 60 fps modes: `2104x1560` failed `STREAMON` after a CCI queue timeout during `ov13b10_start_streaming`, and `2080x1170` timed out waiting for a frame. Running those modes again individually after a short pause succeeded, and a later two-cycle rapid stress run across all six modes passed without reproducing the failure, so no arbitrary sensor delay was added.
- The full-resolution and binned OV13B10 modes are therefore kernel-visible and streamable, but the exported node is still raw Bayer RDI. Browser-ready YUV/RGB discovery still requires a real camera stack/ISP path above this raw kernel path.

### 2026-05-26 — V4L2/media discovery summary (#88)

- Added `--discover` to the pure ioctl helper so future web-camera code can inspect the current kernel interface without hard-coded assumptions: media route, `/dev/video*` and `/dev/v4l-subdev*` nodes, enabled links, sensor mode enumeration, sensor metadata, `VIDIOC_QUERYCAP`, current `VIDIOC_G_FMT`, full `VIDIOC_ENUM_FMT`, and `VIDIOC_ENUM_FMT` filtered by the active sensor media-bus code.
- Device validation on boot #88 passed with `/tmp/lmi-camera-web-preview.py --discover --media /dev/media0`; it reported the selected route as OV13B10 → `msm_csiphy1` → `msm_csid1` → `msm_vfe1_rdi0` → `/dev/video3`.
- `/dev/video3` reported `driver = qcom-camss`, `card = Qualcomm Camera Subsystem`, `bus_info = platform:ac6a000.camss`, and device capabilities including `video_capture_mplane`, `streaming`, `readwrite`, and `io_mc`.
- The current selected video format was `pgAA` at 1364x768 with one plane, stride `1712`, and `sizeimage = 1314816`, matching the previously validated raw stream path.
- Full `VIDIOC_ENUM_FMT` still lists generic pass-through formats such as UYVY/YUYV/GREY and Bayer variants from the CAMSS RDI format table, but `VIDIOC_ENUM_FMT` with `mbus_code = 0x300a` returned only `pgAA`. Future userspace should use media-controller-aware format enumeration rather than treating the unfiltered list as ISP output.
- This confirms the validated lmi route remains raw Bayer RDI until a real processed path or standard userspace pipeline is added.

### 2026-05-26 — Standard mode-index selection (#88)

- Added `--mode-index` to the pure ioctl helper so preview/discovery tests can select OV13B10 modes from `VIDIOC_SUBDEV_ENUM_FRAME_SIZE` results instead of hard-coding width and height.
- The selected mode is applied to the sensor, CSIPHY, CSID, VFE RDI subdev pads and then to `/dev/video3` with `VIDIOC_S_FMT`, and `--discover` reports both the selected sensor mode and configured video format.
- Device validation on boot #88 passed for all six mode indexes through `/tmp/lmi-camera-web-preview.py --discover --mode-index <n> --media /dev/media0`: indexes 0..5 mapped to 4208x3120, 4160x3120, 4160x2340, 2104x1560, 2080x1170, and 1364x768, each with matching `/dev/video3` `pgAA` width/height.
- A one-shot capture with `--once --mode-index 0 --auto-exposure --ae-mode video` produced a valid color PNG from the full 4208x3120 raw stream: stride `5264`, payload `16423680`, sequence `8`, and `image/png` output.
- Validation again used only temporary files under `/tmp`; the helper and test PNG were removed afterward, and no rootfs camera packages or persistent rootfs changes were made.

### 2026-05-26 — V4L2 control discovery (#88)

- Added `--controls` and extended `--discover` so the helper enumerates OV13B10 V4L2 controls via `VIDIOC_QUERYCTRL` / `V4L2_CTRL_FLAG_NEXT_CTRL`, including ranges, steps, defaults, flags, type names, and current values where plain `G_CTRL` applies.
- Device validation on boot #88 returned 16 controls from `/dev/v4l-subdev24`: Exposure, Horizontal Flip, Vertical Flip, Camera Orientation, Camera Sensor Rotation, Vertical Blanking, Horizontal Blanking, Analogue Gain, Link Frequency, Pixel Rate, Test Pattern, Digital Gain, plus the standard control-class headers.
- The low-resolution `--mode-index 5` control ranges were visible from userspace: exposure `4..792`, VBLANK `32..31999`, HBLANK fixed at `3300`, analogue gain `128..1984`, digital gain `1024..4095`, rotation fixed at `90`, and test pattern menu index range `0..4`.
- The subdev path returned `ENOTTY` for `VIDIOC_QUERYMENU`, so the helper treats menu labels as optional and still reports menu control IDs/ranges/current indexes; this keeps future manual-parameter UI generation from depending on rootfs camera tools.
- Validation again used only `/tmp/lmi-camera-web-preview.py`, removed it after testing, and did not install or modify rootfs camera packages.

### 2026-05-26 — Video-node frame-size discovery (#88)

- Added `VIDIOC_ENUM_FRAMESIZES` and `VIDIOC_ENUM_FRAMEINTERVALS` probing to the helper's `--discover` output so future userspace can distinguish sensor-subdev mode discovery from capture-node negotiation.
- Device validation on boot #88 with `--discover --mode-index 5` showed `/dev/video3` reports `pgAA` frame sizes as a continuous range from 1x1 to 8191x8191 with step 1.
- `/dev/video3` returned `ENOTTY` for `VIDIOC_ENUM_FRAMEINTERVALS`, so frame timing must continue to come from the OV13B10 subdev frame-interval metadata, not the CAMSS video node.
- The current configured capture format was still the selected 1364x768 `pgAA` one-plane format with stride `1712` and `sizeimage = 1314816`.
- This confirms the current standard-discovery model: enumerate discrete OV13B10 modes on `/dev/v4l-subdev24`, apply the selected size across the media route, then negotiate the raw capture format on `/dev/video3`.

### 2026-05-26 — 64-bit control discovery (#88)

- Added `VIDIOC_G_EXT_CTRLS` support to the pure ioctl helper so integer64 controls are no longer reported as unreadable by `--controls` and `--discover`.
- Fixed the ctypes UAPI layout for `struct v4l2_ext_control`; the kernel structure is packed, and the value union must sit at offset 12 with total size 20 bytes.
- Local validation passed: `python3 -m py_compile lmi/scripts/lmi-camera-web-preview.py`, a struct-layout check (`V4L2ExtControl` size 20/value offset 12, `V4L2ExtControls` size 32/pointer offset 24), and `git diff --check`.
- Device validation on boot #88 used only temporary `/tmp` files and no rootfs package changes: `--controls --mode-index 5` returned 16 controls and Pixel Rate current value `448000000` through `VIDIOC_G_EXT_CTRLS`.
- `--discover --mode-index 5` also reported Pixel Rate current value `448000000`, selected mode 1364x768, video node `/dev/video3`, and filtered raw format `pgAA`; the temporary helper and JSON files were removed afterward.

### 2026-05-26 — Extended control query metadata (#88)

- Added `VIDIOC_QUERY_EXT_CTRL` support to the pure ioctl helper and merged the extended metadata into `--controls` / `--discover` output, while keeping legacy `VIDIOC_QUERYCTRL` as a fallback.
- Local validation passed: `python3 -m py_compile lmi/scripts/lmi-camera-web-preview.py`; `V4L2QueryExtControl` size is 232 bytes with `minimum` at offset 40 and `reserved` at offset 104; `git diff --check` also passed.
- Device validation on boot #88 returned `controls_count=16` and `query_ext_count=16` for `/dev/v4l-subdev24`, so every enumerated OV13B10 control supports the extended query path.
- Pixel Rate now reports complete 64-bit metadata: minimum `0`, maximum `448000000`, step `1`, default `448000000`, current `448000000`, `elem_size = 8`, `elems = 1`, flags `read_only` and `has_which_min_max`.
- `--discover --mode-index 5` reported the same Pixel Rate metadata with selected mode 1364x768 and filtered raw format `pgAA`; validation used only `/tmp` files and removed them afterward.

### 2026-05-26 — Unit cell size metadata (#89)

- Added `V4L2_CID_UNIT_CELL_SIZE` to the OV13B10 driver as a read-only compound `V4L2_CTRL_TYPE_AREA` control using the lmi ultra-wide OV13B10 pixel size `1120nm x 1120nm`.
- Updated the pure ioctl helper to enumerate controls with `VIDIOC_QUERY_EXT_CTRL` plus `V4L2_CTRL_FLAG_NEXT_COMPOUND`, and to read area payload controls through `VIDIOC_G_EXT_CTRLS` with a `struct v4l2_area` buffer.
- Local validation passed: `python3 -m py_compile lmi/scripts/lmi-camera-web-preview.py`, ctypes layout checks for `V4L2Area` and packed `V4L2ExtControl`, `make ... ARCH=arm64 LLVM=1 drivers/media/i2c/ov13b10.o`, `git diff --check`, and the release `build-kernel.sh` flow.
- Built and flashed copydown boot `camera-unit-cell-size-2026-05-26-122025`; manifest SHA-256 for `boot-linux-copydown-lmi.img` was `cda1fc98c75c82f9aa8a2291c45ca3ae85bb9dc4bfdcc5bc2ce3b2823e2d1764`, and `boot_size_ok=True`.
- Device booted as `7.1.0-rc5-lmi-release+ #89 SMP PREEMPT Tue May 26 12:19:22 CST 2026`; `/dev/media0`, `/dev/v4l-subdev24`, and `/dev/video3` were present.
- `--controls --mode-index 5 --media /dev/media0` returned 17 controls and exposed `Unit Cell Size` as type `area`, `elem_size = 8`, `elems = 1`, flags `read_only` and `has_payload`, current `{ width = 1120, height = 1120 }`.
- `--discover --mode-index 5 --media /dev/media0` reported the same unit-cell-size control under `sensor.controls`, selected mode 1364x768 at about `120.069 fps`, configured `/dev/video3` as `pgAA` 1364x768 with stride `1712`, and still filtered the active media-bus format to raw `pgAA` only.
- Validation again used only temporary files under `/tmp`; the helper and JSON outputs were removed afterward, and no rootfs camera packages or persistent rootfs changes were made.

### 2026-05-26 — CAMSS raw-only format discovery (#90/#91/#92)

- Added `VIDIOC_TRY_FMT` probing to the pure ioctl helper's `--discover` output so it can show how `/dev/video3` negotiates each CAMSS-advertised format under the active OV13B10 RAW10 media route.
- Baseline on boot #89 showed the old CAMSS behavior accepted misleading `TRY_FMT` requests such as UYVY, VYUY, YUYV, and YVYU unchanged, even though the actual route is raw Bayer RDI.
- The first kernel attempt in boot #90 called `video_get_subdev_format()` from `__video_try_fmt()` unconditionally; the device hung during early boot, and recovery pstore showed a NULL pointer dereference at `media_pad_remote_pad_first()` from the CAMSS deferred-probe path before userspace had linked a media route.
- Fixed the #90 regression by only using the remote-subdev format convergence path after `video_is_registered(&video->vdev)` is true, keeping CAMSS registration-time format initialization on the old table-based fallback.
- Boot #91 validated that `TRY_FMT` requests for UYVY, VYUY, YUYV, YVYU, GREY, Bayer variants, and Y10/Y10P all converged to `pgAA` 1364x768 with stride `1712` and sizeimage `1314816` once the media route was active.
- Tightened `video_enum_fmt()` next: when userspace passes `mbus_code = 0` but `/dev/video3` is registered and linked to an active remote subdev, CAMSS now uses the upstream subdev media-bus code instead of listing the whole generic table.
- Local validation passed for the enum-format change: `make ... ARCH=arm64 LLVM=1 drivers/media/platform/qcom/camss/camss-video.o`, `git diff --check`, and the release `build-kernel.sh` flow.
- Built and flashed copydown boot `camera-camss-enumfmt-fix-2026-05-26-183911`; manifest SHA-256 for `boot-linux-copydown-lmi.img` was `b04fb7e7eed8c77d0ed73e2fb11a0994b4cc77996f9951f9bd9dbcba7362a8a2`, and `boot_size_ok=True`.
- Device booted as `7.1.0-rc5-lmi-release+ #92 SMP PREEMPT Tue May 26 18:38:29 CST 2026`; `/dev/media0`, `/dev/v4l-subdev24`, and `/dev/video3` were present, and dmesg no longer showed the #90 early CAMSS oops.
- On #92, `--discover --mode-index 5 --media /dev/media0` reported both unfiltered `formats_all = pgAA` and filtered `formats_for_mbus_code = pgAA`; `try_formats_from_all` only contained `pgAA` and still returned `pgAA` 1364x768 with stride `1712` and sizeimage `1314816`.
- Raw capture continuity still passed: `--once --mode-index 5 --reset-controls --auto-exposure --output /tmp/lmi-camera-test.png` captured `pgAA` / `bayer10p`, payload `1314816` bytes, sequence `8`, and wrote a valid PNG. Temporary helper, JSON, and PNG files were removed afterward; no rootfs camera packages or persistent rootfs changes were made.

## Validation commands

Local build checks:

```sh
git diff --check
# build lmi dtbs / kernel through the existing lmi build flow
# run dtbs_check for qcom,sm8250-camss.yaml and ovti,ov13b10.yaml if schema tooling is available
```

On-device checks:

```sh
dmesg | grep -Ei 'camss|cci|ov13|camera|csiphy|csid|vfe'
ls /dev/media* /dev/v4l-subdev* /dev/video*
python3 lmi/scripts/lmi-camera-web-preview.py --list
python3 lmi/scripts/lmi-camera-web-preview.py --list-modes
python3 lmi/scripts/lmi-camera-web-preview.py --controls --mode-index 5 --media /dev/media0
python3 lmi/scripts/lmi-camera-web-preview.py --discover --mode-index 5 --media /dev/media0
python3 lmi/scripts/lmi-camera-web-preview.py --metadata --metadata-set-interval --mode-index 5 --media-print
```

Pure-ioctl raw preview helper for the current OV13B10 path:

```sh
python3 lmi/scripts/lmi-camera-web-preview.py \
  --once --mode-index 5 --auto-exposure --ae-mode video \
  --output /tmp/lmi-camera-test.png \
  --media /dev/media0 --media-print

python3 lmi/scripts/lmi-camera-web-preview.py \
  --media /dev/media0 --mode-index 5 --host 0.0.0.0 --port 8080 \
  --auto-exposure --ae-mode video
```

The helper is still bring-up tooling: it configures the lmi OV13B10 media route directly, can select modes from standard subdev enumeration with `--mode-index`, enumerates sensor controls with `--controls` including extended metadata through `VIDIOC_QUERY_EXT_CTRL`, integer64 values through `VIDIOC_G_EXT_CTRLS`, and compound area controls such as unit cell size, captures raw `pgAA` frames through V4L2 mplane ioctl, generates a simple 2x2 GRBG color PNG preview, serves a persistent browser stream, and can print a JSON discovery summary with `--discover` including sensor modes, sensor controls, video capabilities, filtered formats, video TRY_FMT probing, video frame-size probing, and raw-output boundary notes. It does not replace ISP/color processing, AE/AWB, photo/video encoding, or the future standard camera stack needed for browser `getUserMedia` style discovery.
