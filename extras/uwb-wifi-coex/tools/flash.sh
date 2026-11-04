#!/usr/bin/env bash
# Upload a .bin to a Portenta C33 with the dfu-util that the Arduino board
# package installs, without the IDE.
#
#   tools/flash.sh build/CoexAnchorUdp.ino.bin               # one board attached
#   tools/flash.sh build/CoexTag.ino.bin 42071E9435373539...  # pick a board by USB serial
#   tools/flash.sh --list                                     # show attached boards and serials
#
# With several boards attached, pass the USB serial number (dfu-util -S);
# "--list" prints it for each board. DFU_UTIL=/path/to/dfu-util overrides the
# search. Sketches that call UWBWiFiCoex.loopTick() switch to the bootloader
# by themselves; a hung sketch needs a double tap on the reset button first.
set -euo pipefail

find_dfu_util() {
  if [[ -n "${DFU_UTIL:-}" ]]; then
    echo "$DFU_UTIL"
    return
  fi
  local roots=(
    "$HOME/.arduino15/packages/arduino/tools/dfu-util"
    "$HOME/Library/Arduino15/packages/arduino/tools/dfu-util"
  )
  if [[ -n "${LOCALAPPDATA:-}" ]]; then
    roots+=("$(cygpath -u "$LOCALAPPDATA" 2>/dev/null || echo "$LOCALAPPDATA")/Arduino15/packages/arduino/tools/dfu-util")
  fi
  for r in "${roots[@]}"; do
    if [[ -d "$r" ]]; then
      local v
      v="$(ls -d "$r"/*/ 2>/dev/null | sort -V | tail -n1)"
      for exe in "${v}dfu-util" "${v}dfu-util.exe"; do
        if [[ -x "$exe" ]]; then
          echo "$exe"
          return
        fi
      done
    fi
  done
  command -v dfu-util || true
}

dfu="$(find_dfu_util)"
if [[ -z "$dfu" ]]; then
  echo "dfu-util not found: install the Arduino Renesas Portenta Boards package or set DFU_UTIL" >&2
  exit 1
fi

# 0x0068 = bootloader, 0x0368 = sketch running (DFU runtime interface)
devices="0x2341:0x0068,:0x0368"

if [[ "${1:-}" == "--list" ]]; then
  "$dfu" -l 2>/dev/null | grep -E "2341:(0068|0368)" || echo "no Portenta C33 found"
  exit 0
fi

bin="${1:?usage: flash.sh file.bin [usb-serial] | --list}"
serial="${2:-}"
args=(--device "$devices" -D "$bin" -a0 -Q)
if [[ -n "$serial" ]]; then
  args+=(-S "$serial")
fi
"$dfu" "${args[@]}" 2>&1 | tail -n 3
