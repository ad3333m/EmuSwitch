#!/usr/bin/env bash
# Builds dekopon.nro inside the NXVK toolchain image (devkitpro/devkita64 + Mesa build deps).
# Run from the repo root:  docker run --rm -v "$PWD:/work" -w /work nxvk bash .ci/switch-nro.sh
set -euxo pipefail

ROOT="$(pwd)"
JOBS="$(nproc)"
git config --global --add safe.directory '*'

# 1. devkitPro packages. The image already carries the Switch portlibs; pkg.devkitpro.org
#    sometimes refuses CI runners (403), so updating is best-effort.
if dkp-pacman -Syu --noconfirm; then
    dkp-pacman -S --noconfirm --needed \
        switch-dev switch-freetype switch-bzip2 switch-libpng switch-zlib switch-curl \
        switch-ntfs-3g switch-lwext4 || true
else
    echo "::warning::pkg.devkitpro.org unreachable, building with the image's packages"
fi
for lib in libfreetype.a libbz2.a libpng.a libz.a libcurl.a; do
    test -f "$DEVKITPRO/portlibs/switch/lib/$lib" || { echo "missing portlib $lib"; exit 1; }
done

# 2. libnx from master. The last tagged release (4.11.1) predates the 22.x firmware fixes.
LIBNX_REF="${LIBNX_REF:-master}"
rm -rf /tmp/libnx
git clone --depth 1 -b "$LIBNX_REF" https://github.com/switchbrew/libnx.git /tmp/libnx
make -C /tmp/libnx -j"$JOBS"
make -C /tmp/libnx install
git -C /tmp/libnx log -1 --format='libnx %H %cd'

# 3. NXVK (NVK Vulkan driver). Only the static archives are needed; the final .so link fails by design.
NXVK_REF="${NXVK_REF:-switch}"
if [ ! -d externals/nxvk/.git ]; then
    rm -rf externals/nxvk
    git clone --depth 1 -b "$NXVK_REF" https://github.com/PalindromicBreadLoaf/nxvk.git externals/nxvk
fi
# NXVK's rust cross-file and rustc wrapper expect its tree at /work/switch.
ln -sfn "$ROOT/externals/nxvk/switch" /work/switch
# Its Makefile chains native tools -> Rust std sysroot -> configure -> driver archives.
# CONTAINER= runs each step directly, since we're already inside the toolchain image.
make -C externals/nxvk driver CONTAINER=
test -f externals/nxvk/switch/build/cross/src/nouveau/vulkan/libnvk.a

# 4. EmuSwitch's other emulators, bundled in the romfs as emus/<system>.nro.
EMUS="$ROOT/dist/emuswitch-romfs/emus"
rm -rf "$ROOT/dist/emuswitch-romfs"
mkdir -p "$EMUS"
# --retry-all-errors: a dropped connection (curl exit 56) isn't retried by --retry alone, and the
# libretro buildbot drops one now and then.
fetch() { curl -fL --retry 5 --retry-all-errors --retry-delay 3 -o "$2" "$1"; }
fetch https://github.com/PalindromicBreadLoaf/ARMSX2-NX/releases/download/v3.0.0/armsx2nx.nro "$EMUS/ps2.nro"
fetch https://github.com/NaGaa95/Cemu-nx/releases/download/1.2.0/cemu.nro "$EMUS/wiiu.nro"
core() {
    fetch "https://buildbot.libretro.com/nightly/nintendo/switch/libnx/latest/$1_libretro_libnx.nro.zip" /tmp/core.zip
    python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extract(sys.argv[2], '/tmp')" /tmp/core.zip "$1_libretro_libnx.nro"
    mv "/tmp/$1_libretro_libnx.nro" "$EMUS/$2.nro"
}
core desmume ds
core mgba gba
core gambatte gb
core nestopia nes
core snes9x snes
core mupen64plus_next n64
core pcsx_rearmed ps1
core ppsspp psp
ls -la "$EMUS"

# Console logos for the Home screen's section headers.
mkdir -p "$ROOT/dist/emuswitch-romfs/logos"
cp "$ROOT"/src/citra_switch/assets/logos/*.png "$ROOT/dist/emuswitch-romfs/logos/"

# The menu's font: Inter (SIL Open Font License), in the romfs next to the emulators.
FONTS="$ROOT/dist/emuswitch-romfs/fonts"
mkdir -p "$FONTS"
fetch https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip /tmp/inter.zip
python3 - /tmp/inter.zip "$FONTS" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
want = {"Inter-Medium.ttf", "Inter-Bold.ttf", "Inter-BlackItalic.ttf", "LICENSE.txt"}
for n in z.namelist():
    base = n.split("/")[-1]
    if base in want and ("extras/ttf/" in n or base == "LICENSE.txt"):
        open(sys.argv[2] + "/" + base, "wb").write(z.read(n))
        want.discard(base)
missing = want - {"LICENSE.txt"}
if missing:
    sys.exit("missing fonts: %s" % missing)
PY
ls -la "$FONTS"

# 5. EmuSwitch itself (Dekopon's frontend running the 3DS engine in-process).
cmake -S . -B build/switch \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DEMUSWITCH_ROMFS="$ROOT/dist/emuswitch-romfs"
cmake --build build/switch --target citra_switch_nro -j"$JOBS"

mkdir -p "$ROOT/artifacts"
cp build/switch/src/citra_switch/dekopon.nro "$ROOT/artifacts/EmuSwitch.nro"
# The unstripped ELF, kept as a workflow artifact (not in releases) so the offsets in a crash
# report (sdmc:/switch/dekopon/log/crash.txt) can be matched to functions.
cp build/switch/src/citra_switch/citra_switch.elf "$ROOT/artifacts/EmuSwitch.elf" ||
    echo "::warning::citra_switch.elf not found, no symbols kept"
ls -la "$ROOT/artifacts"
