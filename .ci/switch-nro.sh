#!/usr/bin/env bash
# Builds dekopon.nro inside the NXVK toolchain image (devkitpro/devkita64 + Mesa build deps).
# Run from the repo root:  docker run --rm -v "$PWD:/work" -w /work nxvk bash .ci/switch-nro.sh
set -euxo pipefail

ROOT="$(pwd)"
JOBS="$(nproc)"
git config --global --add safe.directory '*'

# 1. devkitPro packages, fully up to date.
dkp-pacman -Syu --noconfirm
dkp-pacman -S --noconfirm --needed \
    switch-dev switch-freetype switch-bzip2 switch-libpng switch-zlib switch-curl \
    switch-ntfs-3g switch-lwext4

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
(
    cd externals/nxvk
    bash switch/build/build-native-tools.sh
    bash switch/build/configure-mesa.sh
    ninja -k0 -C switch/build/cross src/nouveau/vulkan/libvulkan_nouveau.so || true
    test -f switch/build/cross/src/nouveau/vulkan/libnvk.a
)

# 4. Dekopon itself.
cmake -S . -B build/switch \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build/switch --target citra_switch_nro -j"$JOBS"

mkdir -p "$ROOT/artifacts"
cp build/switch/src/citra_switch/dekopon.nro "$ROOT/artifacts/"
ls -la "$ROOT/artifacts"
