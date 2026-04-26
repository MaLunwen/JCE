#!/usr/bin/env bash
# ================================================================
# build-linux-arm64.sh -- Build JCE for Linux aarch64
# Usage: build-linux-arm64.sh [--clean]
# Output: build/desktop/linux-arm64/CagedKingdom
#
# System dependencies (Ubuntu/Debian):
#   sudo apt install libx11-dev libxrandr-dev libxcursor-dev libxi-dev \
#       libxinerama-dev libxext-dev libxfixes-dev libxss-dev \
#       libgl-dev libudev-dev libdbus-1-dev \
#       ninja-build cmake pkg-config
# ================================================================
source "$(dirname "$0")/../lib/jce_common.sh"
PROFILE="conan/profiles/linux-arm64"
BUILD_PROFILE="conan/profiles/linux-x64"
CONAN_DIR="build/desktop/linux-arm64-conan"
BUILD_DIR="build/desktop/linux-arm64"
TOOLCHAIN="$CONAN_DIR/build/Release/generators/conan_toolchain.cmake"
HOST_PAK="build/host/jce_pak"

# -- Handle --clean flag --
if [[ "${1:-}" == "--clean" ]]; then
    echo "=== Cleaning build directory ==="
    rm -rf "$BUILD_DIR" "$CONAN_DIR"
    echo "  Done"
fi

# -- Step 1: Ensure host jce_pak exists --
echo "=== Step 1: Resolve host jce_pak ==="
if [[ ! -f "$HOST_PAK" ]]; then
    echo "  Host jce_pak not found - building..."
    bash "$SCRIPT_DIR/build-host-tools.sh"
fi
if [[ ! -f "$HOST_PAK" ]]; then
    echo "ERROR: jce_pak still not found after host build"
    exit 1
fi
HOST_PAK="$(cd "$(dirname "$HOST_PAK")" && pwd)/$(basename "$HOST_PAK")"
echo "  $HOST_PAK"

# -- Step 2: Locate host shaderc --
echo "=== Step 2: Locate host shaderc ==="
HOST_SHADERC=""
_BGFX_DATA="build/host-conan/build/Release/generators/bgfx-release-armv8-data.cmake"
if [[ ! -f "$_BGFX_DATA" ]]; then
    _BGFX_DATA="build/host-conan/build/Release/generators/bgfx-release-x86_64-data.cmake"
fi
if [[ -f "$_BGFX_DATA" ]]; then
    _LINE=$(grep '^set(bgfx_PACKAGE_FOLDER_RELEASE' "$_BGFX_DATA" || true)
    if [[ -n "$_LINE" ]]; then
        _BGFX_DIR=$(echo "$_LINE" | sed 's/^set(bgfx_PACKAGE_FOLDER_RELEASE "\(.*\)")/\1/')
        if [[ -f "$_BGFX_DIR/bin/shaderc" ]]; then
            HOST_SHADERC="$_BGFX_DIR/bin/shaderc"
        fi
    fi
fi
if [[ -n "$HOST_SHADERC" ]]; then
    echo "  $HOST_SHADERC"
else
    echo "  WARNING: host shaderc not found - shaders will not be compiled."
fi

# -- Step 3: Conan install (skip if toolchain exists) --
echo "=== Step 3: Conan install (linux-arm64) ==="
if [[ -f "$TOOLCHAIN" ]]; then
    echo "  Toolchain exists, skipping. Use --clean to force."
else
    conan install . -pr:h "$PROFILE" -pr:b "$BUILD_PROFILE" \
        --output-folder="$CONAN_DIR" --build=missing
fi

if [[ ! -f "$TOOLCHAIN" ]]; then
    echo "ERROR: Conan toolchain not found: $TOOLCHAIN"
    exit 1
fi

# -- Step 4: CMake configure --
echo "=== Step 4: CMake configure (linux-arm64) ==="
TOOLCHAIN="$(cd "$(dirname "$TOOLCHAIN")" && pwd)/$(basename "$TOOLCHAIN")"
CMAKE_ARGS="-S . -B $BUILD_DIR -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
    -DCMAKE_BUILD_TYPE=Release \
    -DJCE_PAK_EXECUTABLE=$HOST_PAK \
    -DJCE_ENABLE_CPPCHECK=OFF"
if [[ -n "$HOST_SHADERC" ]]; then
    CMAKE_ARGS="$CMAKE_ARGS -DJCE_SHADERC_EXECUTABLE=$HOST_SHADERC"
fi
cmake $CMAKE_ARGS

# -- Step 5: Build (Ninja handles incremental) --
echo "=== Step 5: Build (linux-arm64) ==="
cmake --build "$BUILD_DIR"

if [[ ! -f "$BUILD_DIR/CagedKingdom" ]]; then
    echo "ERROR: CagedKingdom not found after build"
    exit 1
fi

echo ""
echo "[SUCCESS] Linux ARM64 build complete: $BUILD_DIR/CagedKingdom"
_jce_success_wait
