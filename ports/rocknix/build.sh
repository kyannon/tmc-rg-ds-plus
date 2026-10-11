#!/bin/bash
# Cross-builds the ROCKNIX (aarch64 Linux) dual-screen port and packages it.
#
#   ports/rocknix/build.sh            -> dist/rocknix/MinishCapDS-rocknix-aarch64.zip
#
# Needs on the build host (x86_64 Linux): git, cmake, ninja, xmake, zip,
# python3 and network access to GitHub. Everything else is fetched here:
#   - an aarch64 GCC toolchain with glibc 2.27 (tttapa/toolchains), so the
#     binary runs on any newer glibc, ROCKNIX's included
#   - SDL 3.4.10 built only as a link stub: the game uses the device's own
#     libSDL3.so.0, which ROCKNIX builds for its Wayland/GPU stack
#   - Dear ImGui v1.92.7, compiled straight into the game
# Override locations with XT_ROOT / SDL3_PREFIX / IMGUI_DIR / WORK.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="${WORK:-$REPO/build/rocknix-work}"
XT_ROOT="${XT_ROOT:-$WORK/x-tools}"
XT_TRIPLE=aarch64-rpi3-linux-gnu
XT="$XT_ROOT/$XT_TRIPLE"
SDL3_PREFIX="${SDL3_PREFIX:-$WORK/sdl3-aarch64}"
IMGUI_DIR="${IMGUI_DIR:-$REPO/libs/imgui}"
OUT="$REPO/dist/rocknix"
mkdir -p "$WORK" "$OUT"

if [ ! -x "$XT/bin/$XT_TRIPLE-gcc" ]; then
  echo "== fetching aarch64 toolchain"
  curl -fL -o "$WORK/xt.tar.xz" \
    "https://github.com/tttapa/toolchains/releases/download/1.4.0/x-tools-$XT_TRIPLE-gcc14.tar.xz"
  mkdir -p "$XT_ROOT/.."
  tar -C "$(dirname "$XT_ROOT")" -xJf "$WORK/xt.tar.xz"
fi

if [ ! -f "$SDL3_PREFIX/include/SDL3/SDL.h" ]; then
  echo "== building SDL 3.4.10 link stub"
  [ -d "$WORK/SDL" ] || git clone --depth 1 -b release-3.4.10 https://github.com/libsdl-org/SDL "$WORK/SDL"
  cmake -S "$WORK/SDL" -B "$WORK/SDL-build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$XT_ROOT/$XT_TRIPLE.toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$SDL3_PREFIX" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF \
    -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_VULKAN=OFF \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=ON -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF \
    -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_HIDAPI_LIBUSB=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF
  ninja -C "$WORK/SDL-build" install
fi

if [ ! -f "$IMGUI_DIR/imgui.cpp" ]; then
  echo "== fetching Dear ImGui v1.92.7"
  git clone --depth 1 -b v1.92.7 https://github.com/ocornut/imgui "$IMGUI_DIR"
fi

if [ ! -d "$REPO/libs/VirtuaAPU/include" ]; then
  git -C "$REPO" submodule update --init --depth 1 libs/VirtuaAPU
fi

echo "== configuring + building tmc_pc (aarch64, dual_screen)"
cd "$REPO"
export SDL3_PREFIX IMGUI_DIR
[ "$(id -u)" = 0 ] && export XMAKE_ROOT=y
xmake f -y -p linux -a arm64 --sdk="$XT" --cross="$XT_TRIPLE-" \
  --game_version=USA --gpu_renderer=n --widescreen_width=384 --dual_screen=y --pc_avx2=n -m release
xmake build -y tmc_pc

echo "== packaging"
STAGE="$WORK/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/minishcapds/libs.aarch64"
cp "$REPO/ports/rocknix/Minish Cap DS.sh" "$STAGE/"
chmod +x "$STAGE/Minish Cap DS.sh"
"$XT/bin/$XT_TRIPLE-strip" -o "$STAGE/minishcapds/tmc_pc" "$REPO/build/pc/tmc_pc"
cp -L "$XT/$XT_TRIPLE/sysroot/lib/libgomp.so.1" "$STAGE/minishcapds/libs.aarch64/"
# Function table for the TMC_SAMPLE profiler (offsets in the PIE, sorted).
"$XT/bin/$XT_TRIPLE-nm" -C --defined-only -n "$REPO/build/pc/tmc_pc" \
  | awk '$2 ~ /^[tTwW]$/ { name=$3; for (i=4;i<=NF;i++) name=name" "$i; sub(/^0+/, "", $1); print $1, substr(name,1,150) }' \
  > "$STAGE/minishcapds/tmc_pc.syms"
cp "$REPO/assets/sounds.json" "$STAGE/minishcapds/"
cp "$REPO/ports/rocknix/README.md" "$STAGE/minishcapds/README.md"
cp "$REPO/LICENSE" "$REPO/THIRD-PARTY-LICENSES.md" "$STAGE/minishcapds/"
rm -f "$OUT/MinishCapDS-rocknix-aarch64.zip"
(cd "$STAGE" && zip -qr "$OUT/MinishCapDS-rocknix-aarch64.zip" "Minish Cap DS.sh" minishcapds)
echo "== $OUT/MinishCapDS-rocknix-aarch64.zip"
