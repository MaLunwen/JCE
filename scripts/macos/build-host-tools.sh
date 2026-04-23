#!/usr/bin/env bash
# ================================================================
# build-host-tools.sh -- Build host jce_pak on macOS
# Usage: build-host-tools.sh [--clean]
# Output: build/host/jce_pak
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

CONAN_DIR="build/host-conan"
BUILD_DIR="build/host"
TOOLCHAIN="$CONAN_DIR/build/Release/generators/conan_toolchain.cmake"

# Detect host architecture for profile selection
ARCH="$(uname -m)"
if [[ "$ARCH" == "x86_64" ]]; then
    PROFILE="conan/profiles/macos-x64"
elif [[ "$ARCH" == "arm64" ]]; then
    PROFILE="conan/profiles/macos-arm64"
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
echo "=== Step 1: Conan install (macOS host) ==="
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
