#!/usr/bin/env bash
# ================================================================
# build-host-tools.sh -- Build host jce_pak on Linux
# Usage: build-host-tools.sh [--clean]
# Output: build/host/jce_pak
# ================================================================
source "$(dirname "$0")/../lib/jce_common.sh"
CONAN_DIR="build/host-conan"
BUILD_DIR="build/host"
TOOLCHAIN="$CONAN_DIR/build/Release/generators/conan_toolchain.cmake"

# Detect host architecture for profile selection
ARCH="$(uname -m)"
if [[ "$ARCH" == "x86_64" ]]; then
    PROFILE="conan/profiles/linux-x64"
elif [[ "$ARCH" == "aarch64" ]]; then
    PROFILE="conan/profiles/linux-arm64"
else
    echo "ERROR: Unsupported architecture: $ARCH"
    exit 1
fi

# -- Handle --clean flag --
if [[ "${1:-}" == "--clean" ]]; then
    echo "=== Cleaning build directory ==="
    rm -rf "$BUILD_DIR" "$CONAN_DIR"
    echo "  Done"
fi

# -- Step 1: Conan install (skip if toolchain exists) --
echo "=== Step 1: Conan install (Linux host) ==="
if [[ -f "$TOOLCHAIN" ]]; then
    echo "  Toolchain exists, skipping. Use --clean to force."
else
    conan install . -pr:h "$PROFILE" -pr:b "$PROFILE" \
        --output-folder="$CONAN_DIR" --build=missing
fi

if [[ ! -f "$TOOLCHAIN" ]]; then
    echo "ERROR: Conan toolchain not found: $TOOLCHAIN"
    exit 1
fi

# -- Step 2: CMake configure --
echo "=== Step 2: CMake configure ==="
TOOLCHAIN="$(cd "$(dirname "$TOOLCHAIN")" && pwd)/$(basename "$TOOLCHAIN")"
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DJCE_ENABLE_CPPCHECK=OFF

# -- Step 3: Build jce_pak --
echo "=== Step 3: Build jce_pak ==="
cmake --build "$BUILD_DIR" --target jce_pak

if [[ ! -f "$BUILD_DIR/jce_pak" ]]; then
    echo "ERROR: jce_pak not found after build"
    exit 1
fi

echo ""
echo "[SUCCESS] Host jce_pak ready: $BUILD_DIR/jce_pak"
_jce_success_wait
