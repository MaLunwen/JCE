#!/usr/bin/env bash
# ================================================================
# build-ios-arm64.sh -- Cross-compile JCE for iOS ARM64 (Xcode)
# Usage: build-ios-arm64.sh [--clean]
# Output:
#   Xcode project: build/mobile/ios-arm64/JCE.xcodeproj
#   Device  .app : build/mobile/ios-arm64/src/Release-iphoneos/JCE.app
# Requires: Xcode with iOS SDK installed
# ================================================================
source "$(dirname "$0")/../../scripts/lib/jce_common.sh"

# Which in-tree project this script packages.  The engine is general; whatever
# sits in this slot is not, so it is a variable rather than a spelled-out name.
# Same slot as CMake's JCE_GAME_TARGET / JCE_GAME_PROJECT_DIR.
: "${JCE_GAME_TARGET:?Set the consumer CMake target}"
: "${JCE_GAME_PROJECT_DIR:?Set the consumer project path}"

# Unlock the login keychain before xcodebuild so codesign can access signing certs.
# Works in both interactive and SSH/headless sessions.
if [[ -n "${KEYCHAIN_PASSWORD:-}" ]]; then
    security unlock-keychain -p "$KEYCHAIN_PASSWORD" ~/Library/Keychains/login.keychain-db 2>/dev/null || true
else
    # No password in env — try a silent unlock (no-op if already unlocked);
    # on macOS this shows a system dialog if the keychain is locked.
    security unlock-keychain ~/Library/Keychains/login.keychain-db 2>/dev/null || true
fi

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
HOST_PAK="build/host/tools/jce_pak"

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
    # Call jce.py directly (not the build-host-tools shim) so this orchestrator
    # keeps its own single beep + ":q" footer instead of nesting a second one.
    python3 "$REPO_ROOT/scripts/jce.py" host-tools
fi
if [[ ! -f "$HOST_PAK" ]]; then
    echo "ERROR: jce_pak still not found after host build"
    exit 1
fi
HOST_PAK="$(cd "$(dirname "$HOST_PAK")" && pwd)/$(basename "$HOST_PAK")"
echo "  $HOST_PAK"

# -- Step 2: Locate host shaderc --
echo "=== Step 2: Locate host shaderc ==="
# jce_shaderc lives in lib/jce_common.sh and is the shell twin of
# lib/jce_build_common.bat's `shaderc` subcommand.  This script used to carry
# a third copy of the build/host-conan generator-file approach, which reads
# the package folder of the conan install that WROTE that file -- and Conan
# garbage-collects package folders.  "require": a miss is fatal here rather
# than hundreds of lines later as a pak integrity error.
jce_shaderc require
HOST_SHADERC="$JCE_SHADERC"

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
    -DJCE_ENABLE_CPPCHECK=OFF \
    -DJCE_BUILD_INTREE_SAMPLE=ON \
    -DJCE_GAME_PROJECT_DIR=$JCE_GAME_PROJECT_DIR"
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
    -target "$JCE_GAME_TARGET"
    -configuration Release
    -sdk "$SDK"
    -quiet
    -allowProvisioningUpdates
)

xcodebuild "${XCODE_BUILD_ARGS[@]}"

# Xcode places the .app in Release-iphoneos/ under the CMake RUNTIME_OUTPUT_DIRECTORY.
# Try both the SDK-suffixed and plain paths to handle different CMake setups.
XCODE_APP=""
for _candidate in \
    "$BUILD_DIR/release/Release-iphoneos/${JCE_GAME_TARGET}.app" \
    "$BUILD_DIR/Release-iphoneos/${JCE_GAME_TARGET}.app" \
    "$BUILD_DIR/release/Release/${JCE_GAME_TARGET}.app" \
    "$BUILD_DIR/release/Release/${JCE_GAME_PROJECT_DIR}.app"; do
    if [[ -d "$_candidate" ]]; then
        XCODE_APP="$_candidate"
        break
    fi
done

if [[ -n "$XCODE_APP" ]]; then
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
    echo "[WARNING] Xcode build completed but ${JCE_GAME_TARGET}.app not found."
    echo "  Searched:"
    echo "    $BUILD_DIR/release/Release-iphoneos/${JCE_GAME_TARGET}.app"
    echo "    $BUILD_DIR/Release-iphoneos/${JCE_GAME_TARGET}.app"
    echo "  Open Xcode manually: open $BUILD_DIR/JCE.xcodeproj"
    _jce_success_wait
fi
