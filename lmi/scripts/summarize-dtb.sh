#!/usr/bin/env bash
set -euo pipefail

REPO_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
INPUT=${1:-"$REPO_DIR/arch/arm64/boot/dts/qcom/sm8250-xiaomi-lmi.dts"}
VERIFY=${VERIFY_RESERVED_MEMORY:-"/home/ccc007/Android/Kernel/lmi/sm8250-xiaomi-lmi-boot/scripts/verify-reserved-memory.py"}
TMP_DIR=

cleanup() {
  if [ -n "$TMP_DIR" ]; then
    rm -rf "$TMP_DIR"
  fi
}
trap cleanup EXIT

if [ ! -f "$INPUT" ]; then
  printf 'missing input: %s\n' "$INPUT" >&2
  exit 1
fi

SOURCE="$INPUT"
case "$INPUT" in
  *.dtb)
    if ! command -v dtc >/dev/null 2>&1; then
      printf 'dtc is required to summarize a DTB\n' >&2
      exit 1
    fi
    TMP_DIR=$(mktemp -d)
    SOURCE="$TMP_DIR/input.dts"
    dtc -I dtb -O dts -o "$SOURCE" "$INPUT"
    ;;
esac

python3 - "$SOURCE" <<'PY'
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(errors="replace")
text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
text = re.sub(r"//.*", "", text)

print(f"source={path}")

for name in ("model", "bootargs"):
    m = re.search(rf"\b{name}\s*=\s*\"([^\"]+)\"\s*;", text)
    if m:
        print(f"{name}={m.group(1)}")

m = re.search(r"\bcompatible\s*=\s*((?:\"[^\"]+\"\s*,?\s*)+)\s*;", text)
if m:
    values = re.findall(r"\"([^\"]+)\"", m.group(1))
    print("compatible=" + ",".join(values))

mem = re.search(r"memory@[^{]+\{(?P<body>.*?)\};", text, flags=re.S)
if mem:
    reg = re.search(r"\breg\s*=\s*<([^>]+)>\s*;", mem.group("body"))
    if reg:
        print("memory.reg=<" + " ".join(reg.group(1).split()) + ">")

reserved = ""
for pattern in (r"reserved-memory\s*\{", r"&\{/reserved-memory\}\s*\{"):
    m = re.search(pattern, text)
    if not m:
        continue
    depth = 1
    pos = m.end()
    start = pos
    while pos < len(text) and depth:
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
        pos += 1
    reserved += "\n" + text[start : pos - 1]

print("reserved-memory.fixed:")
for node in re.finditer(r"(?:(?P<label>[A-Za-z_][\w]*):\s*)?(?P<name>[A-Za-z0-9_,.+-]+)(?:@[0-9a-fA-F]+)?\s*\{(?P<body>[^{}]*)\};", reserved, flags=re.S):
    body = node.group("body")
    reg = re.search(r"\breg\s*=\s*<([^>]+)>\s*;", body)
    if not reg:
        continue
    nums = []
    for raw in reg.group(1).split():
        try:
            nums.append(int(raw, 0))
        except ValueError:
            nums = []
            break
    if len(nums) < 4:
        continue
    addr = (nums[0] << 32) | nums[1]
    size = (nums[2] << 32) | nums[3]
    label = node.group("label") or node.group("name")
    print(f"- {label}: 0x{addr:x}+0x{size:x}")
PY

if [ -x "$VERIFY" ]; then
  VERIFY_ARGS=()
  case "$INPUT" in
    *.dts) VERIFY_ARGS+=(--allow-missing) ;;
  esac
  "$VERIFY" "$SOURCE" "${VERIFY_ARGS[@]}"
fi
