#!/usr/bin/env bash
# ================================================================
# build-macos-arm64.sh -- Build JCE for macOS ARM64 (Apple Silicon)
# Usage: build-macos-arm64.sh [--clean]
# Output: build/desktop/macos-arm64/src/JCE
# ================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

HOST_PROFILE="conan/profiles/macos-arm64"
BUILD_ARCH="$(uname -m)"
if [[ "$BUILD_ARCH" == "x86_64" ]]; then
    BUILD_PROFILE="conan/profiles/macos-x64"
elif [[ "$BUILD_ARCH" == "arm64" ]]; then
    BUILD_PROFILE="conan/profiles/macos-arm64"
else
    echo "ERROR: Unsupported build architecture: $BUILD_ARCH"
    exit 1
fi
CONAN_DIR="build/desktop/macos-arm64-conan"
BUILD_DIR="build/desktop/macos-arm64"
TOOLCHAIN="$CONAN_DIR/build/Release/generators/conan_toolchain.cmake"
HOST_PAK="build/host/src/tools/jce_pak"
MACOS_DEPLOYMENT_TARGET="11.0"
CONAN_HOME_DIR="${CONAN_HOME:-$HOME/.conan2}"
CONAN_HOOKS_DIR="$CONAN_HOME_DIR/extensions/hooks"

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

# -- Step 1.5: Sync repository Conan hooks --
echo "=== Step 1.5: Sync Conan hooks ==="
mkdir -p "$CONAN_HOOKS_DIR"
HOOK_FILES=("$REPO_ROOT"/conan/hooks/hook_*.py)
if [[ -f "${HOOK_FILES[0]}" ]]; then
    cp "$REPO_ROOT"/conan/hooks/hook_*.py "$CONAN_HOOKS_DIR"/
    echo "  Synced hooks -> $CONAN_HOOKS_DIR"
else
    echo "  WARNING: no hook_*.py found under $REPO_ROOT/conan/hooks"
fi

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
echo "=== Step 3: Conan install (macos-arm64) ==="
if [[ -f "$TOOLCHAIN" ]]; then
    echo "  Toolchain exists, skipping. Use --clean to force."
else
    conan install . -pr:h "$HOST_PROFILE" -pr:b "$BUILD_PROFILE" \
        --output-folder="$CONAN_DIR" --build=missing
fi

if [[ ! -f "$TOOLCHAIN" ]]; then
    echo "ERROR: Conan toolchain not found: $TOOLCHAIN"
    exit 1
fi

# -- Step 4: CMake configure --
echo "=== Step 4: CMake configure (macos-arm64) ==="
TOOLCHAIN="$(cd "$(dirname "$TOOLCHAIN")" && pwd)/$(basename "$TOOLCHAIN")"
CMAKE_ARGS="-S . -B $BUILD_DIR -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOS_DEPLOYMENT_TARGET \
    -DJCE_PAK_EXECUTABLE=$HOST_PAK \
    -DJCE_ENABLE_CPPCHECK=OFF"
if [[ -n "$HOST_SHADERC" ]]; then
    CMAKE_ARGS="$CMAKE_ARGS -DJCE_SHADERC_EXECUTABLE=$HOST_SHADERC"
fi
cmake $CMAKE_ARGS

# -- Step 5: Build (Ninja handles incremental) --
echo "=== Step 5: Build (macos-arm64) ==="
cmake --build "$BUILD_DIR"

if [[ ! -f "$BUILD_DIR/src/JCE" ]]; then
    echo "ERROR: JCE not found after build"
    exit 1
fi

echo ""
echo "[SUCCESS] macOS ARM64 build complete: $BUILD_DIR/src/JCE"
