# Vendored v4l2loopback (for the lmi software-ISP camera node)

`v4l2loopback.c`, `v4l2loopback.h` and `v4l2loopback_formats.h` here are based on an upstream **v4l2loopback** snapshot with one lmi-local default-device patch:

- Upstream: https://github.com/umlaeute/v4l2loopback
- Branch:   `main` (snapshot fetched 2026-05-30)
- Version:  0.15.3 (`V4L2LOOPBACK_VERSION_MAJOR/MINOR/BUGFIX` in `v4l2loopback.h`)
- License:  GPL-2.0 (`MODULE_LICENSE("GPL")`), compatible with the kernel tree.
- Local patch: because this driver is built in, the upstream auto-numbered default device could appear before CAMSS and shift the validated RAW camera nodes. lmi therefore defaults to one loopback device on `/dev/video20` (`devices=1`, `video_nr[0]=20`). Boot/module parameters can still override this.

It is vendored in-tree and built **statically (`CONFIG_V4L2LOOPBACK=y`)** because the lmi build produces only `Image.gz` and ships no loadable modules, so a `=m` driver would be dead at runtime.

## Why it is here

The lmi mainline kernel keeps `/dev/video3` as a truthful RAW10 Bayer RDI node (no in-kernel ISP — the VFE480 Y/C path stays gated off). To provide a normal colour camera node, `lmi/scripts/lmi-camera-web-preview.py --isp-sink loopback --isp-node /dev/video20` runs a userspace software ISP (demosaic + AE/AWB/CCM/gamma) over the RAW frames and writes YUYV/NV12 into the v4l2loopback OUTPUT side; the loopback CAPTURE side is a standard V4L2 node suitable for local V4L2 consumers and for future PipeWire/browser `getUserMedia` validation.

## Compatibility note (Linux 7.1)

Upstream master already carries `LINUX_VERSION_CODE` guards up to 6.18; on this 7.1 tree all of them resolve to the newest code paths, which match the tree's current APIs (native `timer_delete_sync`, 2-arg `v4l2_fh_add(fh, filp)`, `VFL_TYPE_VIDEO`). The driver does not use videobuf2. Keep any future kernel-version shims contained in these vendored files, and document lmi-local deviations here.
