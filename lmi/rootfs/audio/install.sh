#!/bin/sh
# Install lmi PipeWire-Pulse audio helper into the current rootfs.

set -eu

PREFIX=${PREFIX:-/}
SRC_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN_SRC=$SRC_DIR/usr/local/bin/lmi-audio-pulse-setup
AUTO_SRC=$SRC_DIR/etc/xdg/autostart/lmi-audio-pulse-setup.desktop
BIN_DST=$PREFIX/usr/local/bin/lmi-audio-pulse-setup
AUTO_DST=$PREFIX/etc/xdg/autostart/lmi-audio-pulse-setup.desktop

if [ "$(id -u)" -ne 0 ] && [ "$PREFIX" = "/" ]; then
	echo "please run as root, or set PREFIX to a writable staging root" >&2
	exit 1
fi

install -d -m 0755 "$(dirname "$BIN_DST")" "$(dirname "$AUTO_DST")"
install -m 0755 "$BIN_SRC" "$BIN_DST"
install -m 0644 "$AUTO_SRC" "$AUTO_DST"

cat <<EOF
Installed lmi audio helper:
  $BIN_DST
  $AUTO_DST

Start it in a running graphical/PipeWire-Pulse session with:
  export XDG_RUNTIME_DIR=/run/user/0
  export PULSE_SERVER=unix:\$XDG_RUNTIME_DIR/pulse/native
  $BIN_DST --setup
  $BIN_DST --status
EOF
