#!/usr/bin/env bash
set -euo pipefail

REPO_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
KERNEL_PROFILE=${KERNEL_PROFILE:-debug}
JOBS=${JOBS:-$(nproc)}
DTS="arch/arm64/boot/dts/qcom/sm8250-xiaomi-lmi.dts"
BASE_CONFIG_FRAGMENT="$REPO_DIR/lmi/configs/m1.config"

case "$KERNEL_PROFILE" in
  debug)
    OUT_DIR=${OUT_DIR:-"$REPO_DIR/out/m1"}
    CONFIG_FRAGMENTS=("$BASE_CONFIG_FRAGMENT")
    ;;
  release)
    OUT_DIR=${OUT_DIR:-"$REPO_DIR/out/m1-release"}
    CONFIG_FRAGMENTS=("$BASE_CONFIG_FRAGMENT" "$REPO_DIR/lmi/configs/m1-release.config")
    ;;
  *)
    printf 'unknown KERNEL_PROFILE: %s\n' "$KERNEL_PROFILE" >&2
    exit 2
    ;;
esac

DTB="$OUT_DIR/arch/arm64/boot/dts/qcom/sm8250-xiaomi-lmi.dtb"

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

for CONFIG_FRAGMENT in "${CONFIG_FRAGMENTS[@]}"; do
  if [ ! -f "$CONFIG_FRAGMENT" ]; then
    printf 'missing config fragment: %s\n' "$CONFIG_FRAGMENT" >&2
    exit 1
  fi
done

mkdir -p "$OUT_DIR"

printf 'profile=%s\n' "$KERNEL_PROFILE"
printf 'out=%s\n' "$OUT_DIR"

make -C "$REPO_DIR" O="$OUT_DIR" ARCH=arm64 LLVM=1 defconfig

if [ -x "$REPO_DIR/scripts/kconfig/merge_config.sh" ]; then
  "$REPO_DIR/scripts/kconfig/merge_config.sh" -m -O "$OUT_DIR" "$OUT_DIR/.config" "${CONFIG_FRAGMENTS[@]}"
else
  for CONFIG_FRAGMENT in "${CONFIG_FRAGMENTS[@]}"; do
    cat "$CONFIG_FRAGMENT" >>"$OUT_DIR/.config"
  done
fi

make -C "$REPO_DIR" O="$OUT_DIR" ARCH=arm64 LLVM=1 olddefconfig
make -C "$REPO_DIR" O="$OUT_DIR" ARCH=arm64 LLVM=1 -j"$JOBS" Image.gz qcom/sm8250-xiaomi-lmi.dtb

if [ ! -f "$DTB" ]; then
  printf 'expected DTB was not produced: %s\n' "$DTB" >&2
  exit 1
fi

printf 'kernel=%s\n' "$OUT_DIR/arch/arm64/boot/Image.gz"
printf 'dtb=%s\n' "$DTB"
