#!/usr/bin/env bash
# ================================================================
# build-ios-arm64.sh -- Cross-compile JCE for iOS ARM64 (Xcode)
# Usage: build-ios-arm64.sh [--clean]
# Output:
#   Xcode project: build/mobile/ios-arm64/JCE.xcodeproj
#   Device  .app : build/mobile/ios-arm64/src/Release-iphoneos/JCE.app
# Requires: Xcode with iOS SDK installed
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

# -- Parse flags --
CLEAN=false
for arg in "$@"; do
    case "$arg" in
        --clean)     CLEAN=true ;;
    esac
done

SDK="iphoneos"
TARGET_PROFILE="conan/profiles/ios-arm64"

# Detect host architecture for build profile
HOST_ARCH="$(uname -m)"
if [[ "$HOST_ARCH" == "x86_64" ]]; then
    HOST_PROFILE="conan/profiles/macos-x64"
elif [[ "$HOST_ARCH" == "arm64" ]]; then
    HOST_PROFILE="conan/profiles/macos-arm64"
else
    echo "ERROR: Unsupported host architecture: $HOST_ARCH"
    exit 1
fi

CONAN_DIR="build/mobile/ios-arm64-conan"
BUILD_DIR="build/mobile/ios-arm64"
TOOLCHAIN="$CONAN_DIR/build/Release/generators/conan_toolchain.cmake"
HOST_PAK="build/host/jce_pak"

# -- Handle --clean flag --
if $CLEAN; then
    echo "=== Cleaning build directories ==="
    rm -rf "$BUILD_DIR" "$CONAN_DIR"
    echo "  Done"
fi

# -- Step 1: Ensure host jce_pak exists --
echo "=== Step 1: Resolve host jce_pak ==="
if [[ ! -f "$HOST_PAK" ]]; then
    echo "  Host jce_pak not found - building..."
    bash "$REPO_ROOT/scripts/macos/build-host-tools.sh"
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

# -- Step 3: Conan install (dependencies) --
echo ""
echo "=== Step 3: Conan install (iOS, Ninja deps) ==="
if [[ -f "$TOOLCHAIN" ]]; then
    echo "  Conan toolchain exists, skipping. Use --clean to force."
else
    conan install . -pr:b "$HOST_PROFILE" -pr:h "$TARGET_PROFILE" \
        --output-folder="$CONAN_DIR" --build=missing \
        -c tools.cmake.cmaketoolchain:generator=Ninja
fi

if [[ ! -f "$TOOLCHAIN" ]]; then
    echo "ERROR: Xcode Conan toolchain not found: $TOOLCHAIN"
    exit 1
fi
TOOLCHAIN="$(cd "$(dirname "$TOOLCHAIN")" && pwd)/$(basename "$TOOLCHAIN")"

# -- Step 4: Generate Xcode project --
echo "=== Step 4: CMake configure (iOS, Xcode) ==="
CMAKE_ARGS="-S . -B $BUILD_DIR -G Xcode \
    -DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DJCE_PAK_EXECUTABLE=$HOST_PAK \
    -DJCE_ENABLE_CPPCHECK=OFF"
if [[ -n "$HOST_SHADERC" ]]; then
    CMAKE_ARGS="$CMAKE_ARGS -DJCE_SHADERC_EXECUTABLE=$HOST_SHADERC"
fi
cmake $CMAKE_ARGS

# -- Step 5: Build with xcodebuild --
echo "=== Step 5: Xcode build ($SDK) ==="

# Close Xcode GUI if open to avoid build.db lock
osascript -e 'quit app "Xcode"' 2>/dev/null || true
sleep 1
rm -rf "$BUILD_DIR/build/XCBuildData" 2>/dev/null || true

XCODE_BUILD_ARGS=(
    -project "$BUILD_DIR/JCE.xcodeproj"
    -target JCE
    -configuration Release
    -sdk "$SDK"
    -quiet
)
XCODE_BUILD_ARGS+=(-allowProvisioningUpdates)

xcodebuild "${XCODE_BUILD_ARGS[@]}"

XCODE_APP="$BUILD_DIR/src/Release-${SDK}/JCE.app"
if [[ -d "$XCODE_APP" ]]; then
    echo ""
    echo "[SUCCESS] iOS build complete! ($SDK)"
    echo "  App: $XCODE_APP"

    # -- Step 6: Auto transfer to connected device --
    echo ""
    echo "=== Step 6: Auto transfer to iPhone ==="
    if ! command -v ios-deploy >/dev/null 2>&1; then
        echo "  ios-deploy not found. Skipping auto transfer."
        echo "  Install: brew install ios-deploy"
        echo "  Then run: ios-deploy --bundle $XCODE_APP --no-wifi"
    else
        _DETECT_LOG="$(ios-deploy --detect --no-wifi 2>&1 || true)"
        if echo "$_DETECT_LOG" | grep -q "Found "; then
            echo "  Device detected. Installing app..."
            if ios-deploy --bundle "$XCODE_APP" --no-wifi; then
                echo "  [SUCCESS] App transferred to connected iPhone."
            else
                echo "  [WARNING] Transfer failed. You can install manually with:"
                echo "    ios-deploy --bundle $XCODE_APP --no-wifi"
            fi
        else
            echo "  No USB iOS device detected. Skipping transfer."
            echo "  Connect device and run: ios-deploy --bundle $XCODE_APP --no-wifi"
        fi
    fi
    _jce_success_wait
else
    echo ""
    echo "[WARNING] Xcode build completed but JCE.app not found at expected path."
    echo "  Open Xcode manually: open $BUILD_DIR/JCE.xcodeproj"
    _jce_success_wait
fi
