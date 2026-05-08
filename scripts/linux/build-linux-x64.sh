#!/usr/bin/env bash
# ================================================================
# build-linux-x64.sh -- Build JCE for Linux x86_64
# Usage: build-linux-x64.sh [--clean] [--dist]
# Output:
#   Game exe : build/desktop/linux-x64/release/caged_kingdom
#              build/desktop/linux-x64/dist/caged_kingdom    (with --dist)
#   JNI lib  : build/jni/natives/linux-x86_64/libjce.so
#
# System dependencies (Ubuntu/Debian):
#   sudo apt install libx11-dev libxrandr-dev libxcursor-dev libxi-dev \
#       libxinerama-dev libxext-dev libxfixes-dev libxss-dev \
#       libgl-dev libudev-dev libdbus-1-dev \
#       ninja-build cmake pkg-config
# ================================================================
source "$(dirname "$0")/../lib/jce_common.sh"
PROFILE="conan/profiles/linux-x64"
CONAN_DIR="build/desktop/linux-x64-conan"
BUILD_DIR="build/desktop/linux-x64"
JNI_BUILD_DIR="build/jni/linux-x64"
JNI_CLASSIFIER="linux-x86_64"
TOOLCHAIN="$CONAN_DIR/build/Release/generators/conan_toolchain.cmake"
HOST_PAK="build/host/tools/jce_pak"
VARIANT="release"

# -- Parse arguments --
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            echo "=== Cleaning build directories ==="
            rm -rf "$BUILD_DIR" "$CONAN_DIR" "$JNI_BUILD_DIR"
            echo "  Done"
            ;;
        --dist)
            VARIANT="dist"
            ;;
        *)
            echo "Unknown argument: $1"
            exit 1
            ;;
    esac
    shift
done

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
_BGFX_DATA="build/host-conan/build/Release/generators/bgfx-release-x86_64-data.cmake"
if [[ ! -f "$_BGFX_DATA" ]]; then
    _BGFX_DATA="build/host-conan/build/Release/generators/bgfx-release-armv8-data.cmake"
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
echo "=== Step 3: Conan install (linux-x64) ==="
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

# -- Step 4: CMake configure --
echo "=== Step 4: CMake configure (linux-x64) ==="
TOOLCHAIN="$(cd "$(dirname "$TOOLCHAIN")" && pwd)/$(basename "$TOOLCHAIN")"
CMAKE_ARGS="-S . -B $BUILD_DIR -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
    -DCMAKE_BUILD_TYPE=Release \
    -DJCE_PAK_EXECUTABLE=$HOST_PAK \
    -DJCE_BUILD_VARIANT=$VARIANT \
    -DJCE_ENABLE_CPPCHECK=OFF"
if [[ -n "$HOST_SHADERC" ]]; then
    CMAKE_ARGS="$CMAKE_ARGS -DJCE_SHADERC_EXECUTABLE=$HOST_SHADERC"
fi
cmake $CMAKE_ARGS

# -- Step 5: Build (Ninja handles incremental) --
echo "=== Step 5: Build (linux-x64, $VARIANT) ==="
cmake --build "$BUILD_DIR"

if [[ ! -f "$BUILD_DIR/$VARIANT/caged_kingdom" ]]; then
    echo "ERROR: caged_kingdom not found after build: $BUILD_DIR/$VARIANT/caged_kingdom"
    exit 1
fi

strip --strip-unneeded "$BUILD_DIR/$VARIANT/caged_kingdom"

echo ""
echo "[SUCCESS] Linux x64 build complete ($VARIANT): $BUILD_DIR/$VARIANT/caged_kingdom"

# -- Step 6: Build JNI shared library and stage for fat JAR --
echo ""
echo "=== Step 6: Build & stage JNI native (linux-x86_64) ==="
JNI_CMAKE_ARGS="-S . -B $JNI_BUILD_DIR -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
    -DCMAKE_BUILD_TYPE=Release \
    -DJCE_PAK_EXECUTABLE=$HOST_PAK \
    -DJCE_ENABLE_CPPCHECK=OFF \
    -DJCE_BUILD_JNI=ON"
if [[ -n "$HOST_SHADERC" ]]; then
    JNI_CMAKE_ARGS="$JNI_CMAKE_ARGS -DJCE_SHADERC_EXECUTABLE=$HOST_SHADERC"
fi
cmake $JNI_CMAKE_ARGS
cmake --build "$JNI_BUILD_DIR" --target CagedKingdom

JNI_LIB="$JNI_BUILD_DIR/release/libjce.so"
if [[ -f "$JNI_LIB" ]]; then
    JNI_STAGE="build/jni/natives/$JNI_CLASSIFIER"
    mkdir -p "$JNI_STAGE"
    cp "$JNI_LIB" "$JNI_STAGE/libjce.so"
    strip --strip-unneeded "$JNI_STAGE/libjce.so"
    echo "  Staged: $JNI_STAGE/libjce.so"
else
    echo "  WARNING: $JNI_LIB not found — JNI stage skipped."
fi

_jce_success_wait
