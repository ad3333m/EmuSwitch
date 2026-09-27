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

# 4. Dekopon itself.
cmake -S . -B build/switch \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build/switch --target citra_switch_nro -j"$JOBS"

mkdir -p "$ROOT/artifacts"
cp build/switch/src/citra_switch/dekopon.nro "$ROOT/artifacts/"
ls -la "$ROOT/artifacts"
