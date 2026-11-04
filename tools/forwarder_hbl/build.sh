#!/usr/bin/env bash
# Builds the HOME Menu forwarder's loader (Sphaira's nx-hbloader variant, ISC: LICENSE.md) into
# <out>/main and <out>/main.npdm, which EmuSwitch puts in its romfs as forwarder/.
# Needs devkitA64, libnx and switch-tools:  bash tools/forwarder_hbl/build.sh <out dir>
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$1"
WORK="$(mktemp -d)"
mkdir -p "$OUT"

CC="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc"
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"

"$CC" $ARCH -std=gnu11 -O2 -Wall -ffunction-sections -D__SWITCH__ -DVERSION='"1.0.0"' \
    -I"$DEVKITPRO/libnx/include" -c "$HERE/source/main.c" -o "$WORK/main.o"
"$CC" $ARCH -x assembler-with-cpp -c "$HERE/source/trampoline.s" -o "$WORK/trampoline.o"
"$CC" -specs="$DEVKITPRO/libnx/switch.specs" $ARCH -Wl,-wrap,exit \
    "$WORK/main.o" "$WORK/trampoline.o" -L"$DEVKITPRO/libnx/lib" -lnx -o "$WORK/hbl.elf"
"$DEVKITPRO/tools/bin/elf2nso" "$WORK/hbl.elf" "$OUT/main"
"$DEVKITPRO/tools/bin/npdmtool" "$HERE/hbl.json" "$OUT/main.npdm"
ls -la "$OUT"
