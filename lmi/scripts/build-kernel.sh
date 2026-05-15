#!/usr/bin/env bash
set -euo pipefail

REPO_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT_DIR=${OUT_DIR:-"$REPO_DIR/out/m1"}
JOBS=${JOBS:-$(nproc)}
DTS="arch/arm64/boot/dts/qcom/sm8250-xiaomi-lmi.dts"
DTB="$OUT_DIR/arch/arm64/boot/dts/qcom/sm8250-xiaomi-lmi.dtb"
CONFIG_FRAGMENT="$REPO_DIR/lmi/configs/m1.config"

if [ ! -f "$REPO_DIR/Makefile" ] || [ ! -d "$REPO_DIR/scripts/kconfig" ]; then
  cat >&2 <<'EOF'
Populate this repository with the pinned latest LTS Linux source before building.
Keep the lmi files in place, then rerun lmi/scripts/build-kernel.sh.
EOF
  exit 2
fi

if [ ! -f "$REPO_DIR/$DTS" ]; then
  printf 'missing DTS: %s\n' "$REPO_DIR/$DTS" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

make -C "$REPO_DIR" O="$OUT_DIR" ARCH=arm64 LLVM=1 defconfig

if [ -x "$REPO_DIR/scripts/kconfig/merge_config.sh" ]; then
  "$REPO_DIR/scripts/kconfig/merge_config.sh" -O "$OUT_DIR" "$OUT_DIR/.config" "$CONFIG_FRAGMENT"
else
  cat "$CONFIG_FRAGMENT" >>"$OUT_DIR/.config"
fi

make -C "$REPO_DIR" O="$OUT_DIR" ARCH=arm64 LLVM=1 olddefconfig
make -C "$REPO_DIR" O="$OUT_DIR" ARCH=arm64 LLVM=1 -j"$JOBS" Image.gz qcom/sm8250-xiaomi-lmi.dtb

if [ ! -f "$DTB" ]; then
  printf 'expected DTB was not produced: %s\n' "$DTB" >&2
  exit 1
fi

printf 'kernel=%s\n' "$OUT_DIR/arch/arm64/boot/Image.gz"
printf 'dtb=%s\n' "$DTB"
