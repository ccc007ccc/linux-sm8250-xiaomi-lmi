# lmi camera bring-up notes

本文件保存 Redmi K30 Pro / POCO F2 Pro (`lmi`) 主线 Linux 摄像头适配摘要。历史逐轮测试日志已压缩为阶段结论，避免文档成为上下文炸弹。

## Scope

第一阶段目标：后置超广角 OV13B10。

暂不启用：

- 前摄 S5K3T2：升降摄像头，机械控制和传感器链路分开处理。
- 主摄 IMX686、长焦 OV08A10、微距 S5K5E9YX04、景深 GC02M1。
- EEPROM、actuator、OIS、flash LED。

当前边界：

- 已支持真实 V4L2/media RAW10 Bayer RDI capture。
- 未支持真实 ISP/YUV/RGB camera output。
- 未支持 libcamera/PipeWire/portal/browser `getUserMedia` 标准相机路径。
- Venus 硬件编码已验证 generated NV12 和 OV13B10 RAW 派生软件 NV12 -> H.264，但还没有真实 camera ISP/YUV -> Venus 编码链路。

## Reference inputs

- Android kernel camera DTS: `android_kernel_xiaomi_sm8250/arch/arm64/boot/dts/vendor/qcom/lmi-sm8250-camera-sensor-mtp.dtsi`
- Android kernel overlay: `android_kernel_xiaomi_sm8250/arch/arm64/boot/dts/vendor/qcom/lmi-sm8250-overlay.dts`
- Stock extracted artifacts: `out/`, especially `out/dtb`, `out/recovery_dtbo`, `out/lmi-firmware-v14/vendor.img`
- User-supplied lens/spec table: `lmi/redmi_k30_pro_camera_specs.md`
- Target mainline tree: `lmi/linux-sm8250-xiaomi-lmi`

## Stock userspace camera configuration findings

Read-only extraction of the V14.0.1.0.SJKMIXM stock `super.img` confirmed the relevant camera userspace pieces live in `vendor.img`; extracted blobs were kept only under the local ignored `.local/camera-stock/` scratch area and must not be committed.

Useful stock paths for OV13B10 context:

- `/vendor/lib64/camera/com.qti.sensor.ov13b10_lmi.so`
- `/vendor/lib64/camera/com.qti.eeprom.lmi_sunny_ov13b10_gt24p64.so`
- `/vendor/lib64/camera/com.qti.sensormodule.lmi_sunny_ov13b10_ultra.bin`
- `/vendor/lib64/camera/com.qti.tuned.lmi_sunny_ov13b10_ultra.bin`
- `/vendor/lib64/camera/com.qti.tuned.lmi_sunny_ov13b10_ultra_pro.bin`
- `/vendor/lib64/hw/camera.qcom.so`
- `/vendor/lib64/hw/com.qti.chi.override.so`
- `/vendor/etc/camera/camxoverridesettings.txt`
- `/vendor/etc/init/android.hardware.camera.provider@2.4-service_64.rc`

String-level inspection of the OV13B10 tuned blobs shows IFE/3A modules such as `linearization34_ife`, `demux13_ife`, `demosaic36_ife`, `demosaic37_ife`, `cc12_ife`, `cc13_ife`, `cst12_ife`, `gamma16_ife`, `dsx10_ife_video_full_dc4`, `ds4to1*`, `lsc*`, `pdpc*`, `abf*`, `AECParamExtension`, `AWBExtensionParam`, and `LightSensorAssistAEC`. This supports the current kernel conclusion: Android's working camera stack relies on Camera HAL/CHI/tuning userspace to generate IFE/CDM module programming, not on an obvious static VFE480 register table in the downstream kernel.

Read-only extraction of `/vendor/lib{,64}/camera/components/` found QTI stats/node libraries and Xiaomi/Arcsoft/FacePP/Vidhance post-processing nodes. Offline scanning of OV13B10 blobs, HAL/CHI libraries, and these component libraries for plausible CDM `REG_CONT` / `REG_RANDOM` command streams did not find a trustworthy embedded VFE480 register program. Apparent hits in component libraries mapped to ELF `.debug_info`, `.gnu.hash`, or unrelated `.rodata` resources rather than valid IFE command buffers.

Stock HAL strings do expose useful runtime dump hooks such as `RegDumpBufferManager`, `ParseFlushRegDump`, `CmdBufferCDMProgram`, `CmdBufferIQSettings`, `register-dump`, `org.quic.camera.debugdata`, `org.quic.camera2.iqsettings`, and `R%lluIFE_RegDump_*.txt`; stock `camxoverridesettings.txt` keeps `enable3ADebugData` / `enableTuningMetadata` disabled by default. Downstream shows these register dumps are user packet descriptors (`CAM_ISP_PACKET_META_REG_DUMP_*`) that read back configured registers through `cam_soc_util_reg_dump_to_cmd_buf()`. They can help capture Android's runtime IFE state, but they are not a static initialization table that can be copied into mainline.

RegDump trigger conclusion from offline analysis: downstream `camera_ife/per_req_reg_dump` only consumes reg-dump descriptors already supplied by HAL packets after CDM completion; `camera_ife/enable_req_dump` only requests dump-on-error plumbing; and `camera_ife/ife_camif_debug` prints downstream CAMIF register ranges only on CAMIF overflow/violation. The only stock text-config toggles found for this path are `enable3ADebugData` and `enableTuningMetadata`, which may make Android HAL package debug/tuning metadata, but offline strings do not prove a no-rootfs, one-shot property/debugfs command that always creates IFE RegDump descriptors. A useful Android-side experiment would therefore have to be temporary and clearly separated from the mainline target: enable those HAL settings through a non-persistent overlay or stock test environment, run a camera request, check for `R%lluIFE_RegDump_*.txt` / `DebugDataAll` output, then discard the modified environment.

Boundary: these stock files are proprietary references only. Do not copy blob contents into the kernel, do not require them in the target rootfs, and do not treat their presence as mainline V4L2/YUV support.

## Camera inventory

| Role | Module hint | Evidence | Current status |
| --- | --- | --- | --- |
| Rear wide | Sunny IMX686 | `com.qti.sensor.imx686_lmi.so`, `lmi_sunny_imx686_wide` | Deferred |
| Rear ultra-wide | Sunny OV13B10 | `com.qti.sensor.ov13b10_lmi.so`, `lmi_sunny_ov13b10_ultra` | First target, RAW working |
| Rear tele | Sunny OV08A10 | `com.qti.sensor.ov08a10_lmi.so`, `lmi_sunny_ov08a10_tele` | Deferred |
| Rear macro | Sunny S5K5E9YX04 | `com.qti.sensor.s5k5e9yx04_lmi.so`, `lmi_sunny_s5k5e9yx04_macro` | Deferred |
| Rear depth | OFilm GC02M1 | `com.qti.sensor.gc02m1_lmi.so`, `lmi_ofilm_gc02m1_depth` | Deferred |
| Front pop-up | Sunny S5K3T2 | `com.qti.sensor.s5k3t2_lmi.so`, `lmi_sunny_s5k3t2_front` | Deferred |

## Current supported camera path

Verified OV13B10 path:

```text
OV13B10 -> CSIPHY1 -> CSID1 -> VFE1 RDI0 -> /dev/video3
```

Runtime facts:

- Sensor node: `ov13b10 21-0010`.
- Working CCI bus: `&cci0_i2c1`.
- Working I2C address: `0x10`.
- Working media bus code: `MEDIA_BUS_FMT_SGRBG10_1X10` / `0x300a`.
- Working video node: `/dev/video3` / `msm_vfe1_video0`.
- Working fourcc: `pgAA` / packed 10-bit GRBG Bayer.
- Example smallest mode capture: `1364x768`, stride `1712`, payload `1314816` bytes.

Exposed and validated OV13B10 modes:

| Mode | Approx fps | RAW status |
| --- | ---: | --- |
| 4208x3120 | 29.799 | Captures `pgAA` |
| 4160x3120 | 29.799 | Captures `pgAA` |
| 4160x2340 | 29.799 | Captures `pgAA` |
| 2104x1560 | 59.598 | Captures `pgAA` |
| 2080x1170 | 59.598 | Captures `pgAA` |
| 1364x768 | 120.069 | Captures `pgAA` |

Important boundary: `/dev/video3` is a truthful RAW Bayer node. It must not advertise fake YUYV/NV12/RGB/browser-ready output.

## OV13B10 bring-up summary

Key hardware variables that are now settled:

- Downstream `cci-master = <1>` maps to mainline `&cci0_i2c1` for this sensor.
- OV13B10 ACK/chip-id is at 7-bit `0x10`; the earlier `0x36` candidate NACKed.
- Reset GPIO is TLMM GPIO91 with `GPIO_ACTIVE_LOW`, requested initially asserted and released during sensor power-on.
- MCLK is MCLK2 / GPIO96 at 19.2 MHz.
- VANA enable uses GPIO63 and downstream-compatible rail/load votes.
- PM8009 L3/L7 load votes alone did not fix the old `0x36` NACK; the real blocker was the address.

Main fixes landed during bring-up:

- Built-in CAMCC requirement: `CONFIG_SM_CAMCC_8250=y` is required when CAMSS/CCI/sensor are built-in.
- OV13B10 reset initial state fixed so reset stays asserted until power sequencing releases it.
- Temporary powered CCI scan found chip ID `0x560d42` at `0x10`; scan diagnostics were removed after fixing DTS.
- Direct ioctl helper uses ACTIVE subdev formats, not TRY formats, so CAMSS pads match at `STREAMON`.
- OV13B10 active-state self-deadlock fixed by not binding `sd.state_lock` to the same mutex used by pad/control callbacks.
- CAMSS format enumeration was tightened so active OV13B10 route reports true `pgAA` instead of misleading generic YUV formats.

## Standard sensor metadata and controls

OV13B10 now exposes enough standard metadata for raw-camera bring-up and future camera stack work:

- Frame-size enumeration for all six modes.
- Frame-interval enumeration and `G/S_FRAME_INTERVAL` based on mode timing.
- Crop/native-size selection metadata.
- CSI-2 frame descriptor and mbus config metadata.
- Orientation and rotation metadata: current candidate is rear-facing `orientation = 1`, `rotation = 90`.
- Control discovery through `VIDIOC_QUERY_EXT_CTRL`, including integer64 and compound controls.
- Unit cell size as read-only `V4L2_CTRL_TYPE_AREA`: `1120nm x 1120nm`.
- Controls include exposure, VBLANK/HBLANK, analogue gain, digital gain, link frequency, pixel rate, H/V flip, test pattern, orientation, and rotation.

## Raw helper / web preview support

Helper: `lmi/scripts/lmi-camera-web-preview.py`.

Purpose: no-rootfs-change validation and future web-camera app prototyping above the raw V4L2/media path.

Capabilities:

- Configures the lmi OV13B10 media route directly through Python ioctl.
- Selects modes via `--mode-index` from subdev frame-size enumeration.
- Captures raw `pgAA` frames through V4L2 mplane MMAP.
- Emits structured discovery JSON with schema `lmi.raw-camera.discovery.v1`.
- Enumerates sensor modes, controls, video node caps, filtered formats, TRY_FMT behavior, frame-size metadata, and raw-output boundary notes.
- Provides software preview AE/AWB/color above the raw stream.
- Supports raw and metadata export: `--raw-output`, `--metadata-output`.
- Provides HTTP endpoints with schema `lmi.raw-camera.http.v1`: `/stream`, `/snapshot`, `/raw`, `/metadata`, `/capabilities`.

Boundary:

- AE/AWB/color are software helper logic, not hardware ISP.
- Browser stream is helper HTTP output, not browser `getUserMedia`.
- The helper must keep reporting `kernel_isp_yuv_or_rgb = false` and `browser_ready_yuv_or_rgb = false` until true processed output or a standard userspace stack exists.

## Venus video codec status

Venus is enabled for lmi via upstream SM8250 node:

- DTS override: `&venus { firmware-name = "qcom/sm8250/xiaomi/lmi/venus.mbn"; status = "okay"; };`
- Config: `CONFIG_VIDEO_QCOM_VENUS=m`, plus V4L mem2mem support. Note: the lmi kernel build currently only produces `Image.gz` + dtb and does not install modules, so Venus remains module-backed bring-up unless the test environment provides a matching module.
- Firmware default upstream name would be `qcom/vpu-1.0/venus.mbn`; lmi uses a device-local path.
- Stock `NON-HLOS.bin` contains `image/venus.mdt` and split `image/venus.b00`..`venus.b19`; `venus.mdt` is copied to a temporary `venus.mbn` for upstream MDT loader validation.
- Firmware blobs are only staged temporarily under ignored/local or `/run/firmware` paths and are not committed.

Verified on device:

- `/dev/video14`: `qcom-venus-decoder`.
- `/dev/video15`: `qcom-venus-encoder`.
- Encoder accepts NV12 input and exposes H.264, VP8, and HEVC capture formats.
- Generated NV12 -> H.264 Annex-B hardware encode works and produces SPS/PPS/IDR.
- #467 camera-derived encode check: OV13B10 `/dev/video3` truthful RAW `pgAA` -> helper software preview-derived NV12 -> `/dev/video15` H.264 works. Test frame was 340x192 software NV12, padded to 384x192 for Venus alignment, and encoded to Annex-B H.264 with SPS/PPS start codes.

Boundary: Venus is a video codec, not a camera ISP. It can encode NV12/YUV input, but it does not turn OV13B10 RAW `pgAA` into YUV/RGB.

## SM8250 VFE480 processed YUV/RGB status

Current state:

- Mainline SM8250 CAMSS/VFE480 still has no complete processed ISP/YUV/RGB output path for lmi.
- Verified `/dev/video3` stays RAW RDI.
- VFE1 has an experimental `VFE_LINE_PIX` branch that creates `msm_vfe1_pix` / `/dev/video6` for bring-up only.
- r17-r23 made `/dev/video6` enumerate NV12 and reach `STREAMON`, but no processed frame was ever dequeued.
- Persistent VFE BUS image-size violations and missing FULL comp_done prove `/dev/video6` is not a working YUV node.

What r17-r23 proved:

- VFE1 PIX media node can be exposed without breaking `/dev/video3` RAW.
- CSID source stream 3 can route through PXL/IPP and live config included downstream-required `pix_store_en`.
- VFE480 can allocate two Y/C WMs and program FULL Y/C bus clients 0/1.
- CAMIF starts, PIX reg-update arrives, and RUP ack is visible.
- Top CGC override values matching downstream can be applied and read back.
- Despite all of that, FULL Y/C image-size violations (`0x1`/`0x3`) persist and no buffer completion occurs.

Likely blocker:

- The missing piece is not basic WM allocation, IRQ visibility, top CGC, or CSID pix-store.
- The likely blocker is the real VFE480 processed/top/module chain that converts raw Bayer input to Y/C output. Android downstream relies on camera userspace/tuning/CDM blobs for IFE module programming such as demux, demosaic, CC, CST, gamma, DSX, and related blocks.

Current rule:

- Do not advertise `/dev/video6` as supported YUV/RGB.
- Do not add processed formats to `/dev/video3`.
- Do not keep adding endless dormant guards.
- Further kernel-only diagnostics should be bounded and should avoid pretending unconfigured FULL Y/C is real ISP output.

## VFE480 RAW_DUMP diagnostic status

After r23, the experimental PIX branch was converted away from fake NV12. `/dev/video6` is now used only as a bounded VFE480 RAW_DUMP diagnostic path, not as supported YUV/RGB output.

Implemented diagnostic shape:

- `/dev/video3` remains untouched as RAW `pgAA` RDI.
- Experimental SM8250 PIX enumeration exposes single-plane unpacked GRBG10 `V4L2_PIX_FMT_SGRBG10` / `BA10`, not NV12.
- VFE480 PIX RAW_DUMP maps the logical CAMSS WM to hardware client 10 and comp group 3.
- RAW_DUMP uses `MODE_QCOM_PLAIN` and PLAIN16_10 LSB packer config `0x15`.
- Any future successful `/dev/video6` frame must be documented only as raw diagnostic data, not ISP/YUV/RGB success.

Downstream facts used for the diagnostic:

- FULL Y/C uses bus clients 0/1 and comp group 0.
- RAW_DUMP / PIXEL RAW uses bus client 10 and comp group 3.
- RAW_DUMP image-size violation bit is `0x0400`.
- RAW_DUMP uses source group 0 like FULL, but a different bus client path.
- Downstream RAW_DUMP setup uses one plane, `en_cfg = 0x1`, stride = width, and PLAIN16_10BPP plus LSB align (`0x15`).

r24-r46 device results:

- r24 plain mainline `Image.gz` boot image was rejected in practice by a black screen; the same kernel/DTB booted when repacked through the copydown shim. Keep using copydown images for this diagnostic series.
- r24/r25 converted `/dev/video6` to `BA10` RAW_DUMP and confirmed client10 starts, but capture timed out with RAW_DUMP image-size violation `0x400`.
- r25 fixed RAW_DUMP `IMAGE_CFG_0` low field from byte stride to pixel width: `0x4920820` for `2080x1170`. The `0x400` image-size violation persisted.
- r27 changed RAW_DUMP `IMAGE_CFG_2` from byte stride `0x1040` to pixel width `0x820`; the `0x400` image-size violation persisted.
- r28 changed RAW_DUMP `FRAME_INCR` from byte frame size `0x4a4480` to pixel stride x height `0x252240`; the `0x400` image-size violation persisted.
- r29 programmed VFE480 `TOP_CORE_CFG_INPUTMUX_PP(vfe->id)`, changing VFE1 `core_cfg0` from `0x78002800` to `0x78002820`. This removed the observed RAW_DUMP image-size violation, but no frame was dequeued.
- r30 added stop/non-BUS IRQ snapshots. `/dev/video3` still captured `1364x768 pgAA` payload `1314816`; `/dev/video6` still timed out. Stop snapshot showed no BUS IRQ, no PIX RUP ack, no comp_done, `rup=0x41`, `core_cfg0=0x78002820`, client10 `image=0x4920820/0x0/0x820`, `frame_incr=0x252240`, `packer=0x15`.
- r31 enabled CSID PXL SOF/EOF/error IRQ diagnostics. `/dev/video6` still timed out, but CSID1 PXL reported real SOF/EOF with `error=0x0`, proving the sensor -> CSID PXL side is live.
- r32 reissued VFE PIX `reg_update` once on the first CSID PXL SOF. The reissue fired, `/dev/video3` RAW regression still passed, but `/dev/video6` still timed out with no VFE BUS IRQ, no PIX RUP ack, and no comp_done.
- r33 reasserted VFE480 TOP CGC, TOP debug, and `TOP_CORE_CFG_INPUTMUX_PP(vfe->id)` on repeated PIX `reg_update`. `/dev/video3` RAW regression still passed, but `/dev/video6` still timed out with no BUS IRQ, no PIX RUP ack, and no comp_done. `core_cfg0=0x78002820` stayed correct, so the blocker is not simple PP mux drift.
- r34 enabled VFE TOP diagnostic config and expanded PIX snapshots. `/dev/video3` RAW regression still passed with `1364x768 pgAA` payload `1314816`; `/dev/video6` still timed out. Stop snapshot showed `diag-cfg=0x1` but `diag=0x0/0x0`, `dsp=0x0`, `rup=0x41`, no BUS IRQ, no PIX RUP ack, and no comp_done. The later non-BUS snapshot showed TOP/CAMIF bits cleared after stop, not a successful runtime transition.
- r35 added runtime `vfe480_pix_mux_override` for VFE480 PP input mux sweep. `/dev/video3` RAW regression still passed with `1364x768 pgAA` payload `1314816`. `/dev/video6` still timed out for mux `-1`, `0`, `1`, `2`, and `3`, but mux `0` changed `core_cfg0` to `0x78002800` and produced VFE PIX BUS IRQ plus PIX RUP ack before failing with RAW_DUMP image-size violation `0x400`; mux `-1`/`1` kept `0x78002820`, mux `2` kept `0x78002840`, and mux `3` kept `0x78002860`, all without successful BUS/RUP completion.
- r36 made mux `0` the default diagnostic path and added runtime RAW_DUMP width/stride mode parameters. `/dev/video3` RAW regression still passed with `1364x768 pgAA` payload `1314816`. `/dev/video6` did not dequeue a frame for the width/stride sweep: `w0/s0`, `w0/s1`, and `w0/s2` timed out; `w0/s3`, `w0/s4`, and `w1/s0` failed quickly with `STREAMON` `EIO`; later combinations mostly returned `EBUSY`, so the sweep after the first stuck failure is not reliable evidence for geometry correctness.
- r37 fixed the generic CAMSS `video_start_streaming()` error path to roll back already enabled subdevs if a later subdev returns an error. `/dev/video3` RAW regression still passed with `1364x768 pgAA` payload `1314816`. Repeated `/dev/video6` attempts in one boot no longer collapsed into immediate `EBUSY`; `w0/s3`, `w0/s0`, and another `w0/s3` all reached streaming and timed out, but still produced no frame. RAW_DUMP image-size violation `0x400` persisted on mux `0`.
- r38 added only read-only CAMIF raw-crop and PP CLC `0x2800` diagnostics to the existing violation log. Kernel `#131` booted through copydown; `/dev/video3` RAW regression still passed (`video3 rc=0 raw=1314816 json=1569 png=38509`). `/dev/video6` still timed out (`video6 rc=1 raw=0 json=0 png=0`) and RAW_DUMP image-size violation `0x400` persisted. New evidence showed `raw-crop=0x0/0x0` and `PP CLC[0x2800] status=0x10000001/0x0 cfg=0x0 addr=0x0 size=0x0/0x0 pack=0x0 stride=0x0 frame=0x0 burst=0x0`, so the sampled PP block is not being programmed by the current mainline path.
- r39 matched downstream VFE BUS update ordering more closely for RAW_DUMP by rewriting client10 `cfg`, `image_cfg_0`, `image_cfg_1`, `image_cfg_2`, `image_addr`, and `frame_incr` during buffer update before `reg_update`. Kernel `#132` booted through copydown; `/dev/video3` RAW regression still passed (`video3 rc=0 raw=1314816 json=1569 png=38501`). `/dev/video6` still timed out (`video6 rc=1 raw=0 json=0 png=0`) and RAW_DUMP image-size violation `0x400` persisted. The violation log still showed client10 programmed (`image=0x4920820/0x0/0x820`, `frame_incr=0x252240`, `packer=0x15`) while PP CLC `0x2800` stayed zero, so WM buffer-update timing is not the current blocker.
- r40 added only read-only PP CLC PREPROCESS `0x2200` diagnostics, based on downstream's register dump labels. Kernel `#133` booted through copydown; `/dev/video3` RAW regression still passed (`video3 rc=0 raw=1314816 json=1569 png=35795`, `pgAA` `1364x768` stride `1712`). `/dev/video6` still timed out (`video6 rc=1 raw=0 json=0 png=0`) and RAW_DUMP image-size violation `0x400` persisted. New evidence showed CSID1 PXL SOF/RUP/BUS still active, `PP PREPROCESS[0x2200] status=0x10000000/0x0 cfg=0x1 debug=0x0/0x0`, but `PP CLC[0x2800] status=0x10000001/0x0 cfg=0x0 addr=0x0 size=0x0/0x0 pack=0x0 stride=0x0 frame=0x0 burst=0x0`; so the preprocess block is enabled while the later module block remains unprogrammed.
- r41 added only a downstream-derived read-only name decode for BUS image-size bit `0x0400`. Kernel `#134` booted through copydown; `/dev/video3` RAW regression still passed (`v3_rc=0`, `raw=1314816`, `json=1569`, `png=38520`, `pgAA` `1364x768`, stride `1712`). `/dev/video6` still timed out (`v6_rc=1`, no raw/json/png), and the log now prints `image-size: 0x400 (PIXEL RAW DUMP)` with the same CSID1 PXL SOF/RUP/BUS path, `PP PREPROCESS[0x2200] cfg=0x1`, and unprogrammed `PP CLC[0x2800] cfg=0x0`; this confirms the diagnostic name but does not change the blocker.
- r42 added only read-only PP CAMIF `0x2600` window diagnostics, matching downstream's CAMIF register-dump range. Kernel `#135` booted through copydown; `/dev/video3` RAW regression still passed (`raw=1314816`, `pgAA` `1364x768`, stride `1712`). `/dev/video6` still timed out and still reported `image-size: 0x400 (PIXEL RAW DUMP)`. New evidence showed `PP CAMIF[0x2600] ver=0x10000000 status=0x0 module=0x101 raw-crop=0x0/0x0 skip=0xffffffff/0xffffffff period=0x0 irq=0xffffffff epoch=0x140124 debug=0x410/0x18428008 test=0x0 spare=0x0`, while `PP PREPROCESS[0x2200] cfg=0x1` and `PP CLC[0x2800] cfg=0x0` remained unchanged; so CAMIF is at least enabled, but the downstream-labeled module window is still not programmed by mainline.
- r43 added only read-only BUS client10 status diagnostics from downstream's PIXEL RAW table (`addr_status_0..3`, `debug_status_cfg`, `debug_status_0`, `debug_status_1`) plus BUS top debug status. The copydown image booted through Windows fastboot; `/dev/video3` RAW regression still passed (`raw=1314816`, `pgAA` `1364x768`, stride `1712`). `/dev/video6` still timed out and still reported `image-size: 0x400 (PIXEL RAW DUMP)`. New evidence showed client10 programmed as `cfg=0x1 addr=0xff000000 frame_incr=0x252240 image=0x4920820/0x0/0x820 packer=0x15 burst=0x3`, with `addr_status=0x0/0x0/0x1/0xff800000` and `debug=0x0/0x0/0x0`; after stop, client10 returned to `cfg=0x10 addr=0x0` and zero status. This keeps the blocker on RAW_DUMP source/PP module programming rather than missing ordinary WM register programming.
- r44 aligned mainline VFE480 TOP `core_cfg0` with the Android OV13B10 CAMIF start evidence for mux `0`: Android ctx1 reports `0x60002f00`, while the old mainline mux `0` value was `0x78002800`. Kernel `#137` booted through the copydown image via Windows `fastboot boot`; `/dev/video3` RAW regression still passed at `1364x768 pgAA` with `1314816` bytes. `/dev/video6` configured as `BA10` RAW_DUMP but still timed out at both `1364x768` and `2080x1170`; the live PIX log now shows `core_cfg0=0x60002f00`, CSID PXL SOF/EOF, VFE BUS IRQ, and PIX RUP ack, but no buffer completion. The TOP correction is validated but not sufficient for a supported processed or RAW_DUMP frame.
- r45 kept the true-YUV boundary closed and only added DEMUX even/odd structural names to the bus-violation diagnostic. Boot image `sm8250-xiaomi-lmi-boot/builds/camera-r45-demux-boundary-2026-05-29-143103/boot-linux-copydown-lmi.img` (`sha256=8fe670b73464e1fca8ec2e3b1e1bda98de341a77974dc6fba252222dc3767cd3`) booted as `Linux lmi-ubuntu 7.1.0-rc5-lmi-release+ #139 SMP PREEMPT Fri May 29 14:30:08 CST 2026 aarch64`. `/dev/video3` RAW mode5 regression still passed as truthful `pgAA` only: `1364x768`, stride `1712`, raw payload `1314816` bytes, and both `formats_all` and `formats_for_mbus_code 0x300a` reported only `pgAA`. `/dev/video6` stayed experimental `BA10` only. With the correct PIX route `msm_csid1:pad4 -> msm_vfe1_pix:pad0`, mode5 configured `1360x768` (`bytesperline=2720`, `sizeimage=2088960`) and mode4 configured `2080x1170` (`bytesperline=4160`, `sizeimage=4867200`); both attempts failed with `TimeoutError: timed out waiting for a camera frame`. Dmesg still showed CSID1 PXL activity plus `VFE1 PIX bus irq: status=0x5 reg_update=0x41` and `VFE1 PIX RUP ack`, but no comp_done and no dequeued frame. No `DEMUX[0x2800]` line appeared in this run because the mode4/mode5 timeouts did not trigger the bus-violation path that contains the new even/odd diagnostic. This does not make `/dev/video6` supported YUV/RGB and does not open `vfe480_yc_pp_chain_configured()`.
- r46 added the read-only common-path state dump promised by #468. Boot image `sm8250-xiaomi-lmi-boot/builds/camera-r46-commonpath-state-2026-05-29-155105/boot-linux-copydown-lmi.img` (`sha256=1d83a447cfded669e95b77431185d079f3aa903b909223abbde09069839dff2a`) booted as `Linux lmi-ubuntu 7.1.0-rc5-lmi-release+ #140 SMP PREEMPT Fri May 29 15:41:25 CST 2026 aarch64`. `/dev/video3` RAW mode5 regression still passed as truthful `pgAA` only: `1364x768`, stride `1712`, raw payload `1314816` bytes, and the active route still exposed only `pgAA`. `/dev/video6` stayed experimental `BA10` only. With the correct PIX route `msm_csid1:pad4 -> msm_vfe1_pix:pad0`, mode5 configured `1360x768` (`bytesperline=2720`, `sizeimage=2088960`) and mode4 configured `2080x1170` (`bytesperline=4160`, `sizeimage=4867200`); both attempts still failed with `TimeoutError: timed out waiting for a camera frame`. The new dump now appears even without a BUS violation and showed `DEMUX[0x2800] version=0x10000001 status=0x0 clc=0x0 module=0x0 even=0x0 odd=0x0 bpc-pdpc-demux=0x0 demosaic=0x0 color-correct=0x0 color-xform=0x0`; in one mode4 stop/reset sequence it showed `status=0x3` with the same module fields still zero and a `VFE reset timeout` / `Failed to disable vfe outputs` cleanup failure. This proves the current PIX path reaches CSID PXL/VFE PIX activity while DEMUX/DEMOSAIC/color common-path blocks remain unconfigured; it still does not make `/dev/video6` supported YUV/RGB and does not open `vfe480_yc_pp_chain_configured()`.

Current RAW_DUMP conclusion:

- r29 proved the PP input mux was a real missing TOP setting, but r35 proved the earlier `TOP_CORE_CFG_INPUTMUX_PP(vfe->id)` value for VFE1 (`mux=1`) was not the working lmi route.
- r31 proved CSID PXL receives real frames without PXL errors.
- r32 proved the initial VFE `reg_update` being too early is not sufficient to explain the mux=1 failure.
- r33 proved TOP/CGC/debug/PP mux reassertion is not sufficient when the mux value is wrong; `core_cfg0=0x78002820` stayed stable while PIX RUP/BUS/comp_done remained absent.
- r34 proved simply enabling VFE TOP diagnostic hardware is not sufficient; diagnostic sensor status stayed zero with mux=1 while CSID PXL IRQs continued and PIX `reg_update` remained pending.
- r35 proved `input_mux_sel_pp=0` is the first PP mux value that makes VFE1 PIX see BUS IRQ and RUP ack on the lmi OV13B10 route.
- r36 proved simply sweeping RAW_DUMP width/stride register units on mux=0 is not sufficient to produce a frame.
- r37 proved generic start-stream rollback was missing and could invalidate same-boot sweeps after failures; that cleanup is fixed, but it does not solve RAW_DUMP capture.
- r38 proved the currently sampled PP CLC block stays mostly unprogrammed while RAW_DUMP client10 is programmed and the path reaches BUS/RUP, so ordinary BUS client setup is not the only missing piece.
- r39 proved rewriting RAW_DUMP client10 stride/frame/address registers during buffer update, matching downstream BUS update ordering more closely, is not sufficient to produce a frame or clear image-size violation `0x400`.
- r40 proved the downstream-labeled PP PREPROCESS block is at least enabled (`cfg=0x1`) on the mux=0 path, while the later PP CLC module window at `0x2800` remains unprogrammed; the missing piece is therefore beyond basic preprocess/CAMIF enable and ordinary BUS client setup.
- Downstream `cam_vfe_bus_ver3_err_irq_bottom_half()` decodes BUS image-size violation bit `0x0400` as `PIXEL RAW DUMP image size violation`, matching the current client10 RAW_DUMP path. A small mainline diagnostic now prints that name beside `image-size: 0x400`; this is only a read-only decode, not a functional fix.
- r42 proved the downstream-labeled PP CAMIF window is enabled enough to expose `module=0x101` and epoch/debug state on mux=0, but this still does not program the later PP CLC module window at `0x2800` or produce a RAW_DUMP frame.
- r43 proved downstream-defined RAW_DUMP BUS client10 is programmed and has address-status activity while its debug status remains zero; therefore the current failure is not explained by a missing client10 `cfg`/address/stride/packer write.
- r44 proves the Android-aligned public TOP bits are necessary cleanup but not the missing PP/IFE module-chain: for OV13B10/ctx1, downstream CAMIF start uses `mux_pp=0`, `vid_ds16/vid_ds4=0/0`, `disp_ds16/disp_ds4=1/1`, and IHIST/HDR stats source bits set, producing `core_cfg0=0x60002f00`; mainline now reaches the same value and still times out on `/dev/video6`. This is not a PP module-chain implementation and does not make `/dev/video6` a supported YUV/RGB node.
- r45 proves the new DEMUX even/odd diagnostic is only a future violation aid, not a hidden functional change: `/dev/video3` stayed stable as `pgAA` RAW, `/dev/video6` stayed `BA10` RAW_DUMP diagnostic, both tested PIX modes timed out without comp_done, and the DEMUX diagnostic did not fire because no bus-violation IRQ was observed in that run.
- r46 proves the #468 read-only common-path dump is useful but not functional progress by itself: timeout-only PIX runs now show the DEMUX/DEMOSAIC/color state, and all independently required common-path enable/config registers still read zero (`DEMUX module/even/odd`, `BPC/PDPC demux`, `DEMOSAIC`, `color-correct`, `color-xform`). The next real kernel step must configure a validated VFE480 common-path model; it should not be another helper expansion, fake NV12 enumeration, or copied Android tuning/DMI/CDM value.
- Downstream `cam_vfe_bus_ver3_print_dimensions()` decodes `addr_status_0..3` as last consumed address, last frame address, FIFO count, and current client address. The r43 `addr_status=0x0/0x0/0x1/0xff800000` therefore means no consumed/completed frame, FIFO count 1, and a live/latched current client address; it does not by itself identify a safe register fix.
- Downstream UAPI documents RAW_DUMP output width/stride units as pixels/lines (`cam_isp_out_port_info.width`, `cam_plane_cfg.plane_stride`, `slice_height`), so r43's `image_cfg_2=0x820` and `frame_incr=0x252240` match downstream units for the `2080x1170` diagnostic path and should not be reverted to byte stride.
- Downstream VFE480 CAMIF v3 start does not write `pdaf_raw_crop_width_cfg` / `pdaf_raw_crop_height_cfg`; those offsets only appear in the register table and RegDump range. The r42/r43 `raw-crop=0x0/0x0` is therefore not a proven missing mainline write.
- The r42/r43 `epoch=0x140124` also matches downstream VFE480 CAMIF v3 logic for a 1170-line frame: `epoch0 = (last_line - first_line) / 4 = 0x124`, `epoch1 = 0x14`. The current blocker is not an obvious fixed-epoch typo.
- Downstream RAW_DUMP acquire/config also matches the current client10 diagnostic path in the fields checked: out-port width/height feed acquired width/height, RAW_DUMP maps to WM10/comp group 3, WM10 uses `stride = width`, `en_cfg = 0x1`, PLAIN16_10 LSB packer, start writes `image_cfg_0`/packer/`cfg`, and buffer update writes `image_cfg_2 = plane_stride` plus `frame_inc = plane_stride * slice_height`. This does not reveal a safe missing BUS/WM register write.
- Downstream VFE480 `source_group` is table-driven rather than a separate RAW_DUMP source-selector register: RAW_DUMP and FULL both use `CAM_VFE_BUS_VER3_SRC_GRP_0`, while RDI paths use groups 3/4/5. The source group only drives BUS/RUP resource grouping and IRQ subscription in `cam_vfe_bus_ver3_start_vfe_out()`, which the current mainline mux=0 path already reaches.
- Downstream `input_mux_sel_pp` is not fixed in CAMIF or DTS; it is supplied through the userspace ISP packet blob `CAM_ISP_GENERIC_BLOB_TYPE_IFE_CORE_CONFIG` as `struct cam_isp_core_config.input_mux_sel_pp`, then copied into CAMIF core config. The local mux=0 finding is therefore useful, but the remaining PP/IFE module-chain programming still needs packet/CDM or RegDump evidence rather than another hard-coded kernel guess.
- With mux=0, the remaining blocker is still RAW_DUMP geometry source or missing VFE480 PP/CDM module setup, because repeated same-boot attempts now clean up but still hit RAW_DUMP image-size violation `0x400`.
- Downstream Android confirms that RAW_DUMP WM10/BUS setup is kernel-side, but VFE module register programming is normally delivered by userspace camera packets: `cam_isp_add_command_buffers()` tags `CAM_ISP_PACKET_META_*` command buffers as `CAM_ISP_IQ_BL`, `cam_ife_mgr_config_hw()` submits them through `cam_cdm_submit_bls()`, and CDM `REG_CONT`/`REG_RANDOM` commands write arbitrary VFE offsets such as the `0x2800..0x8ffc` PP CLC module window. There is no obvious static VFE480 PP module table in the downstream kernel to copy.
- #455 compared the current mainline VFE480 RAW_DUMP path against downstream CAMIF/BUS lifecycle code and did not find a safe static PP/IFE module-chain entry point. FULL/FULL_DISP/RAW_DUMP WM indices and comp groups already match downstream (`0/1`, `4/5`, and `10`/group `3`), and no separate RAW_DUMP source-selector register was found; the missing piece remains an open PP/IFE module-chain model rather than another BUS mapping typo hunt.
- #456 adds the code-level boundary for that model without pretending it is implemented: Y/C processed PIX output now has to pass `vfe480_yc_pix_ready()`, which combines valid Y/C layout with an explicit `vfe480_yc_pp_chain_configured()` gate that currently returns false. This keeps RAW_DUMP diagnostic handling separate and prevents future NV12/YUV start, CAMIF, BUS client, and comp-done paths from running until a real open PP/IFE chain exists.
- #457 closes the current read-only search for the next safe code entry: downstream `IFE_CORE_CONFIG` only carries public TOP/CAMIF fields, `cam_vfe480.h` names resource offsets and BUS clients but not a per-module bypass/processed chain, and stock OV13B10 tuned/component symbols point to chromatix/IQ modules. This evidence is not enough to open `vfe480_yc_pp_chain_configured()`; the next step needs an authoritative open Titan480 PP module model or a narrower Android runtime question tied to one specific module-chain semantic.
- #458 found no reusable open Titan480 PP module-chain semantics in the local source set. Mainline VFE4.x exposes an older PIX path with `set_module_cfg()`/DEMUX/scale/crop/xbar registers, but those offsets and modules do not map to VFE480 PP CLC `0x2800..0x8ffc`; current mainline VFE680 is also RDI/WM-only. Downstream Titan480 hits are version/resource branches, CAMIF/TOP setup, BUS tables, or generic `PP CLC Modules` dumps, not a safe processed-chain recipe.
- Stock component-library extraction did not uncover a reusable embedded CDM command stream. The identifiable stock path for learning real register state is the runtime RegDump mechanism: HAL allocates reg-dump command buffers, packet parser records `CAM_ISP_PACKET_META_REG_DUMP_PER_REQUEST` / `_ON_FLUSH` / `_ON_ERROR`, and downstream `cam_soc_util_reg_dump_to_cmd_buf()` reads configured ranges back after CDM programming. That is useful evidence for Android-side capture, but not a no-rootfs mainline fix by itself.
- Offline RegDump trigger analysis found no trustworthy one-shot property or dumpsys command that guarantees IFE RegDump generation without changing the Android camera environment. The downstream debugfs knobs are conditional: `per_req_reg_dump` needs HAL packet descriptors, `enable_req_dump` affects error handling, and `ife_camif_debug` prints CAMIF ranges only on downstream CAMIF error paths. Stock `enable3ADebugData` / `enableTuningMetadata` are the only direct config toggles found and are disabled by default.
- Further work should focus on capturing/understanding stock userspace camera packet or register-dump contents from a running Android stack, or implementing a real open mainline ISP configuration path, not on advertising processed formats or blind geometry sweeps.

## Android runtime camera evidence

Initial Android/crDroid runtime evidence was captured after carving a temporary Android `userdata` partition and booting the stock crDroid Android 16 `boot.img`. Evidence files are local-only under `lmi/.local/android-camera-evidence/20260529-android-runtime-1/` and should not be committed:

- `lmi_camera_logcat5_ultra.txt`
- `lmi_camera_dumpsys5_ultra.txt`
- `lmi_camera_trace5_ultra.txt`
- Earlier comparison captures: `lmi_camera_trace4_main.txt`, `lmi_camera_logcat4_main.txt`, `lmi_camera_dumpsys4_main.txt`

Confirmed ultrawide facts from Android Aperture `0.7x`:

- Android Camera ID `2` is the OV13B10 ultrawide path: logcat reports `SensorCaps sensorId=3 positionType=3 activitysize(8,8,4208,3120) ov13b10_lmi`.
- `dumpsys media.camera` shows Aperture connected to camera ID `2`; static metadata reports back-facing focal length `2.13`, pixel array `4224x3136`, and RAW/YUV/manual capabilities.
- Active client streams were a `1600x1200` SurfaceView with format `0x22` and a `4000x3000` ImageReader with format `0x21`; this confirms Android is using its HAL pipeline, not a raw-only V4L2 browser node.
- Tracefs camera events were usable only after enabling both per-event camera tracepoints and global `tracing_on`.

Ultrawide tracefs summary:

- Event counts included `cam_apply_req=342`, `cam_req_mgr_apply_request=342`, `cam_req_mgr_add_req=350`, `cam_isp_activated_irq=2045`, `cam_log_event=1596`, and `cam_buf_done=1700`.
- `bufdone_IRQ val2` values were comp group IDs, not `CAM_VFE_BUS_VER3_VFE_OUT_*` output enum values. Downstream `cam_vfe_bus_ver3.c` logs `resource_data->comp_grp_type` in that field.
- Observed ultrawide comp group counts were: group 1 = 113, group 2 = 113, group 5 = 113, group 6 = 113, group 7 = 113, group 8 = 112, group 9 = 3, group 10 = 3, group 11 = 113, group 12 = 113.
- `val1` matched comp-done IRQ status bits shifted by downstream `comp_done_shift = 6`: group 1 -> `0x80`, group 2 -> `0x100`, group 5 -> `0x800`, group 6 -> `0x1000`, group 7 -> `0x2000`, group 8 -> `0x4000`, group 9 -> `0x8000`, group 10 -> `0x10000`, group 11 -> `0x20000`, group 12 -> `0x40000`.

Downstream VFE480 BUS mapping relevant to the captured groups:

| Comp group | Downstream BUS clients | Output meaning |
| ---: | --- | --- |
| 0 | clients 0/1/2/3 | VID FULL Y/C + DS4/DS16; not observed in the ultrawide trace sample |
| 1 | clients 4/5/6/7 | DISP FULL Y/C + DISP DS4/DS16 |
| 2 | clients 8/9 | FD Y/C |
| 3 | client 10 | PIXEL RAW / RAW_DUMP; this is the current mainline `/dev/video6` diagnostic target, but it was not observed in the Android ultrawide bufdone sample |
| 5 | clients 12/13 | HDR BE / HDR BHIST stats |
| 6 | clients 14/15 | Tintless BG / AWB BG stats |
| 7 | clients 16/17/18/19 | BHIST / RS / CS / IHIST stats |
| 8 | client 20 | BF stats |
| 9 | client 21 | PDAF / 2PD |
| 10 | client 22 | LCR |
| 11 | client 23 | RDI0 |
| 12 | client 24 | RDI1 |

A second Android pass used locally built downstream release kernels with temporary `CAM_INFO` / `LMI_CAM_TRACE` instrumentation, because broad debug kernels were not viable:

- The local downstream release config boots Android.
- `CONFIG_DEBUG_FS=y` alone fails early on this setup and falls back to Ubuntu; `DEBUG_FS+KPROBES/KPROBE_EVENTS` and the full `FUNCTION_TRACER/DYNAMIC_FTRACE` debug config also fail. No useful pstore crash log was preserved.
- Therefore the downstream `camera_ife/*` debugfs knobs and kprobe events are not a reliable path here; evidence collection used bootable release config plus targeted log-only instrumentation.
- The temporary trace boot images were started with `fastboot boot` only and did not write the boot partition or rootfs.
- Evidence files are local-only under `lmi/.local/android-boot-test/20260529-camera-debug/`, including `lmi-camtrace-id0-20260529-044040.log`, `lmi-camtrace-id2-20260529-044155.log`, `lmi-cdm-payload-dmesg.txt`, `lmi-cdm-payload-trace.txt`, `lmi-cdm-payload-full-dmesg.txt`, `lmi-cdm-payload-full-ctx1-summary.txt`, `lmi-dmi-table-dump-dmesg.txt`, `lmi-dmi-table-dump-mapped-dmesg.txt`, `lmi-dmi-table-dump-mapped-summary.{txt,csv,json}`, and Aperture screenshots.

The Camera ID `2` / OV13B10 `LMI_CAM_TRACE` pass captured:

- `core_blob`: `ctx=1`, `vid_ds16=0`, `vid_ds4=0`, `disp_ds16=1`, `disp_ds4=1`, `dsp_tap=0`, `ihist=1`, `hdr_be=1`, `hdr_bhist=1`, `mux_pdaf=0`, `mux_pp=0`.
- `camif_start`: `vfe=1`, `core_cfg=0x60002f00`, `first=0,0`, `last=4207,3119`, `mux_pp=0`, `mux_pdaf=0`, matching a `4208x3120` active CAMIF window.
- Counts in the first saved ID2 log: `1458` CDM BL entries, `292` CDM submits, `299` VFE out blobs, `16` WM acquire logs, `14` VFE out acquire logs, one core blob, and one CAMIF start.
- The first CDM payload pass captured `6567` `LMI_CAM_TRACE` lines, including `190` `cdm_payload_*` lines and no `cdm_payload_map_fail` entries, but only sampled the first 64 dwords per buffer. A follow-up full-dword pass captured `3898` `LMI_CAM_TRACE` lines, `632` `cdm_payload_*` lines, `525` `cdm_payload_words` lines, `10` payload metadata records, and no `cdm_payload_map_fail` entries. Aperture had to be woken/unlocked first; otherwise it registered as a camera listener but did not hold an active preview session.

Camera ID `2` acquired VFE outputs:

| res_type | UAPI meaning | downstream out | WM(s) | Comp | Mask | Format / pack | Dimensions | Notes |
| --- | --- | ---: | --- | ---: | --- | --- | --- | --- |
| `0x3004` | FD | 8 | 8/9 | 2 | `0x300` | `0x20` / `0x3` | `640x480` | two-plane FD output |
| `0x300a` | HDR BE stats | 10 | 12 | 5 | `0x1000` | `0x13` / `0xa` | `294912x1` | stats |
| `0x300b` | HDR BHIST stats | 11 | 13 | 5 | `0x2000` | `0x13` / `0xa` | `6144x1` | stats |
| `0x300c` | TL BG stats | 12 | 14 | 6 | `0x4000` | `0x13` / `0xa` | `294912x1` | stats |
| `0x300d` | BF stats | 13 | 20 | 8 | `0x100000` | `0x13` / `0xa` | `4x360` | stats |
| `0x300e` | AWB BG stats | 14 | 15 | 6 | `0x8000` | `0x13` / `0xa` | `1382400x1` | stats |
| `0x300f` | BHIST stats | 15 | 16 | 7 | `0x10000` | `0x13` / `0xa` | `32768x1` | stats |
| `0x3010` | RS stats | 16 | 17 | 7 | `0x20000` | `0x11` / `0x8` | `65536x1` | stats |
| `0x3012` | IHIST stats | 18 | 19 | 7 | `0x80000` | `0x11` / `0x8` | `4096x1` | stats |
| `0x3013` | FULL_DISP | 19 | 4/5 | 1 | `0x30` | `0x27` / `0xb` | `1600x1200` | display processed output, `ubwc=1`, Y/C WMs `1600x1200` + `1600x600` |
| `0x3014` | DS4_DISP | 20 | 6 | 1 | `0x40` | `0x24` / `0xa` | `400x300` | display downscale; WM log shows `200x150` |
| `0x3015` | DS16_DISP | 21 | 7 | 1 | `0x80` | `0x24` / `0xa` | `100x76` | display downscale; WM log shows `50x38` |
| `0x3007` | RDI_1 | 1 | 24 | 12 | `0x1000000` | `0x3` / `0x0` | `4208x3120` | raw/RDI path active in HAL |
| `0x3006` | RDI_0 | 0 | 23 | 11 | `0x800000` | `0xe` / `0x0` | `528x780` | secondary raw/RDI path active in HAL |

CDM packet and payload evidence from the ID2 logs:

- Init packet `ctx=1 req=0` submitted three BL entries, including IQ command buffers with `flags=1`, `offset=0 len=2424` and `offset=2428 len=568`.
- Runtime requests generally submitted five BL entries each; the large `flags=1` entry is the per-request IQ BL, while the other entries are short update/auxiliary BLs.
- The full payload pass mapped the `CAM_ISP_IQ_BL` buffers successfully through `cam_mem_get_cpu_buf()`. For the active OV13B10 session, `ctx=1 req=0 idx=1` parsed `606` dwords and `idx=2` parsed `142` dwords; `req=1 idx=1` parsed `597` dwords; `req=2 idx=1` parsed `332` dwords; `req=3 idx=1` parsed `174` dwords. All five active ctx=1 buffers had `truncated=0`.
- `ctx=1 req=0 idx=1` is a large PP/IFE module program: `109` parsed commands, dominated by PP module `REG_CONT` writes, plus `14` DMI loads. It repeatedly programs PP CAMIF CLC offsets `0x2660`, `0x2668`, `0x2670`, and `0x2678`, then a wide set of downstream-labeled `PP CLC Modules` offsets in `0x2800..0x8ffc`.
- The init PP module program includes key ranges such as `0x3458/0x3468`, `0x2e58/0x2e68`, `0x3090`, `0x3058/0x3068/0x30ac`, `0x3658/0x3668`, `0x3860..0x3e58`, `0x4060..0x6268`, `0x7e60..0x8e68`, and `0x8858..0x88f0`. Later requests update a smaller but still substantial subset of the same module chain.
- DMI loads are now fully visible as command headers, for example `req=0 idx=1` loads `0x3408` banks `1/3/4` (`256/128/168` bytes), `0x2e08` bank `1` (`144` bytes), `0x3008` banks `1/2` (`512/256` bytes), `0x3608` banks `1/2/3` (`884` bytes each), `0x3c08` bank `1` (`512` bytes), `0x3e08` banks `1/2/3` (`256` bytes each), and `0x8808` bank `1` (`300` bytes). These command-buffer entries point at external DMI source buffers.
- `ctx=1 req=0 idx=2` is a separate WM timing/subsample program: `14` `REG_RANDOM` commands writing framedrop and IRQ subsample fields for active WM clients, including FD WMs `8/9`, RDI WM `24`, stats WMs `12..20`, display WMs `4/5/6/7`, and RDI/stats-related WMs `23/24`.
- The full decode confirms that the previous `64`-dword sample undercounted both command count and DMI activity. It was enough to identify offsets, but not enough to reconstruct the actual Android PP/IFE programming sequence.

A follow-up Android release-kernel pass instrumented `cam_packet_util_process_patches()` at the exact packet patch point where KMD writes `src_iova + src_offset` into the CDM DMI command `addr` field. The first pass captured `4964` DMI patch metadata records but no table words because the source handles were not allocated with `CAM_MEM_FLAG_KMD_ACCESS`, so `cam_mem_get_cpu_buf()` returned `-EINVAL`. A second temporary evidence-only pass added a bounded `cam_mem_lmi_get_cpu_buf_any()` path that maps the DMA-BUF directly and unmaps it immediately after dumping.

Mapped DMI-table evidence from the second pass:

- Boot method remained `fastboot boot` only; no Android boot/rootfs partition was flashed or modified.
- Aperture Camera ID `2` was active while the trace was captured.
- `lmi-dmi-table-dump-mapped-dmesg.txt` contains `128` dumped DMI patches, `1849` `dmi_patch_words` lines, and `0` map failures.
- Requests covered by the bounded dump: req `0` = `19` patches, req `1` = `59`, req `2` = `25`, req `3` = `7`, req `4` = `18`.
- Source handles in the captured run were `0x80002b` (`80` patches) and `0x57000f` (`48` patches).
- The summary artifacts `lmi-dmi-table-dump-mapped-summary.txt`, `.csv`, and `.json` index each table by request, patch index, source handle/offset, patched IOVA, DMI address, DMI bank selector, byte length, and dumped words.
- Frequently loaded DMI targets include `0x3008` banks `1/2`, `0x3408` banks `1/3/4`, `0x3608` banks `1/2/3`, `0x3c08` bank `1`, `0x3e08` banks `1/2/3`, `0x8208` banks `1..12`, `0x8808` banks `1/2`, and `0xa608` banks `1..4`.
- Example init tables now have actual data: `0x3408` bank `1` starts with descending lens/shading-like values such as `0000b200 00004859 00003a7d ...`; `0x3408` banks `3/4` are `0x100`-style tables; `0x3608` banks `1/2` contain long nonzero 221-word curves; `0x8808` bank `1` contains packed multi-word entries beginning `d03940ab 000d9028 ...`.

Boundary: this proves Android's external DMI tables can be captured without persistent rootfs changes, but it is still Android HAL/CHI/tuning-generated data. It should be used as evidence to understand VFE480 PP/IFE modules and to compare against mainline register state, not pasted into the mainline kernel as a static proprietary tuning table.

A third Android release-kernel pass added a bounded post-CDM register snapshot in `cam_ife_cam_cdm_callback()` after `CAM_CDM_CB_STATUS_BL_SUCCESS`. The snapshot used the existing CAMIF `CAM_ISP_HW_CMD_QUERY_REGSPACE_DATA` path to get the VFE core reg map and then read fixed windows with `cam_io_r_mb()`. It was limited to the first `8` dumps, init packets or req `<= 3`, and selected PP CAMIF / PP module / BUS windows.

Post-CDM register evidence:

- Boot method remained `fastboot boot` only; no Android boot/rootfs partition was flashed or modified.
- Aperture Camera ID `2` was active while the trace was captured.
- Evidence artifacts: `lmi-post-cdm-regs-dmesg.txt`, `lmi-post-cdm-regs-summary.txt`, `.csv`, `.json`, and `lmi-post-cdm-regs-ctx1-focus.txt`.
- The pass captured `6857` `LMI_CAM_TRACE` lines, `728` `post_cdm` lines, `8` post-CDM metadata dumps, `720` parsed register lines, and `216` window rows. Dumps `1..4` were `ctx=0`; dumps `5..8` were `ctx=1`, the active OV13B10 / Aperture Camera ID `2` context.
- For `ctx=1`, req `0` was an init snapshot (`opcode=1`) and req `1..3` were per-request snapshots (`opcode=2`) after CDM BL success.
- `ctx=1` PP CAMIF windows are live: `camif_2600` (`0x2600`, 36 dwords) has `3/4` nonzero dwords across req `0..3`; `camif_27f0` has debug/status-like values changing from `80428008` at init to `106x8018 06181070` on runtime requests.
- `ctx=1` PP module / DMI-adjacent windows are live: `0x2e00`, `0x3000`, `0x3400`, `0x3600`, `0x3c00`, `0x3e00`, `0x8000`, `0x8200`, `0x8800`, and `0x8a00` all contain nonzero post-CDM state. `0x9000` stayed all zero in this capture; `0x7c00` and sampled `0xa600` were mostly static in the sampled range.
- `ctx=1` BUS clients are also programmed after CDM completion. Display-like clients `0xb000/0xb100/0xb200/0xb300`, FD clients `0xb400/0xb500`, stats clients `0xb800/0xbb00/0xbc00/0xc000`, and RDI clients `0xc300/0xc400` all have nonzero state. Init req `0` contains geometry/config with zero output addresses; runtime req `1..3` fill changing buffer addresses and sizes.
- Example runtime BUS values for `ctx=1`: `bus_b000` req `1` starts `00000001 d6205000 002a8000 04b00640 ...`; `bus_b100` req `1` starts `00000001 d64ab000 00159000 02580640 ...`; `bus_c300` req `0` differs from `ctx=0` geometry (`030c0042` instead of `06b80090`), matching the active ID2 resource split rather than a generic static table.

Boundary: this proves Android's post-CDM VFE core state is readable without persistent rootfs changes and confirms the captured CDM/DMI data is reflected in live VFE registers. It still does not authorize copying HAL/CHI/tuning-generated register or DMI values into mainline as static proprietary tuning data.

A fourth Android release-kernel pass expanded the same read-only post-CDM snapshot to the PP module subwindows that the CDM parser had identified as written but the first post-CDM pass had not covered:

- Boot method remained `fastboot boot` via Windows platform tools only; no Android boot/rootfs partition was flashed or modified.
- Test boot image: `lmi/.local/android-boot-test/20260529-camera-debug/repack-tests/boot-camera-post-cdm-missing-windows-release-Imagegz-noavb.img`, SHA256 `d1f10ade5339f57a7065ba98af2ecace2869851cfa1d5c870964a9df6984f7e4`.
- Kernel `Image.gz` SHA256: `e5d298349894aba8c67ccfefb1241a303f5be5c50ced62c5a7c49b0eed2e89bc`; DTB SHA256: `996519d9284c14280ad4b626156210d7c83342f2db5dbe0e7d8004657d6eeb5c`.
- Aperture was switched to `0.7x`; the captured rows are `ctx=1`, matching the OV13B10 / Android Camera ID `2` ultrawide path.
- Evidence artifacts: `lmi-post-cdm-missing-windows-dmesg.txt`, `lmi-post-cdm-missing-windows-summary.txt`, `.csv`, and `.json` under `lmi/.local/android-boot-test/20260529-camera-debug/`.
- The pass captured `552` post-CDM segment lines, `224` aggregated rows, req `0..3`, dumps `5..8`, and all `29/29` targeted new PP module windows.
- Newly confirmed live post-CDM windows include `0x3260`, `0x3860`, `0x3a60`, `0x4060`, `0x4460`, `0x4660`, `0x4860`, `0x4a60`, `0x4c60`, `0x4e60`, `0x5060`, `0x5260`, `0x5408`, `0x5504`, `0x5608`, `0x5704`, `0x5860`, `0x5a60`, `0x5c08`, `0x5d04`, `0x5e08`, `0x5f04`, `0x6060`, `0x6260`, `0x7e60`, `0x8460`, `0x8660`, `0x8890`, and `0x8e60`.
- Example latest rows show nonzero Android state in geometry/curve/table-adjacent PP module subwindows, such as `mod_3260` nonzero `7/8`, `mod_4060` nonzero `12/16`, `mod_5060` nonzero `7/16`, `mod_6260` nonzero `7/16`, and `mod_8890` nonzero `21/32`.

Boundary: this closes the earlier missing-window gap in the Android evidence, but it still does not make the values safe static kernel constants. The useful result is structural: Android's OV13B10 PP/IFE module chain spans many CDM-written windows beyond the coarse first snapshot.

Correlation against downstream VFE480 definitions:

- `0x2600` and `0x27f0` are named in downstream as the PP CLC CAMIF block: `cam_vfe480.h` maps `0x2600` to `hw_version`, `0x2660` to `module_cfg`, `0x2668/0x266c` to PDAF raw-crop width/height, `0x2670/0x2674` to skip patterns, `0x2678` to period config, `0x2680` to epoch IRQ config, and `0x27f0..0x27fc` to debug/test/spare registers.
- Downstream's own register-dump path labels `0x2800..0x8ffc` only as `PP CLC Modules`; the kernel tree does not provide safe per-submodule names for the sampled `0x2e00`, `0x3000`, `0x3400`, `0x3600`, `0x3c00`, `0x3e00`, `0x7c00`, `0x8000`, `0x8200`, `0x8800`, or `0x8a00` windows. These should stay documented as PP CLC module windows unless a more authoritative Titan/IFE register map is found.
- `0xa600` is in the downstream lite CAMIF dump path as `CLC PDLIB` (`0xa600..0xa718`), with the related PDLIB CAMIF block at `0xa400..0xa5fc`.
- VFE480 BUS client mapping is explicit in `cam_vfe480.h`: `0xb000/0xb100` are DISP Y/C clients 4/5, `0xb200` is DISP DS4 client 6, `0xb300` is DISP DS16 client 7, `0xb400/0xb500` are FD Y/C clients 8/9, `0xb600` is PIXEL RAW client 10, `0xb700` is CAMIF PD client 11, `0xb800..0xc000` cover stats clients, and `0xc300/0xc400` are RDI0/RDI1 clients 23/24.
- This makes the Android/mainline mismatch sharper: Android ID2 preview uses DISP/FD/stats/RDI clients plus a broad PP CLC module program; mainline `/dev/video6` uses the separate PIXEL RAW/RAW_DUMP client 10 and has not shown the Android-style PP CLC module chain. The current mainline failure is therefore not a missing write to one ordinary BUS client register.

Interpretation:

- Android's OV13B10 path is not just the mainline raw RDI path: it uses processed display output, FD, many stats streams, and RDI resources through the downstream HAL/IFE/CDM pipeline.
- The `mux_pp=0` runtime finding matches the earlier mainline mux sweep result where mux `0` was the first value that made the VFE1 PIX path see BUS/RUP activity.
- The useful processed preview path in this Android sample is `FULL_DISP` comp group 1 with two WMs and `ubwc=1`, not a simple proof that mainline can advertise linear NV12/YUV today.
- VID FULL comp group 0 and RAW_DUMP comp group 3 were not acquired in the ID2 preview sample; this reinforces that the mainline `/dev/video6` RAW_DUMP diagnostic is not the same path Android Aperture uses for preview.
- The payload, mapped-DMI, and post-CDM register evidence now prove Android userspace is actively programming PP CAMIF CLC, a broad PP/IFE module chain, DMI table selectors, WM framedrop/IRQ fields, and live BUS client state through CDM/IQ BLs. The missing mainline piece is therefore not just BUS output wiring; it is the VFE480 PP/IFE module-chain programming normally generated by HAL/CHI/tuning.
- The captured CDM/DMI/post-CDM register windows now have a matching mainline comparison target. Copying only the observed BUS/WM setup into mainline will not produce true ISP/YUV by itself.

Mainline diagnostic patch and r24 comparison result:

- `drivers/media/platform/qcom/camss/camss-vfe-480.c` now emits a one-shot, read-only `VFE%u PIX ... compare windows` dump from the BUS violation path when the PIX/RAW_DUMP diagnostic path is active.
- The dump covers the confirmed comparison rows: PP CLC CAMIF `0x2600/0x27e0`, selected downstream-labeled `PP CLC Modules` rows from `0x2800..0x8a00`, CLC PDLIB `0xa600`, and VFE480 BUS client rows including `0xb600` PIXEL RAW plus display/FD/stats/RDI rows.
- `lmi-camera-vfe480-compare-2026-05-29-r24` booted through the copydown shim via Windows fastboot only. Boot image: `sm8250-xiaomi-lmi-boot/builds/lmi-camera-vfe480-compare-2026-05-29-r24/boot-linux-copydown-lmi.img`, SHA256 `26d70de8bc1a57d4ebbe1acf76f83b594e07d206cff2fb7355f0b2f8a7551ad8`; runtime DTB SHA256 `cf8ee80f547ab58b44ea1d0b6815c1d03c96eebb39eed4b64d71412b1bd1dde4`.
- Triggering `/dev/video6` on `msm_vfe1_pix` with mux `0`, `mode-index 4`, and `BA10` still timed out; no RAW_DUMP frame was dequeued. The kernel still reached client10 start/address updates, PIX BUS IRQ, PIX RUP ack, and then `image-size: 0x400 (PIXEL RAW DUMP)`.
- The r24 one-shot dump parsed `27` mainline rows. Evidence files: `lmi-mainline-vfe480-compare-r24-dmesg.txt`, `lmi-mainline-vfe480-compare-r24-helper.log`, `lmi-mainline-vfe480-compare-r24-summary.txt`, and `.json` under `lmi/.local/android-boot-test/20260529-camera-debug/`.
- Corrected comparison against Android `ctx=1` post-CDM rows shows `13 / 27` dumped offsets differ in the first 8 dwords and `21` offsets are nearly blank in mainline while Android has more nonzero state. Android did not include same-offset rows for mainline-only debug/RAW_DUMP windows `0x27e0`, `0x2800`, `0xb600`, and `0xb700`.
- Clear mismatches include PP module rows `0x2e00`, `0x3600`, and `0x8800`; display/FD/stats/RDI BUS clients `0xb000`, `0xb100`, `0xb400`, `0xb500`, `0xb800`, `0xbb00`, `0xbc00`, `0xc000`, `0xc300`, and `0xc400`; Android also has programmed `0xb200`/`0xb300` DISP DS rows that were not in the mainline dump.
- This does not program any Android CDM/DMI values and does not change `/dev/video6` into supported YUV/RGB. It confirms that current mainline only programs the RAW_DUMP client10 diagnostic enough to hit BUS/RUP, while Android post-CDM state contains active HAL/CDM programming across PP module windows and display/FD/stats/RDI BUS clients.

CDM/post-CDM/mainline correlation artifacts:

- `lmi-vfe480-cdm-post-mainline-correlation.txt`, `.csv`, and `.json` correlate the captured Android `ctx=1` CDM payload, mapped DMI targets, post-CDM register rows, and r24 mainline one-shot dump.
- `lmi-vfe480-cdm-post-mainline-correlation-expanded.txt`, `.csv`, and `.json` fold in the missing-window pass and expand the previously uncaptured PP module bucket.
- The original parser found `373` active `ctx=1` CDM commands across init and req `1..3`, plus `27` captured Android post-CDM windows.
- The expanded pass adds `552` post-CDM segment lines / `224` aggregated rows and confirms `29/29` targeted CDM-written PP module subwindows. These new windows account for the original `PP_MODULES_UNCAPTURED_OR_PARTIAL` bucket's `166` REG_CONT commands.
- Android CDM explicitly writes `camif_2600` offsets such as `0x2660`, `0x2670`, `0x2678`, and `0x2668`; the same window is nonzero in post-CDM snapshots. Mainline r24 only matches the first status-like dword and lacks the broader programmed CAMIF state.
- CDM writes and DMI loads line up with the first live PP module windows: `mod_2e00`, `mod_3000`, `mod_3400`, `mod_3600`, `mod_3c00`, `mod_3e00`, `mod_8800`, and related ranges all have REG_CONT and/or DMI activity, and post-CDM snapshots show nonzero state there. Mainline r24 is mostly only the first status/header dword in these windows.
- The expanded pass confirms the narrower PP subwindows are also live after CDM: `mod_3260`, `mod_3860`, `mod_3a60`, `mod_4060`, `mod_4460`, `mod_4660`, `mod_4860`, `mod_4a60`, `mod_4c60`, `mod_4e60`, `mod_5060`, `mod_5260`, `mod_5408`, `mod_5504`, `mod_5608`, `mod_5704`, `mod_5860`, `mod_5a60`, `mod_5c08`, `mod_5d04`, `mod_5e08`, `mod_5f04`, `mod_6060`, `mod_6260`, `mod_7e60`, `mod_8460`, `mod_8660`, `mod_8890`, and `mod_8e60`.
- BUS client post-state must be interpreted carefully: display/FD/stats/RDI client rows are partly kernel/resource-manager setup and partly CDM-updated framedrop/IRQ fields; runtime req rows also contain changing buffer addresses. These values are evidence of Android's active graph, not safe static register constants to paste into mainline.
- #455 read-only review adds the same boundary from the downstream kernel side: the explicit VFE480 BUS mapping agrees with current mainline for FULL, FULL_DISP, and RAW_DUMP, while CAMIF/TOP setup only covers public mux/epoch/IRQ state. The broad PP module windows are produced by Android HAL/CHI/CDM packets and live DMI-backed state, so copying the captured values would be copying tuning output rather than implementing a mainline ISP path.
- #459 mapped the 128 captured DMI table loads against the expanded `ctx=1` PP windows. DMI writes land at the `+0x8` table-control offset of `mod_2e00`, `mod_3000`, `mod_3400`, `mod_3600`, `mod_3c00`, `mod_3e00`, `mod_7c00`, `mod_8000`, `mod_8200`, `mod_8800`, `mod_8a00`, `mod_9000`, and `mod_a600`, plus one unmapped `0x1288` target. Nine of the thirteen variable PP windows have DMI attached, and even stable windows such as `mod_7c00`, `mod_8200`, and `mod_a600` carry multiple DMI selectors/payloads; this looks like HAL/CHI/tuning-backed module state, not a minimal public bypass/enable recipe.
- #460 first narrowed the local-source question: stock `camera.qcom.so` exposes module/class names and source paths such as `camxifelinearization34titan480.cpp`, `camxifedemux13titan480.cpp`, `camxifedemosaic36titan480.cpp`, `camxifegamma16titan480.cpp`, `camxifepedestal13titan480.cpp`, `camxifebls12titan480.cpp`, `camxifeabf40titan480.cpp`, `camxifehdr30titan480.cpp`, `camxifelcr10titan480.cpp`, and `camxifemnds21titan480.cpp`; OV13B10 tuned blobs expose matching Chromatix/IQ module names. Local stock/downstream sources still did not expose a usable VFE480 PP register layout, and old VFE17x `0x2e00/0x3000/0x3400/0x3600` hits are BUS-client layouts from earlier hardware, not VFE480 PP semantics.
- Public GitHub code search found stronger external references: `comprehensive9/vendor_qcom_proprietary` branch `11se` carries CamX Titan480 module sources plus generated `prebuilt_HY11/target/product/kona/obj/include/camx/titan48x/titan480_ife.h` (no repository license declared). This maps key PP CLC registers such as CAMIF `0x2660`, DEMUX module `0x2860`, BPC/PDPC DEMUX config `0x3090`, DEMOSAIC `0x3860`, color correct `0x3a60`, GLUT DMI bank `0x3e58`, color transform `0x4060`, and video full Y/C MNDS/crop blocks `0x6460..0x6a60`. The mirror is technically useful for deriving module semantics and cross-checking our Android CDM/post-CDM captures, but it must be treated as proprietary reference material: do not copy CamX code, generated headers, or tuning constants into the mainline kernel; reimplement only a small, understood, clean-room style register model if the semantics are confirmed.
- #461 converted the public no-license CamX/Titan480 mirror into a bounded semantic map rather than copying source or generated header content. The useful structure is: CommonPath programs CAMIF, pedestal, ABF, linearization, demux, PDPC, HDR, LSC, WB, demosaic, color correct, GTM, gamma/GLUT, and color transform; VideoFullPath then programs Y/C MNDS, crop, and round/clamp before BUS output. For an open mainline attempt, the important split is between simple control registers and tuning/DMI-backed modules.

  | Block | Key offsets | Minimal-use meaning | DMI/tuning risk |
  | --- | --- | --- | --- |
  | PP CAMIF | `0x2660`, `0x2668..0x2680` | enable CAMIF-to-IFE output, Bayer pattern, skip/period/epoch geometry | low; already partly mirrored by public CAMIF/TOP evidence |
  | DEMUX | `0x2860`, `0x3090` | demux enable/period plus BPC/PDPC demux gain/black-level routing | medium; demux can be described structurally, but Android also programs PDPC state |
  | BLS / black level | `0x7c60` plus adjacent black-level regs | black-level subtraction/offset setup | medium; captured Android DMI also touches `0x7c08`, so static values are not safe |
  | DEMOSAIC | `0x3860` and interpolation coefficients | RAW Bayer to RGB-domain reconstruction | medium; can name the block, but quality/correctness depends on tuned coefficients |
  | Color correct | `0x3a60` and matrix coefficients | RGB color correction matrix | medium; identity/bypass may be possible, copied tuning is not |
  | Color transform | `0x4060` and transform coefficients | RGB-to-YUV/color-space transform | medium; identity/BT matrix must be cleanly derived, not copied |
  | Gamma/GLUT, LSC, ABF, GTM, pedestal, linearization | `0x2c00`, `0x2e00`, `0x3400`, `0x3600`, `0x3c00`, `0x3e00` and DMI controls | image-quality modules with tables/curves | high; Android DMI evidence shows these are HAL/CHI/tuning generated |
  | VideoFull Y/C MNDS | `0x6460`, `0x6660` | output scaler for luma/chroma video path | low-to-medium; geometry is derivable, but must match BUS stride/planes |
  | VideoFull Y/C crop + round/clamp | `0x6860`, `0x6a60` plus crop/clamp regs | final Y/C crop, rounding, clamping | low-to-medium; safe only if dimensions and format are fully understood |

  This makes the next clean implementation target narrower: first build a minimal non-UBWC VideoFull Y/C path from PP CAMIF through DEMUX/DEMOSAIC/color transform and VideoFull Y/C MNDS/crop/round-clamp, with DMI-heavy IQ modules bypassed or left disabled where the hardware allows. The block names and offsets are now good enough to design a mainline register model, but not enough to paste Android register values or DMI payloads into `vfe480_yc_pp_chain_configured()`.
- The actionable conclusion remains: next progress needs a real open VFE480 PP/IFE module-chain model, or a carefully bounded way to translate these structures into mainline semantics. It should not be another blind RAW_DUMP geometry sweep, dormant guard, fake YUV advertisement, or copied HAL/CHI tuning table.

## Pitfalls log

- Do not enable the pop-up front camera before lift/position handling is understood.
- Do not copy Android downstream `qcom,cam-sensor` nodes directly into mainline DTS.
- Treat CCI mapping and I2C address as real-device variables until probe proves them.
- Do not commit vendor blobs, stock DTB contents, extracted firmware, or signed camera/video blobs.
- Do not install camera tools or packages into rootfs for kernel bring-up unless explicitly allowed.
- Direct subdev ioctl tests must use `V4L2_SUBDEV_FORMAT_ACTIVE`; TRY formats do not configure the live CAMSS path.
- Python ioctl structures must match 64-bit UAPI layout exactly.
- V4L2 controls retain state across runs; helper should reset controls before AE tests unless preserving state is explicit.
- Long VBLANK lowers frame rate; browser-style preview should default to video-priority AE.
- Do not query remote media pads during CAMSS video-device registration; the media graph is not linked yet.
- Use sensor subdev frame-size enumeration as the authoritative OV13B10 mode list; CAMSS video node reports generic continuous sizes.
- Do not treat unfiltered CAMSS format tables as real ISP output; filtered active media-bus format is the relevant current boundary.
- Do not treat Venus as camera ISP.
- Do not treat `/dev/video6` `NV12` enumeration as support; r17-r23 showed negotiation can pass while `DQBUF` never returns.
- For lmi fastboot camera tests, reboot device to bootloader first with `systemctl --no-wall --reboot-argument=bootloader reboot`, then run Windows `fastboot.exe boot ...img`.

## Current next steps

1. Keep `/dev/video3` RAW path stable and regress it after any CAMSS/VFE change.
2. Keep `/dev/video6` documented only as RAW_DUMP diagnostic until a real frame is dequeued and payload is validated.
3. Use the full CDM decode and mapped DMI summaries as Android references for PP/IFE module programming, but do not copy them blindly into mainline because the values are HAL/CHI/tuning-generated.
4. Treat the confirmed downstream labels as the current boundary: PP CLC CAMIF `0x2600/0x27f0`, generic PP CLC Modules `0x2800..0x8ffc`, CLC PDLIB `0xa600..0xa718`, and explicit VFE480 BUS clients `0xb000..0xc500`.
5. Do not invent per-module names or static programming for `0x2e00..0x8a00`; the downstream kernel does not expose enough submodule semantics for a safe open implementation yet.
6. Treat the r24 mainline comparison dump, the expanded Android post-CDM evidence, and the #459 DMI-to-PP-window correlation as the current diagnostic baseline; the missing-window Android pass already covered the known CDM-written PP subwindows, and the DMI cross-check reinforces that the observed stable/variable windows are not safe static mainline programming values.
7. Treat the r44 TOP core config alignment and #455 downstream CAMIF/BUS review as closed bounded checks: public TOP bits and ordinary BUS mapping are now ruled out as the only blocker.
8. Keep the #456 `vfe480_yc_pix_ready()` gate closed until a real open PP/IFE module-chain configuration exists; do not bypass it with fake NV12 enumeration, blind RAW_DUMP geometry sweeps, or copied Android tuning/DMI values.
9. If another Android pass is needed, make it narrower than the current missing-window pass and tie it to a specific unanswered module-chain question.
10. #462 has now been translated into code-level boundaries: `camss-vfe-480.c` names the Titan480 PP blocks needed for a minimal VideoFull Y/C chain, the `/dev/video6` Y/C gate reads DEMUX/DEMOSAIC/color-transform/MNDS/crop/round-clamp readiness, and it still returns false until those blocks are genuinely programmed. This is a design checkpoint, not YUV support.
11. #463 added the first clean-room VideoFull Y/C terminal configuration entry: 1:1/no-scale MNDS image/crop setup plus post-MNDS crop/clamp setup for Y and C, derived only from requested V4L2 geometry. This did not copy Android DMI/tuning values and did not open the `vfe480_yc_pp_chain_configured()` gate.
12. #464 common-path semantic check found that DEMUX has low-risk structural fields (`0x2860` module enable/period, `0x2868` even-line pattern, `0x286c` odd-line pattern), but DEMOSAIC immediately depends on WB gain/offset and interpolation registers (`0x3868..0x3878`), while color-transform depends on matrix/offset/clamp registers (`0x4068..0x4094`). DEMOSAIC is not safe to enable from guessed values or Android tuning captures.
13. #465 adds only the independently derivable part of that common path: a CST12/color-transform register model and BT.601 full-range RGB-to-YUV 1024-fixed-point programming, with coefficients derived from the public color-space formula rather than copied from CamX constructors or Android captures. This still does not configure DEMUX/DEMOSAIC, and the Y/C gate remains closed.
14. #466 checked the public Demosaic36/37 setting, IQ-module, XML, and XSD references for a safe bypass/bilinear path. The useful public semantics are only that `dis_directional_g/rb=1` disables directional interpolation and that `edge_det_*` / `lambda_*` remain tuning-controlled; mono XML disables demosaic only for monochrome sensors. This does not prove a safe Bayer OV13B10 DEMOSAIC reset/bypass/bilinear model.
15. The next kernel step is not another dormant guard: either obtain an open hardware semantic for Titan480 DEMOSAIC programming, or build a clearly marked software-ISP userspace bridge above RAW while keeping kernel `/dev/video3` truthful RAW and `/dev/video6` unsupported for YUV.
16. #467 implements and validates that bounded bridge in `lmi-camera-web-preview.py`: `--nv12-output` writes software preview-derived NV12 from the truthful RAW Bayer RDI frame, and the resulting camera-derived NV12 was successfully fed to `/dev/video15` Venus for H.264 Annex-B output after width alignment. This remains RAW + software conversion + hardware encoding, not kernel ISP/YUV output and not browser `getUserMedia` support.
17. #468 continues the true VFE480/YUV path without opening it: `camss-vfe-480.c` now names the DEMUX even/odd structural registers (`0x2868`/`0x286c`), includes them in the PIX bus-violation diagnostic, and also dumps the read-only DEMUX/DEMOSAIC/color module state from the PIX stop/client-status path so timeout runs that do not raise a BUS violation can still show whether the common-path blocks are configured. The in-code `vfe480_yc_pp_chain_configured()` gate remains closed until an independently validated open DEMUX + DEMOSAIC + color model exists. This intentionally does not program DEMUX, does not enable DEMOSAIC, does not add NV12 enumeration, and does not make `/dev/video6` a supported YUV node.
18. r46 validation closes the current diagnostic-only #468 step: the newly added stop/client-status dump confirms the common path is still completely closed (`DEMUX` module/even/odd, `BPC/PDPC demux`, `DEMOSAIC`, `color-correct`, and `color-xform` all zero) while CSID PXL/VFE PIX still show activity and `/dev/video6` still times out as `BA10`. The next implementation must either introduce a clean, minimal, publicly justifiable DEMUX/DEMOSAIC/color common-path configuration or keep the Y/C gate closed; do not spend more iterations on RAW helper features or unsupported format advertising.
19. #468 follow-up semantic search did not find a safe open register model for the missing common-path blocks. The useful public material currently stops at abstract CAMSS/IFE demux parameters (`gain_even`, `gain_odd`, `gain_gr`, `gain_gb`, `gain_r`, `gain_b`, `period`, `blk_in`, `blk_out`) and explicitly does not define the VFE480 register packing for `0x2860/0x2868/0x286c/0x3090`; public Demosaic36/37 references still only prove high-level knobs such as disabling directional interpolation, not a complete Bayer OV13B10 reset/bilinear path. Android runtime evidence proves HAL/CHI/CDM writes `0x3090`, `0x3860`, `0x3a60`, and `0x4060`, but those values are tuning/runtime output and remain non-copyable. Therefore the kernel must keep `vfe480_yc_pp_chain_configured()` closed and `/dev/video6` diagnostic `BA10` only until an independently validated DEMUX + DEMOSAIC + color model exists.
20. #468 OPE v2 语义复核只提供了可用于比对的线索，不足以作为在线 VFE480 开链补丁：公开 `qcom-camss-ope` 是离线 mem2mem OPE，不是 Titan480/VFE480 online path；它把 CLC_DEMO 命名为 `module +0x60/+0x68/+0x6c/+0x70`，字段为 `lambda_g/lambda_rb/a_k/w_k`，默认值约为 `lambda_rb=0`、`lambda_g=128`、`a_k=128`、`w_k=102`，可用于解释 Android `0x3860..0x3870` 这类 post-CDM 证据，但不能证明 OV13B10 的在线 DEMOSAIC 安全默认值。该补丁也没有给出 VFE480 `0x2860/0x2868/0x286c/0x3090` DEMUX/BPC-PDPC demux 打包语义，且 OPE chroma-enhancement/RGB2YUV 与当前 VFE480 `0x4068..0x4094` CST12/color-xform 模型不同；因此仍只能做文档/诊断比对，不能功能性开启 DEMUX/DEMOSAIC/color 或打开 Y/C gate。
21. 本地源码再次确认 DEMUX 仍是硬阻塞：GPL 主线 `camss-vfe-4-1.c`/`4-7.c`/`4-8.c` 有旧 VFE DEMUX period/gain/even/odd pattern 写法，但那是 legacy packed-YUV/older VFE 模型，不能直接迁到 Titan480 Bayer DEMUX13；Android GPL techpack `cam_vfe480.h` 和 `cam_vfe_camif_ver3.c` 只提供 VFE480 CAMIF/TOP/BUS/violation 模块编号等硬件表，不包含 `0x2868/0x286c/0x3090` 的字段 packing。当前树里唯一 VFE480 DEMUX 偏移定义仍是 `camss-vfe-480.c` 的读回/日志命名，足够做诊断但不够写寄存器。
22. #468 下一步只加入 read-only detail diagnostics，不打开功能 gate：`vfe480_dump_pix_common_path()` 现在会同时读出 `0x3090..0x30a8` 的 BPC/PDPC DEMUX 7-word 窗口和 `0x3860..0x387c` 的 DEMOSAIC 8-word 窗口，用于和 Android CDM 证据中 request-varying 的 `0x3090` 以及 OPE v2 CLC_DEMO `+0x60/+0x68/+0x6c/+0x70` 线索做对照。该补丁仍不写 DEMUX/DEMOSAIC/color，不声明 `0x3090` packing，不启用 Y/C，不改变 `/dev/video6` `BA10` 诊断属性；下一轮实机只验证这些窗口在 mainline 下是否仍为零或是否暴露新的只读状态。
23. r47 detail-diagnostic validation confirmed the narrower boundary. Boot image `sm8250-xiaomi-lmi-boot/builds/camera-r47-common-detail-2026-05-29-185236/boot-linux-copydown-lmi.img` (`sha256=93f947ab60abbf25c416c6b6244d96972ddd9b21e466f0cb0d1e9aeab2e5a9d9`) booted as `Linux lmi-ubuntu 7.1.0-rc5-lmi-release+ #141 SMP PREEMPT Fri May 29 18:51:34 CST 2026 aarch64`. `/dev/video3` RAW mode5 regression still passed as truthful `pgAA` only (`1364x768`, stride `1712`, `raw_size=1314816`, `raw_format=bayer10p`, `kernel_isp=false`). `/dev/video6` still enumerated only `BA10` on `msm_vfe1_video3`; with the correct `msm_csid1:pad4 -> msm_vfe1_pix:pad0` route, mode5 (`1360x768`, `bytesperline=2720`, `sizeimage=2088960`) and mode4 (`2080x1170`, `bytesperline=4160`, `sizeimage=4867200`) both timed out waiting for a frame. The new detail dumps showed `BPC/PDPC demux[0x3090]: 00000000 00000000 00000000 00000000 00000000 00000000 00000000` and `DEMOSAIC[0x3860]: 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000` in both mode5 and mode4 stop/reset paths; `DEMUX module/even/odd`, `color-correct`, and `color-xform` also remained zero. This proves mainline still never programs the Android/OPE-observed common-path windows, so no functional YUV/RGB claim is valid and `vfe480_yc_pp_chain_configured()` must stay closed.

24. r47 后把 Android CDM 证据和 mainline 零读回对齐后，结论更明确：Android OV13B10/ctx1 会通过 IQ/CDM 在每个请求写 `0x3090` BPC/PDPC DEMUX 7-word 窗口，且中间 4 个 word 随请求变化（例如 req0 `04040404`、req1 `042b042b`、req2/req3 `041d041d`），说明它不是固定 reset/default 配置。Android 同时写 DEMOSAIC `0x3860=00004001`、`0x3868..0x3874`（req0 可为全 0，req1 出现 `070f0400 00000723`）、`0x3878=00000080 00800066`，并打开 `0x3a60` color-correct 与 `0x4060` color-xform；但 r47 mainline 在同一类窗口读到 `0x3090..0x30a8` 和 `0x3860..0x387c` 全 0，`DEMUX module/even/odd`、`color-correct`、`color-xform` 也全 0。这个对照只证明 Android HAL/CHI/tuning/CDM 确实运行时配置了 common path，不能作为 mainline 静态寄存器表；在没有公开可解释的 `0x2860/0x2868/0x286c/0x3090` DEMUX/BPC-PDPC packing 和 DEMOSAIC 安全模型前，不能打开 Y/C gate。

25. 本地 `/mnt/c/Users/MengChao/Downloads/4pda_spectra480` 只读检查也没有解除 #468 的硬阻塞。`Spectra480+Files.zip` 中的 `Source Code/` 与 `XSD and XML/` 是 Chromatix Parameter Parser 生成/配套资料，标注为 Qualcomm confidential/proprietary，只能作为“参数语义存在”的参考而不能复制到内核。可用的公开层面结论很有限：`demux_1_3_0` 只描述 `demux_enable` 和 `green_gain_even/odd`、`blue_gain`、`red_gain` 这类 tuning 参数；`demosaic_3_6_0` / `3_7_0` 只描述 `edge_det_weight`、`edge_det_noise_offset`、`dis_directional_g/rb`、`lambda_g/rb`、`en_dyna_clamp_g/rb`；`cst_1_2_0` / `cc_1_3_0` 只描述矩阵、offset、clamp、q-factor 等高层参数。文件中没有 `titan480`、`IFE_IFE_0`、`PP_CLC`、`BPC_PDPC_DEMUX`、`PackIQRegisterSetting`、`CreateCmdList`、`0x2860`、`0x3090`、`0x3860`、`0x4060` 等寄存器/打包证据；三个 RAR 的 Chromatix Parser 包也主要是 xsd/parser 工具，未提供 Titan480 register header 或 HW setting 源码。`ife_spectra480_000.xsd` 只说明 Spectra480 IFE 参数集合包含 `pedestal13`、`linearization34`、`pdpc30`、`demux13`、`demosaic36`、`cc13`、`cst12` 等模块，不定义在线 VFE480 register offsets/bitfields。附带 `Spectra data flow.png` 只给出 BPS/IFE/IPE 的 Bayer->Demosaic->CC/GTM->Gamma->CSC->YUV420 高层数据流。结论：这些文件支持“模块链语义名称”的文档判断，但不能用来编写 `0x2860/0x2868/0x286c/0x3090` DEMUX/BPC-PDPC packing，也不能证明 OV13B10 安全 DEMOSAIC 默认值；`vfe480_yc_pp_chain_configured()` 仍必须保持关闭，`/dev/video6` 仍只能是 `BA10` 诊断节点。

26. 继续只读反编译 Android vendor Camera HAL 后，#468 的 clean-room 语义边界更清楚，但仍不足以打开真 YUV gate。`/home/ccc007/Android/Kernel/lmi/.local/camera-stock/hal/lib64__hw__camera.qcom.so` 虽然主 ELF stripped，但 `.gnu_debugdata` 暴露了 `CamX::IFEDemux13Titan480`、`IFEDemosaic36Titan480`、`IFEWB13Titan480`、`IFEPDPC30Titan480` 等本地符号；实际代码需按这些地址在原始 HAL binary 上反汇编。已确认 `IFEDemux13Titan480::CreateCmdList` 只把对象 blob `+0x18` 开始的 7 个 word 写到 `0x3090`，`PackIQRegisterSetting` 把输入字段打包为 `moduleConfig`、`demuxGainCH0`、`demuxGainCH12`、`demuxRightGainCH0`、`demuxRightGainCH12`、`demuxEvenConfig`、`demuxOddConfig`，其中 `+0x1c..+0x28` 是多组 15-bit gain pair，`+0x2c/+0x30` 分别来自 input `+0x08/+0x04` 的 even/odd config；这解释了 Android CDM `0x3090` 7-word 块为什么会随请求变化，但它仍是 HAL/tuning 生成值，不是可静态复制的 mainline 配置，也没有给当前 `0x2860/0x2868/0x286c` 直接开链一个安全答案。`IFEDemosaic36Titan480::CreateCmdList` 写 `0x3860` count 1 和 `0x3878` count 2，`0x3868` count 4 则由 `IFEWB13Titan480` 写入；WB13 的 packing 是多组 15-bit gain/offset 类字段，说明 Android CDM 里的 `0x3868..0x3874` 不是 Demosaic36 自己的固定默认值。`IFEPDPC30Titan480::CreateCmdList` 还会写 `0x3058` count 3、`0x3068` count 10、`0x30ac` count 10，并通过 `0x3008` 选择器 1/2 装载两段 DMI/table 数据（约 `0x200` 和 `0x100` 字节）；其 packing 覆盖 black level、exposure ratio/recipe、BPC/BCC offset、RG/BG/GR/GB WB gain、PDAF offset/end/table offset、Bayer pattern 等，并从输入大偏移与 runtime/context 指针、table copy 中取值。结论：反汇编已经证明相关 common-path/PDPC/DEMUX/WB/DEMOSAIC 状态是 HAL/CHI/tuning/CDM/DMI 组合生成的结构化程序，不能把 Android 请求值搬进内核；在没有独立可解释、可最小化、可实机验证的 DEMUX + PDPC + DEMOSAIC + WB + color 模型前，`vfe480_yc_pp_chain_configured()` 必须继续返回 false，`/dev/video6` 继续只是 `BA10` RAW_DUMP 诊断节点。

27. 继续 #485 定位 `0x2860/0x2868/0x286c` 后，当前最可信结论是：这些 DEMUX module/even/odd CLC 寄存器没有在 stock HAL 的 IFE Titan480 IQ-module `CreateCmdList`/`CreateSubCmdList` 路径中被直接运行时写入。对 `PacketBuilder::WriteRegRange` 的全局调用点和 IFE Titan480 `CreateCmdList`/`CreateSubCmdList`/`SetupRegisterSetting`/`CopyRegCmd`/`DumpRegConfig` 复核显示，在线 IFE 相关命中仍是 `IFEDemux13Titan480 -> 0x3090 count 7`、`IFEDemosaic36Titan480 -> 0x3860/0x3878`、`IFEWB13Titan480 -> 0x3868`、`IFEPDPC30Titan480 -> 0x3058/0x3068/0x30ac`、`IFECC13Titan480 -> 0x3a60/0x3a68`、`IFECST12Titan480 -> 0x4060/0x4068`；`IFECAMIFPPTitan480` 只写 PP CAMIF `0x2660/0x2670/0x2678/0x2668`，不是 DEMUX `0x2860`。直接立即数里出现的 `0x2868` 属于离线 BPS WB13（Titan17x/Titan480）路径，不是在线 IFE DEMUX；`0x2860` 其它命中是插值/数据偏移或 rodata 元数据。`CamX::Titan480IFE::ProgramIQModuleEnableConfig` 在该 HAL 中实际是 `return 0`，不像 Titan150/175 那样写一组 module-enable 寄存器；`Titan480IFE::SetupCoreConfig` 只生成 generic blob type 7、size `0x30`，downstream GPL 对应 `CAM_ISP_GENERIC_BLOB_TYPE_IFE_CORE_CONFIG -> CAM_ISP_HW_CMD_CORE_CONFIG`，其字段最终影响 TOP `core_cfg_0`（VFE480 offset `0x2c`）里的 DS/R2PD、DSP tap、stats mux、`input_mux_sel_pp` 等 CAMIF/TOP 选择，不是 `0x2860` DEMUX CLC。唯一明确包含 `0x2860` 的 Titan480IFE 结构是 hang/regdump rodata 表：stride `0x6c` 的 entry 6 描述 `0x2860 count 4`，覆盖 `0x2860/0x2864/0x2868/0x286c`，但它是 dump metadata，不是运行时写寄存器证据。Android post-CDM/CDM payload 证据也没有把 `0x2860` 作为已写 PP module window 暴露出来，只稳定看到 `0x3090` 及后续 PDPC/DEMOSAIC/WB/color 窗口。因此不能从 Android HAL 得出“写 `0x2860/0x2868/0x286c` 即可开 DEMUX”的结论；下一步若继续真 YUV，只能转向独立解释 `0x3090`/PDPC/DEMOSAIC/WB/color 的最小可验证模型，或寻找公开/可 clean-room 的 Titan480 PP CLC header。当前 gate 继续关闭，`/dev/video6` 继续是 `BA10` RAW_DUMP 诊断节点。

28. 继续 #486 只读反编译 color / CST / terminal 模块后，VFE480 后段窗口可归类得更完整，但仍不能打开真 YUV gate。`IFECC13Titan480::CreateCmdList` 写 `0x3a60 count 1`（object `+0x18`）和 `0x3a68 count 9`（object `+0x1c`），`CreateSubCmdList` 只写 `0x3a60 count 1`；`SetupRegisterSetting` 仅把输入 bit0 合入 `+0x18` bit0，正常 `PackIQRegisterSetting` 则把 module enable 置入 `+0x18`，并把 `+0x1c..+0x3c` 标成 `coefficientAConfig0/1`、`coefficientBConfig0/1`、`coefficientCConfig0/1`、`offsetKConfig0/1`、`shiftMConfig`，由 12-bit coefficient、13-bit offset 和 2-bit shift 字段组合而来。`IFECST12Titan480::CreateCmdList` 写 `0x4060 count 1`（`+0x18`）和 `0x4068 count 12`（`+0x1c`），`CreateSubCmdList` 同样只写 module enable；Dump label 对应 `ch0/ch1/ch2` 的 `Coefficient0/1`、`OffsetConfig`、`ClampConfig`，packing 由每通道 13-bit coefficient、11-bit offset、10-bit clamp 字段组成。terminal path 方面，`IFEMNDS21Titan480::CreateCmdList` 会按 output path 写多组 MNDS，已确认 VideoFull Y/C 是 `0x6460 count 9`（object `+0xa8`）和 `0x6660 count 9`（`+0xcc`）；`PackIQRegisterSetting` 会保存 runtime/context 指针，调用 `CalculateInterpolationResolution()`，按 `+0xfc` path 分支选择不同 9-word block，并由输入/输出尺寸、缩放比例、stripe/pre-crop context 状态计算 phase/step/size 类字段，而不是使用固定表。`IFECrop11Titan480` 也是按 path 选择 crop window，其中 VideoFull Y/C 对应 `0x6868/0x6a68 count 2`；`IFERoundClamp11Titan480` 对 VideoFull Y/C 写 `0x6860 count 1 + 0x6870 count 6` 和 `0x6a60 count 1 + 0x6a70 count 6`，同一函数还选择其它 FD/DS/disp/video 终端窗口。结论：后段 Y/C terminal、CC13、CST12 的寄存器窗口、blob 偏移和字段类型已经有 clean-room 事实，但它们全部依赖 HAL/tuning/runtime 尺寸与路径状态；前段 DEMUX/PDPC/DEMOSAIC/WB 仍缺可独立解释的最小模型，不能用 Android CDM/DMI/tuning 输出值填空，`vfe480_yc_pp_chain_configured()` 继续保持 false，`/dev/video6` 继续只是 `BA10` RAW_DUMP 诊断节点。

29. #487 补齐了 `IFEDemosaic36Titan480` 与 `IFEWB13Titan480` 的最小字段图，但结论仍是“可解释窗口”，不是“可开启配置”。`IFEDemosaic36Titan480::CreateCmdList` 的在线写入仍只有 `0x3860 count 1`（object `+0x18`）和 `0x3878 count 2`（object `+0x1c/+0x20`）；`DumpRegConfig` label 对应 `Demo moduleConfig`、`Demo interpolationCoeffConfig`、`Demo interpolationClassifier0`。`PackIQRegisterSetting` 对 `+0x18` 只打包若干 1-bit 开关：input `+0x00` bit0 -> bit0，`+0x02` bit0 -> bit10，`+0x0e/+0x10/+0x12/+0x14` bit0 -> bits12..15；对 `+0x1c` 写 input `+0x16` byte -> bits0..7、input `+0x18` byte -> bits16..23；对 `+0x20` 写 input `+0x08` low10 -> bits0..9、input `+0x0a` low12 -> bits16..27。`SetupRegisterSetting` 只把输入 bit0 合入 `+0x18` bit0。`IFEWB13Titan480::CreateCmdList` 写 `0x3868 count 4`（object `+0x18..+0x24`），因此 Android CDM 中 `0x3868..0x3874` 属于 WB13，不是 Demosaic36；`DumpRegConfig` label 对应 `G_Gain`、`B_Gain`、`R_Gain`、`G_OFFSET`、`B_OFFSET`、`R_OFFSET`。`PackIQRegisterSetting` 把 input `+0x04/+0x06/+0x08` 作为三组 15-bit gain 写入 `+0x18 low/high15` 与 `+0x1c low15`，把 input `+0x0a/+0x0c/+0x0e` 作为三组 15-bit offset 写入 `+0x20 low/high15` 与 `+0x24 low15`。这些事实解释了 `0x3860`/`0x3878`/`0x3868` 的模块边界和字段宽度，但 Demosaic36 的 interpolation coefficient/classifier 与 WB gain/offset 仍来自 HAL tuning/runtime 计算；没有公开 Bayer OV13B10 安全默认/旁路模型，不能把 Android 请求值静态搬进 mainline，Y/C gate 保持关闭。

30. #488 追踪了 VFE480 common-path 模块 **enable 的来源**，并用多 agent 对抗式独立反汇编核对了每条结论（每个事实都被一个单独 agent 重新反汇编验证，地址均为 binary 自身 VA）。最终结论是：stock HAL 里**不存在可复制的中央/静态 module-enable 配方**，模块 enable 由分散的 per-module 运行时计算产生。已验证事实：
    - `Titan480IFE::ProgramIQModuleEnableConfig` @`0x4cac68` 是纯 stub：`mov w0,wzr; ret`，不写任何 module-enable 寄存器（与会写一组 enable 寄存器的 Titan150/175 不同）。
    - `Titan480IFE::GetIFEDefaultConfig` @`0x4cb4e8` 仅把 rodata `0x155dd0` 的 16 字节常量 `{0x10, 0x400, 0x5a8, 0x4}`（u32×4）拷给 `IFEDefaultModuleConfig*`，是默认拓扑/计数标量，不是 `{register,value}` enable 对，也不写任何寄存器。
    - `Titan480IFE::GetISPIQModulesOfType` @`0x4cac70` 线性扫描基址 `0x8d23b8`、stride `0x18` 的静态描述符表，按请求 module type 比对 word[0]，命中后拷 24 字节（`q0`+`x8`）到输出；表项形如 `{moduleType, pathId, present=1, ...}`，只是“模块存在性/输出路径枚举”，不含寄存器值，全函数体无 `bl/blr/br`、不写寄存器。
    - `IFENode::ProgramIQConfig` @`0x366938` 是 per-frame 调度器：迭代 IQ module list 并对每个模块做 12 处 `blr` 虚调用（各模块自身的 RunCalculation/CreateCmdList），自身 0 次 PacketBuilder 调用；唯一文件 IO（`fopen`@`0x367170`）是被运行时 flag 门控（`ldarb`@`0x3670a8` + `tbz`）的 tuning-metadata 调试 dump，与寄存器编程无关。
    - `IFENode::ProgramIQEnable` @`0x369188` 是 mode 门控的薄调度器：对 `[x0+0xe5dc]` 做 5/6 范围判断（`sub w12,w11,#5; cmp w12,#1; b.hi`），可能把模块字段 `+0x3c` 的 3-bit 经 `bfxil` 拷进 context `[x9+0x578]`，然后选三条几乎相同的“记录日志 + 尾调用 `vtable+0x18` 虚函数”分支；它**不写任何 VFE 寄存器**。注意：先前一度把“stride-`0x2e0` 数组 + 跳表 + bit `0x80` 位运算”的 per-module enable 机制误记到本函数——对抗式核对已纠正：那套机制实际属于相邻的 `NewActiveStreamsSetup` @`0x3694a0`，`ProgramIQEnable` 内没有该数组/跳表/`0x80` 掩码。
    - 在 6 个 IFE Titan480 `CreateCmdList` 内穷举 PacketBuilder 调用，写入窗口与 #485/#486/#487 完全一致：Demux13→`0x3090`(7)、Demosaic36→`0x3860`(1)+`0x3878`(2)、WB13→`0x3868`(4)、PDPC30→`0x3058`(3)+`0x3068`(10)+`0x30ac`(10)+`0x3008` DMI、CC13→`0x3a60`(1)+`0x3a68`(9)、CST12→`0x4060`(1)+`0x4068`(12)，每个函数内没有任何额外 PacketBuilder 调用。全二进制范围内 `0x286c` 作为寄存器立即数 0 命中，`0x2868` 仅出现在 `BPSWB13Titan17x/Titan480::CreateCmdList` 两处（BPS `0x2xxx` 基址，非 IFE），`0x2860` 仅出现在 Linearization34/33 插值查表的栈存项，均非 IFE 在线 DEMUX 写入。
    - **完整性审计**（专门尝试反证“是否还有被漏掉的中央 enable 路径”）反证失败，即三段式模型完整：已逐一排除 `IFENode::AcquireResources` @`0x35fd20`（5 个 generic blob，type `0/1/4/5/6`，是 CSL acquire 的 resource/clock/bandwidth/core/UBWC，运行时逐字段构建，无静态 enable 表）、`Titan480IFE::SetupCoreConfig` @`0x4cb518`（1 个 type7/48 字节 generic blob，只拷 2 个输入字段，是 core/CAMIF config 不是 IQ enable）、`ProcessingNodeFinalizeInitialization` @`0x35c940`（0 PacketBuilder）、`PostPipelineCreate` @`0x369808`（0 PacketBuilder）。
    - 结论（对内核决策）：VFE480 common-path 模块 enable 来源是三段——(a) 静态模块存在性表（只有 type/path，无寄存器值），(b) `GetIFEDefaultConfig` 极小默认结构（拓扑/计数标量），(c) 每帧、每模块的 RunCalculation+CreateCmdList 自己设置自身 enable 位（如 DEMOSAIC `0x3860` bit0）并从 tuning 输入打包寄存器，由 `ProgramIQEnable` 的运行时门控决定是否激活；`ProgramIQModuleEnableConfig` 为 stub 进一步证明没有 monolithic enable 块可搬。因此 mainline 仍缺安全的 DEMUX/DEMOSAIC/WB/color 静态默认模型，不能把 Android tuning/CDM/DMI 值当常量，`vfe480_yc_pp_chain_configured()` 必须继续保持关闭，`/dev/video6` 继续只是 `BA10` RAW_DUMP 诊断节点。真 YUV 的下一步只能是独立构建可解释、可最小化、可实机验证的模块默认模型，而不是从 HAL 提取配方。

31. #468 转向**用户态 software-ISP → 标准相机节点**（在 #488 证明内核 ISP 配方不可提取之后），目标是在不伪装 RAW 节点的前提下提供独立标准 YUYV/NV12 节点，供 host UVC consumer 和后续本机 PipeWire/browser `getUserMedia` 验证使用；内核继续保持真实 RAW、Y/C gate 关闭。**Phase 0/1/2/3 已实机跑通端到端；本机无桌面 `/dev/video20` 标准 V4L2 程序调用已验证，PipeWire/browser `getUserMedia` 仍未验证。**
    - **软件 ISP daemon**（扩展 `lmi/scripts/lmi-camera-web-preview.py`，仍纯 stdlib）：新增 `--isp-sink {none,loopback,uvc}`、`--isp-format {yuyv,nv12}`、`--isp-width/height`、`--isp-src-width`（demosaic 工作分辨率 perf 旋钮）、`--isp-fps-cap`、`--isp-dump/--isp-frames/--isp-status`。新增 `rgb_to_yuyv`（YUY2/4:2:2，BT.601，按节点 `bytesperline` 排版+最近邻 resample）和 `rgb_to_nv12_scaled` packer，新增单平面 `v4l2_pix_format` + `V4L2_BUF_TYPE_VIDEO_OUTPUT=2` 和 `LoopbackSink`（`S_FMT(OUTPUT)` 回读 stride/sizeimage，每帧 `os.write()`），以及 `isp_loop`（抓 RAW→demosaic→AWB/AE→pack→写节点，AE 用 pre-gain luma）。`VIDIOC_S_FMT` ioctl 号不变（union 加 `pix` 后 `V4L2Format` 仍 208 字节）。
    - **vendored v4l2loopback**：upstream main 0.15.3（GPL-2.0）放入 `drivers/media/v4l2loopback/`，`bool CONFIG_V4L2LOOPBACK=y`（构建不产模块，必须 `=y`），hook `drivers/media/{Kconfig,Makefile}`。它不使用 videobuf2、自管缓冲；7.1 上所有 `LINUX_VERSION_CODE` 守卫都走最新分支，与本树原生 API 吻合（`timer_delete_sync`、2-arg `v4l2_fh_add(fh,filp)`、`VFL_TYPE_VIDEO`）。本地只保留一个 lmi 默认设备补丁：built-in 默认 `devices=1`、`video_nr[0]=20`，避免 auto-numbered loopback 抢在 CAMSS 前面导致 RAW 节点编号整体漂移；启动参数仍可覆盖。
    - **配置陷阱已修**：`m1.config`/`m1-release.config` 显式钉 `CONFIG_VIDEO_DEV=y`、`CONFIG_V4L2LOOPBACK=y`，并为 UVC sink 钉 `CONFIG_VIDEOBUF2_VMALLOC=y`、`CONFIG_UVC_COMMON=y`、`CONFIG_USB_F_UVC=y`、`CONFIG_USB_CONFIGFS_F_UVC=y`（生成 .config 里这两个曾是 `=m` 即死代码）。
    - **实机验证**（boot image `sm8250-xiaomi-lmi-boot/builds/camera-isp-r1-2026-05-30-081339/boot-linux-copydown-lmi.img`，`sha256=18543d3419fe5f7127b4b94bbf86cff953e6696962df37cff8dc37bacbfa4ed0`，`fastboot boot` 起 `#142` 内核）：当时开机 `v4l2-loopback driver version 0.15.3 loaded`，自动建节点 `/dev/video0`（"Dummy video device"，CAMSS 节点整体后移一位、helper 按 entity 名解析不受影响；本次自审已把当前默认改为 `/dev/video20`，后续需在新默认上复测）。daemon `--isp-sink loopback --isp-node /dev/video0 --isp-format yuyv 640x480` 持续把 OV13B10 RAW(`bayer10p`) 软件 ISP 成 YUYV 写入 loopback OUTPUT；用 `dd if=/dev/video0 bs=614400 count=3` 从 loopback CAPTURE 侧读回 3 帧，每帧正好 614400 字节、内容真实（distinct 42–46、Y 14–43 变化、meanU≈127 中性色度）——证明了标准 V4L2 CAPTURE 侧可消费软件 ISP 输出，适合作为后续本机 PipeWire/browser `getUserMedia` 验证前提。AE/AWB 在线收敛（ae_luma 16→20、blue gain 1.0→1.17）。
    - **性能现实**：纯 Python 全链约 **640x480 ~1.8–3.4fps、480x360 ~2.9fps、320x240 ~5.5fps**（瓶颈是 per-pixel Python；C/NDK 优化是后续路径，先跑通不追帧率）。
    - **边界保持**：RAW 路径仍真实（daemon 状态 `raw_format=bayer10p`、`kernel_isp=false`），`/dev/video3`(现 entity 仍是 RDI) 不宣告假格式，`vfe480_yc_pp_chain_configured()` 仍关闭，`/dev/video6` 仍 `BA10` 诊断。loopback 是**独立软件节点**，不是内核 ISP。
    - **Phase 2（UVC gadget → 联机 PC UVC consumer）已实机验证（设备 + Windows 枚举 + 主机实际取帧）**：内核侧 `f_uvc` 等 `=y` 随 `#142` 刷入。用户态新增并扩展 `lmi/scripts/lmi-uvc-gadget.c`（format-aware feeder，按 canonical uvc-gadget 处理 UVC PROBE/COMMIT + OUTPUT 队列，从 FIFO 取帧，支持 YUYV 固定帧和 MJPEG 变长帧；NDK r27d 静态交叉编译零 warning）和 helper 的 `UvcSink`（在 live 系统 configfs 建 `lmi_uvc` gadget、临时借用 UDC——保存并解绑当前占用 UDC 的 previous gadget，退出/显式停止时恢复 `lmi_ubuntu`/previous gadget——启 feeder、非阻塞整帧或丢帧写 FIFO，避免 host 断开竞态卡死）。实测：daemon 解绑 `lmi`→建 `lmi_uvc`→绑 UDC（`state=configured function=lmi_uvc`）→`f_uvc` 出 `/dev/video16`（`driver=g_uvc`）→feeder `event CONNECT` + `first frame received`→dmesg `uvc_function_bind()`/`set_alt`，**Windows 端 `Get-PnpDevice` 看到 `UVC Camera` 且 `Status OK`（免驱）**；随后 Windows MediaCapture 实际打开摄像头触发 `STREAMON`，早期成功保存真实 `1280x720` JPEG（302356 bytes）；#515 延迟调优改用 `640x480@15` + 2 个 UVC MMAP buffer，Windows DirectShow/ffmpeg 识别为 `YUY2 640x480 15fps`，30/60 帧抓取实际约 13fps，feeder 日志确认 `streaming ON (640x480 YUYV, 2 buffers)`。退出按 feeder-first/显式 stop 顺序干净恢复 `lmi_ubuntu`/previous gadget。`SSH 在 Wi-Fi (wlp1s0)` 故借 UDC 不影响控制通道。UVC 默认/低延迟验证优先按 USB-HS 带宽选 `640x480@15` + 2 buffers；`720p@10` 可工作但在 Windows 端容易累积明显延迟；当前性能验证按 2080x1170 输入为上限，不继续追更高输入分辨率；压缩 UVC 见第 32/33 条：MJPEG 已实现并完成 640x480、960x540、1280x720 多档验证，H.264 仍只是 Venus-gated scaffold。一条低延迟命令拉起：`python3 /tmp/lmi-camera-web-preview.py --isp-sink uvc --width 2080 --height 1170 --isp-width 640 --isp-height 480 --isp-fps-cap 15 --isp-gamma 2.2 --auto-exposure --isp-uvc-buffers 2`（需 `/tmp/lmi-uvc-gadget` 与默认 `/tmp/lmi-isp`）。
    - **Phase 3（C software-ISP 性能/分辨率）已实机验证并接入 helper 默认路径**：新增 `lmi/scripts/lmi-isp.c`，直接抓已配置好的 RDI RAW10(`pgAA`)、10-bit 解包、black-level、稀疏 gray-world AWB、按输出采样位置 full bilinear demosaic、10-bit→8-bit gamma LUT（默认 2.2，用来改善低亮/宽容度观感）、可选软件 auto-tone 输出增益、YUYV/NV12 pack，并写入 v4l2loopback OUTPUT 或 UVC FIFO；`lmi-camera-web-preview.py --isp-engine c` 现在默认 spawn `/tmp/lmi-isp`，Python 只负责 media route/sensor mode、UVC gadget/feeder 生命周期和参数传递。独立实测 OV13B10 `2080x1170` RAW → `1280x720` YUYV 可达 **29.4fps**、ISP 约 **22ms/frame**；同一 C engine 走 UVC 在 720p@10 完成初验，后续低延迟档 `640x480@15` 设备侧约 14.6–14.9fps、Windows DirectShow/ffmpeg 实际取流约 13fps。AE 现在按 exposure→analogue gain→digital gain 增亮；实测当前场景会出现传感器曝光、模拟增益、数字增益全部打满而 RAW mean 仍低，软件 auto-tone 将用户态输出 JPEG mean 从约 `(26,29,22)` 抬到约 `(94,100,84)`。这只是 userspace software-ISP 的显示/输出增益，不代表内核真 ISP/YUV 或传感器曝光能力已改变。helper 还保留 `--isp-engine python` 回退和 C engine `--isp-dump/--isp-frames` 有限 dump 回归路径；本次补回归实测 `--isp-dump /tmp/lmi-cisp-dump.yuyv --isp-frames 3 --isp-width 640 --isp-height 360 --isp-fps-cap 0` 通过 helper 默认 C engine 正确运行，输出 3 帧共 1382400 bytes，distinct=256、meanY=98、meanU=125、meanV=125。`--isp-sink uvc` 的未压缩路径仍使用 YUYV；MJPEG 通过 `--isp-uvc-format mjpeg` 走变长 JPEG FIFO，不把 RAW 节点声明成 MJPEG/NV12。C 源本地 `cc -Wall -Wextra -Werror -O2 -c` 通过；UVC feeder 的 `logf` 命名也改为 `ilog`，避免与 libm builtin 冲突。

32. #521/#522/#523 为 UVC 压缩输出做了第一版实现和验证。USB-HS 下未压缩 `YUY2 640x480@60` 约 36.9 MB/s，当前 gadget/Windows/HS 链路现实可用档仍以 `640x480@15 + 2 buffers` 为 fallback；为降低带宽和主机端排队延迟，新增 **MJPEG UVC** 路径：`lmi-isp.c` 在 userspace software-ISP 内把 RAW10 解包、AWB、demosaic、gamma/auto-tone 后编码为 baseline JFIF JPEG，并通过带 magic/version/length/sequence 的 FIFO record 交给 `lmi-uvc-gadget.c`；feeder 对 UVC gadget 设置 `V4L2_PIX_FMT_MJPEG`，压缩帧 `QBUF.bytesused` 使用真实 payload size，且在收到第一张合法 JPEG SOI/EOI 帧前不会启动 compressed STREAMON，避免 Windows 先收到无效 MJPEG buffer。`lmi-camera-web-preview.py` 新增 `--isp-uvc-format {yuyv,mjpeg,h264}`、`--isp-uvc-mjpeg-quality`、`--isp-uvc-max-frame-bytes`，并为 configfs 生成 `streaming/mjpeg/mjpg/<height>p` descriptor；默认仍不把 `/dev/video3` 伪装成 MJPEG/YUV，MJPEG 只是独立 UVC 输出。H.264 目前仅保留 framebased descriptor/feeder record scaffold；helper 会明确拒绝 `--isp-uvc-format h264`，因为还没有 `lmi-isp --nv12 -> lmi-venus-h264 -> lmi-uvc-gadget --format h264` 的 Venus bridge，不能宣称 UVC H.264 可用。UVC 生命周期也已按最近 unknown USB device 事故调整：启动前保存 previous gadget，退出/失败时关闭 FIFO writer、停止 feeder、解绑 `lmi_uvc`、删除 configfs、恢复 previous gadget；feeder 增加 signal cleanup 和 ready-file handshake，Python helper 也捕获 SIGINT/SIGTERM 走统一清理。2026-05-30 本地 `py_compile`、native `cc -Wall -Wextra -Werror`、`git diff --check` 和 NDK arm64 static helper 构建已通过；实机以 OV13B10 `2080x1170` RAW 输入、`640x480@30 quality=60 max-frame=262144` 验证，Windows MediaCapture 枚举 `NV12 640x480 30fps` 和 `MJPG 640x480 30fps`，连续拍照 5 张 JPEG 成功（约 189 KB/张），短 MP4 录制成功（约 1.38 MB/7s），设备侧日志确认 `streaming ON/OFF`、`fmt=MJPEG`、`dq/q` 统计和 C ISP 约 29.2–29.3fps。WinRT frame-reader PowerShell 脚本在当前 Windows 环境下 `FrameSources=0`/拿不到 `MediaFrameSource`，记录为该脚本限制，不作为 UVC 功能失败。停止链路后实测无 orphan `lmi-isp`/`lmi-uvc-gadget` 进程、`lmi_uvc` configfs 被删除、`lmi_ubuntu` 恢复到 `a600000.usb`；Windows 侧 USB 串口 COM4 为 OK，VID_1D6B 没有当前 Unknown USB 设备（Camera 类里可残留 stale `UVC Camera` devnode）。#526/#529 针对开启摄像头后 USB 线材/接口接触不良继续加固：默认 `lmi-uvc-gadget` 收到 `UVC_EVENT_DISCONNECT` 会停止 streaming 但保持 feeder/gadget，等待 host 重新枚举；若 UDC 抖动进一步让 gadget fd 出现 `POLLHUP/POLLNVAL` 或 ioctl 返回 `ENODEV/ENXIO/EIO/EPIPE/ESHUTDOWN`，feeder 会退出，Python 父进程在重试上限内重新绑定 `lmi_uvc`、重新发现新的 `g_uvc` video node 并重启 feeder。受控 UDC 断开/重绑已验证恢复到新的 `g_uvc` 节点并重新收到 MJPEG 帧，Windows 侧 `UVC Camera` 恢复 OK；显式停止测试时才解绑临时 `lmi_uvc` 并恢复 `lmi_ubuntu` 到 `a600000.usb`。

33. #532/#533 针对“画面分辨率低、噪点多、不流畅”的反馈补测更高 MJPEG UVC 档位并收紧增益：`960x540@30` 使用 `quality=55`、`max-frame=524288`、`max-digital-gain=2048`、`max-soft-gain=3.0`，启动时 digital gain 会立即夹到 2048 而不是保留 4095，设备侧 `lmi-isp` / feeder 约 29.3–29.4fps，受控 UDC 断开后经 Python rebind/rediscover/restart 恢复并重新收到首个 MJPEG frame，Windows PnP 显示 `UVC Camera` 为 OK。第一版 `1280x720@30` 使用 `quality=50`、`max-frame=786432`、`max-digital-gain=1536`、`max-soft-gain=2.5`，首帧约 94KB，设备侧约 29.4fps、ISP 约 25.8–26.3ms/frame，Windows PnP 仍为 `UVC Camera` OK，显式停止后 `lmi_ubuntu` 恢复到 `a600000.usb`。#540 为改善“720p 看起来不像 720p”的主观细节，把开机档位提高到 `quality=78`、`max-frame=1048576`，并把 MJPEG 下采样从单 Bayer 点采样改为 Bayer-aware box averaging（每个输出 footprint 至少覆盖完整 2x2 CFA quad，避免输出像素继承单个 Bayer 位置的颜色/细节）；中间全 demosaic area-average 版本能改善细节但设备侧只剩约 18–19fps，最终 fastbox 版本恢复到约 27.9–29.3fps。2026-05-31 实机验证 `lmi-uvc-720p-quality-fastbox`：release `Image.gz` 已嵌入新 initramfs（`q78` 存在、`q50` 不存在），运行时 `/run/lmi-camera/lmi-isp` sha 为 `efb285f9c21124de97aa50e9f19950ff28db053d58f14fbd78a27a6faca3a639`，Windows MediaCapture 6 秒录制成功，设备日志仍显示 `/dev/video3` 为 `2080x1170 pgAA`，UVC 为 `1280x720 MJPEG sizeimage=1048576`。这些结果说明设备端软件 ISP/压缩链路已经能在更高输出分辨率下接近 30fps；若 Windows 预览仍感到不流畅，下一步应查 host consumer 实际选择的 MJPG mode、dequeue cadence、USB-HS/Windows 缓冲和应用端延迟，而不是回到内核假 YUV 或继续升高传感器 digital/software gain。

34. #535/#538 新增 **UVC 按需启动**管理和开机支持层：`lmi-uvc-gadget` 通过 `--event-fifo` 把 host `CONNECT`/`DISCONNECT`/`STREAMON`/`STREAMOFF` 事件交给 Python helper；`lmi-camera-web-preview.py --isp-sink uvc --isp-engine c --isp-uvc-demand-start` 会先只创建并绑定 `lmi_uvc`，让 Windows/host 可枚举 `UVC Camera`，但在 host 应用真正 `STREAMON` 前不调用 `configure()`、不打开 `/dev/video3`、不启动 OV13B10/CAMSS RAW stream，也不 spawn `lmi-isp`。收到 `STREAMON` 后才配置 media route 并启动 C software-ISP 写 UVC FIFO；收到 `STREAMOFF` 或 `DISCONNECT` 后按 `--isp-uvc-idle-stop-delay`（默认 2s）延迟停止 `lmi-isp`/RAW，保留 UVC gadget 和 feeder 继续枚举以便下一次打开摄像头可重新启动；若 host 仍在 streaming 时 `lmi-isp` 异常退出，helper 会按 `--isp-uvc-restart-delay` 重新尝试启动 camera/ISP。开机默认枚举不写发行版 rootfs：`sm8250-xiaomi-lmi-initramfs` 构建时可内嵌 `lmi-camera-web-preview.py`、`lmi-isp` 和 `lmi-uvc-gadget`，rootfs 挂载后只在将被 `switch_root` 继承的 `/run` tmpfs 中写入 `/run/lmi-camera/` 与 transient `/run/systemd/system/lmi-camera-uvc.service`，并在同一个 `/run` 里临时 mask 旧的 rootfs `lmi-usb-gadget.service`，同时让 UVC unit `Conflicts=`/`Before=` 该旧服务，避免调试 USB gadget 在同一 UDC 上抢占 UVC；只有 transient UVC unit staging 成功后 `/init` 才解绑早期 ACM gadget，失败时保留 ACM 维护通道而不是留下空 UDC；由该 unit 用 MJPEG `1280x720@30 quality=78 max-frame=1048576` 档常驻拉起 UVC demand-start；helper、FIFO、event FIFO、ready file 都显式放在 `/run/lmi-camera/`，避免使用 rootfs `/tmp` 作为开机运行时状态；禁用可用 kernel cmdline `lmi.uvc=0` 或 `lmi.no_uvc`。因此目标语义是“开机后 host 看到 UVC Camera，但 OV13B10/CAMSS/RAW/ISP 只在 host `STREAMON` 时启动”，同时普通 rootfs 不落盘安装服务。2026-05-31 已用 `builds/lmi-uvc-demand-start/boot-linux-copydown-lmi.img`（sha256 `04d8d193b6463c55400069003d1fb586fffc6ff3a6116b9cd4ab5bdd6039a841`）刷入实机验证：Windows/usbipd 开机枚举 `1d6b:0102 UVC Camera`；SSH 侧 `lmi-camera-uvc.service` active，`/run/systemd/system/lmi-usb-gadget.service -> /dev/null` runtime mask 存在；host 未取流前只有 Python manager 和 `lmi-uvc-gadget`，没有独立 `lmi-isp` 进程。Windows MediaCapture 打开 `UVC Camera` 后枚举 `NV12 1280x720 30fps` 与 `MJPG 1280x720 30fps`，拍照触发 `STREAMON`，日志显示 `/run/lmi-camera/lmi-isp` 才启动并打开 `/dev/video3` `2080x1170` `pgAA` RAW、写出 `1280x720` MJPEG FIFO，feeder 收到首帧后 `streaming ON`；应用关闭触发 `STREAMOFF`，2s idle grace 后 `lmi-isp`/RAW 停止，UVC gadget 保持枚举并回到只剩 manager+feeder 的空闲状态。临时 Windows probe 和测试照片已清理。临时手动验证示例仍可用：`python3 /tmp/lmi-camera-web-preview.py --isp-sink uvc --isp-engine c --isp-uvc-demand-start --isp-uvc-format mjpeg --width 2080 --height 1170 --isp-width 1280 --isp-height 720 --isp-fps-cap 30 --isp-uvc-mjpeg-quality 78 --isp-uvc-max-frame-bytes 1048576 --auto-exposure --isp-max-digital-gain 1536 --isp-max-soft-gain 2.5`。

## Validation commands

Local build checks:

```sh
git diff --check
python3 -m py_compile lmi/scripts/lmi-camera-web-preview.py
make O=out/m1-release ARCH=arm64 LLVM=1 drivers/media/platform/qcom/camss/
```

On-device RAW camera checks:

```sh
dmesg | grep -Ei 'camss|cci|ov13|camera|csiphy|csid|vfe'
ls /dev/media* /dev/v4l-subdev* /dev/video*
python3 lmi/scripts/lmi-camera-web-preview.py --list-modes --media /dev/media0
python3 lmi/scripts/lmi-camera-web-preview.py --controls --mode-index 5 --media /dev/media0
python3 lmi/scripts/lmi-camera-web-preview.py --discover --mode-index 5 --media /dev/media0
python3 lmi/scripts/lmi-camera-web-preview.py --once --mode-index 5 --auto-exposure --ae-mode video --media /dev/media0 --output /tmp/lmi-camera-test.png --raw-output /tmp/lmi-camera-test.raw --nv12-output /tmp/lmi-camera-test.nv12 --metadata-output /tmp/lmi-camera-test.json
```

Venus checks after booting a Venus-enabled image:

```sh
dmesg | grep -Ei 'venus|vidc|hfi|video-codec|firmware'
find /dev -maxdepth 1 -name 'video*' -print
# Confirm a mem2mem encoder node appears, then encode a generated NV12/YUV test pattern.
# #467 also validated: OV13B10 RAW pgAA -> helper --nv12-output software NV12 -> qcom-venus-encoder H.264.
# Firmware must be staged from ignored/local blobs only and cleaned afterward.
```

Experimental PIX diagnostics, only for builds intentionally exposing `/dev/video6`:

```sh
python3 lmi/scripts/lmi-camera-web-preview.py \
  --route lmi-ov13b10 \
  --vfe msm_vfe1_pix \
  --video-entity msm_vfe1_video3 \
  --csid-source-pad 4 \
  --mode-index 4 \
  --discover --media /dev/media0
```

Do not document experimental PIX output as supported unless a real frame is dequeued and payload is validated.
