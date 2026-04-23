#!/usr/bin/env bash
# ================================================================
# build-editor.sh -- Build JCE editor for Linux (auto-detects arch)
# Usage: build-editor.sh [--clean] [--dist]
# Output: build/desktop/linux-x64/release/jce_editor
#         build/desktop/linux-x64/dist/jce_editor    (with --dist)
#         build/desktop/linux-arm64/...               (on aarch64)
#
# System dependencies (Ubuntu/Debian):
#   sudo apt install libx11-dev libxrandr-dev libxcursor-dev libxi-dev \
#       libxinerama-dev libxext-dev libxfixes-dev libxss-dev \
#       libgl-dev libudev-dev libdbus-1-dev \
#       ninja-build cmake pkg-config
# ================================================================
set -euo pipefail

_jce_on_fail() {
    echo ""
    echo "[FAILED] Build failed."
    [[ -t 0 ]] || return
    local t=15 e=0 paused=false colon=false key=""
    while true; do
        if [[ "$paused" == false ]]; then
            printf "\r[FAILED] Auto-closing in %ds... (any key to pause, :q to quit)  " $((t - e))
            if IFS= read -r -t 1 -n 1 key 2>/dev/null; then
                paused=true
                printf "\r[FAILED] Paused - type :q to quit.                                    "
                [[ "$key" == ":" ]] && colon=true || colon=false
            else
                e=$((e + 1)); [[ $e -ge $t ]] && { echo ""; break; }
            fi
        else
            if IFS= read -r -t 0.1 -n 1 key 2>/dev/null; then
                if [[ "$colon" == true && ("$key" == "q" || "$key" == "Q") ]]; then echo ""; break; fi
                [[ "$key" == ":" ]] && colon=true || colon=false
            fi
        fi
    done
}
trap '_jce_on_fail' ERR

_jce_success_wait() {
    [[ -t 1 ]] || return
    local t=5 e=0 key=""
    while true; do
        printf "\r[SUCCESS] Auto-closing in %ds... (:q to quit)  " $((t - e))
        if IFS= read -r -t 1 -n 1 key 2>/dev/null; then
            if [[ "$key" == ":" ]]; then
                IFS= read -r -t 2 -n 1 key2 2>/dev/null || true
                [[ "${key2:-}" == "q" || "${key2:-}" == "Q" ]] && { echo ""; return; }
            fi
        fi
        e=$((e + 1)); [[ $e -ge $t ]] && { echo ""; return; }
    done
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

# -- Detect native architecture --
BUILD_ARCH="$(uname -m)"
if [[ "$BUILD_ARCH" == "x86_64" ]]; then
    ARCH_TAG="linux-x64"
    PROFILE="conan/profiles/linux-x64"
    _BGFX_DATA_PRIMARY="build/host-conan/build/Release/generators/bgfx-release-x86_64-data.cmake"
    _BGFX_DATA_FALLBACK="build/host-conan/build/Release/generators/bgfx-release-armv8-data.cmake"
elif [[ "$BUILD_ARCH" == "aarch64" ]]; then
    ARCH_TAG="linux-arm64"
    PROFILE="conan/profiles/linux-arm64"
    _BGFX_DATA_PRIMARY="build/host-conan/build/Release/generators/bgfx-release-armv8-data.cmake"
    _BGFX_DATA_FALLBACK="build/host-conan/build/Release/generators/bgfx-release-x86_64-data.cmake"
else
    echo "ERROR: Unsupported architecture: $BUILD_ARCH"
    exit 1
fi

CONAN_DIR="build/desktop/${ARCH_TAG}-conan"
BUILD_DIR="build/desktop/${ARCH_TAG}"
TOOLCHAIN="$CONAN_DIR/build/Release/generators/conan_toolchain.cmake"
HOST_PAK="build/host/jce_pak"
VARIANT="release"

# -- Parse arguments --
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)
            echo "=== Cleaning build directory ==="
            rm -rf "$BUILD_DIR" "$CONAN_DIR"
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
_BGFX_DATA="$_BGFX_DATA_PRIMARY"
if [[ ! -f "$_BGFX_DATA" ]]; then
    _BGFX_DATA="$_BGFX_DATA_FALLBACK"
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
echo "=== Step 3: Conan install ($ARCH_TAG) ==="
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
echo "=== Step 4: CMake configure ($ARCH_TAG, $VARIANT) ==="
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

# -- Step 5: Build JCE_Editor --
echo "=== Step 5: Build JCE_Editor ($ARCH_TAG) ==="
cmake --build "$BUILD_DIR" --target JCE_Editor

if [[ ! -f "$BUILD_DIR/$VARIANT/jce_editor" ]]; then
    echo "ERROR: jce_editor not found after build: $BUILD_DIR/$VARIANT/jce_editor"
    exit 1
fi

echo ""
echo "[SUCCESS] Editor build complete ($VARIANT): $BUILD_DIR/$VARIANT/jce_editor"
_jce_success_wait
