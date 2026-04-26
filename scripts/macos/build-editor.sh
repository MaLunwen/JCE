#!/usr/bin/env bash
# ================================================================
# build-editor.sh -- Build JCE editor for macOS (auto-detects arch)
# Usage: build-editor.sh [--clean] [--dist]
# Output: build/desktop/macos-arm64/release/jce_editor  (Apple Silicon)
#         build/desktop/macos-x64/release/jce_editor     (Intel)
#         .../dist/jce_editor                             (with --dist)
# ================================================================
source "$(dirname "$0")/../lib/jce_common.sh"
# -- Detect native architecture --
NATIVE_ARCH="$(uname -m)"
if [[ "$NATIVE_ARCH" == "arm64" ]]; then
    ARCH_TAG="macos-arm64"
    HOST_PROFILE="conan/profiles/macos-arm64"
    BUILD_PROFILE="conan/profiles/macos-arm64"
    CMAKE_OSX_ARCH="arm64"
    MACOS_DEPLOYMENT_TARGET="11.0"
    _BGFX_DATA_PRIMARY="build/host-conan/build/Release/generators/bgfx-release-armv8-data.cmake"
    _BGFX_DATA_FALLBACK="build/host-conan/build/Release/generators/bgfx-release-x86_64-data.cmake"
elif [[ "$NATIVE_ARCH" == "x86_64" ]]; then
    ARCH_TAG="macos-x64"
    HOST_PROFILE="conan/profiles/macos-x64"
    BUILD_PROFILE="conan/profiles/macos-x64"
    CMAKE_OSX_ARCH="x86_64"
    MACOS_DEPLOYMENT_TARGET="10.15"
    _BGFX_DATA_PRIMARY="build/host-conan/build/Release/generators/bgfx-release-x86_64-data.cmake"
    _BGFX_DATA_FALLBACK="build/host-conan/build/Release/generators/bgfx-release-armv8-data.cmake"
else
    echo "ERROR: Unsupported macOS architecture: $NATIVE_ARCH"
    exit 1
fi

CONAN_DIR="build/desktop/${ARCH_TAG}-conan"
BUILD_DIR="build/desktop/${ARCH_TAG}"
TOOLCHAIN="$CONAN_DIR/build/Release/generators/conan_toolchain.cmake"
HOST_PAK="build/host/jce_pak"
CONAN_HOME_DIR="${CONAN_HOME:-$HOME/.conan2}"
CONAN_HOOKS_DIR="$CONAN_HOME_DIR/extensions/hooks"
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
    conan install . -pr:h "$HOST_PROFILE" -pr:b "$BUILD_PROFILE" \
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
    -DCMAKE_OSX_ARCHITECTURES=$CMAKE_OSX_ARCH \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOS_DEPLOYMENT_TARGET \
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
