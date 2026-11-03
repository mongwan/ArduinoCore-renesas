#!/usr/bin/env bash
# Apply (or revert) the UWB + Wi-Fi coexistence patch to an installed
# "Arduino Renesas Portenta Boards" core (renesas_portenta).
#
#   ./apply_core_patch.sh                 # auto-detect the newest installed version
#   ./apply_core_patch.sh /path/to/core   # explicit core folder (has platform.txt)
#   ./apply_core_patch.sh --revert [/path/to/core]
#
# Applies every patches/NNNN-*.patch in order (0001 coexistence, 0002 UDP
# pbuf leak fix); --revert removes them in reverse order.
# Tested against core 1.5.3 and 1.6.0. Close the Arduino IDE first.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
patches=( $(ls "$here"/../patches/[0-9][0-9][0-9][0-9]-*.patch | sort) )
revert=0
if [[ "${1:-}" == "--revert" ]]; then
  revert=1
  shift
fi

find_core() {
  local roots=(
    "$HOME/.arduino15/packages/arduino/hardware/renesas_portenta"
    "$HOME/Library/Arduino15/packages/arduino/hardware/renesas_portenta"
  )
  if [[ -n "${LOCALAPPDATA:-}" ]]; then
    roots+=("$(cygpath -u "$LOCALAPPDATA" 2>/dev/null || echo "$LOCALAPPDATA")/Arduino15/packages/arduino/hardware/renesas_portenta")
  fi
  for r in "${roots[@]}"; do
    if [[ -d "$r" ]]; then
      ls -d "$r"/*/ 2>/dev/null | sort -V | tail -n1
      return
    fi
  done
}

core="${1:-$(find_core)}"
core="${core%/}"
if [[ -z "$core" || ! -f "$core/platform.txt" ]]; then
  echo "core folder not found; pass it explicitly (the folder that holds platform.txt)" >&2
  exit 1
fi
echo "core:  $core ($(grep -m1 '^version=' "$core/platform.txt"))"

if (( revert )); then
  for (( i=${#patches[@]}-1; i>=0; i-- )); do
    p="${patches[$i]}"
    if patch -d "$core" -p1 -R --dry-run --quiet < "$p" >/dev/null 2>&1; then
      patch -d "$core" -p1 -R --quiet < "$p"
      echo "reverted  $(basename "$p")"
    else
      echo "not applied, skipped  $(basename "$p")"
    fi
  done
  exit 0
fi

backup=""
for p in "${patches[@]}"; do
  if patch -d "$core" -p1 -R --dry-run --quiet < "$p" >/dev/null 2>&1; then
    echo "already applied  $(basename "$p")"
    continue
  fi
  if [[ -z "$backup" ]]; then
    backup="$core/.coex-backup-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$backup"
    for d in SPI ESPhost lwIpWrapper Arduino_FreeRTOS; do
      cp -a "$core/libraries/$d" "$backup/"
    done
    echo "backup: $backup"
  fi
  patch -d "$core" -p1 --dry-run --quiet < "$p"
  patch -d "$core" -p1 --quiet < "$p"
  echo "applied  $(basename "$p")"
done
echo "done. Rebuild your sketch (the IDE recompiles the core libraries)."
