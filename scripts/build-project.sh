#!/usr/bin/env bash
# ===================================================================
# build-project.sh  Universal JCE project build entry point (POSIX).
#
# Usage:
#   build-project.sh <project_dir> [options]
#
# Options:
#   --sdk <dir>                  JCE SDK root (overrides jce_project.json
#                                sdk field and the JCE_SDK_DIR env var).
#   --target <CMakeTarget>       Target to build  (default: project name).
#   --exe <name>                 Expected artifact filename (verification).
#   --variant <debug|release|dist>   Build variant (default: release).
#   --arch <x86_64|i686|aarch64|armv7>
#                                Target architecture (aliases x64/x86/arm64/arm
#                                accepted).  Defaults to host arch via
#                                uname -m.
#   --target-platform <p>        linux|darwin|android|ios|wasm
#                                (host-native is the default).
#   --clean                      Remove the project's build/ subdir first.
#
# Resolution order for the SDK:
#   1. --sdk <dir>
#   2. <project_dir>/jce_project.json "sdk"
#   3. $JCE_SDK_DIR
#   4. <editor_install_dir>/sdk
#   5. Engine-workspace fallback: auto cmake --install the engine into
#      <engine_root>/dist/sdk/<platform>-<arch> the first time, then use it.
#
# Output:
#   <project_dir>/build/<platform>-<arch>-<variant>/<exe>
# ===================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<EOF
build-project.sh  Build a JCE project.

  build-project.sh <project_dir> [--sdk DIR] [--target NAME]
      [--exe NAME] [--variant debug|release|dist]
      [--arch x86_64|i686|aarch64|armv7]  (aliases: x64|x86|arm64|arm)
      [--target-platform linux|darwin|android|ios|wasm] [--clean]
EOF
    exit 1
}

[[ $# -ge 1 ]] || usage

PROJECT_DIR="$1"; shift
SDK_DIR=""
TARGET=""
EXE_NAME=""
VARIANT="release"
case "$(uname -s)" in
    Linux*)  PLATFORM="linux" ;;
    Darwin*) PLATFORM="darwin" ;;
    *)       PLATFORM="linux" ;;
esac
# Probe host arch (overridable via --arch).  Normalised to the
# GNU-triplet style (x86_64/i686/aarch64) used by the SDK install tree.
case "$(uname -m)" in
    x86_64|amd64)  ARCH="x86_64" ;;
    aarch64|arm64) ARCH="aarch64" ;;
    i?86)          ARCH="i686" ;;
    arm*)          ARCH="armv7" ;;
    *)             ARCH="x86_64" ;;
esac
DO_CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sdk)              SDK_DIR="$2"; shift 2 ;;
        --target)           TARGET="$2"; shift 2 ;;
        --exe)              EXE_NAME="$2"; shift 2 ;;
        --variant)          VARIANT="$2"; shift 2 ;;
        --arch)             ARCH="$2"; shift 2 ;;
        --target-platform)  PLATFORM="$2"; shift 2 ;;
        --clean)            DO_CLEAN=1; shift ;;
        -h|--help)          usage ;;
        *) echo "ERROR: unknown argument: $1" >&2; usage ;;
    esac
done

# Accept short aliases for --arch.
case "$ARCH" in
    x64|amd64)  ARCH="x86_64"  ;;
    x86)        ARCH="i686"    ;;
    arm64)      ARCH="aarch64" ;;
    arm|armv7a|armhf) ARCH="armv7" ;;
esac
case "$ARCH" in
    x86_64|i686|aarch64|armv7) ;;
    *)
        echo "[build-project] ERROR: unsupported --arch '$ARCH'" >&2
        echo "                (expected: x86_64|i686|aarch64|armv7, aliases x64|x86|arm64|arm)" >&2
        exit 2
        ;;
esac

# Normalise platform aliases.
case "$PLATFORM" in
    mac|macos|osx)   PLATFORM="darwin" ;;
    win|windows)     PLATFORM="win32" ;;
esac

[[ -d "$PROJECT_DIR" ]] || { echo "[build-project] ERROR: project directory not found: $PROJECT_DIR" >&2; exit 2; }
PROJECT_DIR="$(cd "$PROJECT_DIR" && pwd)"

MANIFEST="$PROJECT_DIR/jce_project.json"
[[ -f "$MANIFEST" ]] || {
    echo "[build-project] ERROR: no jce_project.json in $PROJECT_DIR" >&2
    echo "                Create one with the editor: File > New Project" >&2
    exit 2
}

# ------- Light-touch JSON scrape (flat string fields only) ----------
json_str() {
    # $1 = manifest, $2 = key
    sed -nE "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"([^\"]+)\".*/\1/p" "$1" | head -n1
}

[[ -z "$TARGET"   ]] && TARGET="$(json_str "$MANIFEST" target)"
[[ -z "$TARGET"   ]] && TARGET="$(json_str "$MANIFEST" name)"
[[ -z "$EXE_NAME" ]] && EXE_NAME="$(json_str "$MANIFEST" exe)"
[[ -z "$EXE_NAME" ]] && EXE_NAME="$TARGET"

# Platform stub: only host-native is implemented today.
case "$PLATFORM" in
    linux|darwin) ;;
    *)
        echo "[build-project] Target platform '$PLATFORM' is not yet wired up" >&2
        echo "                in build-project.sh on this host.  Planned in a" >&2
        echo "                follow-up phase (toolchain detector + per-platform" >&2
        echo "                dispatch).  For now only linux|darwin are implemented." >&2
        exit 3
        ;;
esac

# ------- Resolve SDK -------------------------------------------------
[[ -z "$SDK_DIR" ]] && SDK_DIR="$(json_str "$MANIFEST" sdk || true)"
[[ -z "$SDK_DIR" && -n "${JCE_SDK_DIR:-}" ]] && SDK_DIR="$JCE_SDK_DIR"
if [[ -z "$SDK_DIR" && -f "$SCRIPT_DIR/../sdk/lib/cmake/JCE/JCEConfig.cmake" ]]; then
    SDK_DIR="$(cd "$SCRIPT_DIR/../sdk" && pwd)"
fi
[[ -n "$SDK_DIR" ]] && SDK_DIR="$(cd "$SDK_DIR" && pwd)"

USE_SDK=0
JCE_CMAKE_DIR=""
if [[ -n "$SDK_DIR" ]]; then
    if [[ -f "$SDK_DIR/lib/cmake/JCE/JCEConfig.cmake" ]]; then
        USE_SDK=1; JCE_CMAKE_DIR="$SDK_DIR/lib/cmake/JCE"
    elif [[ -f "$SDK_DIR/cmake/JCEConfig.cmake" ]]; then
        USE_SDK=1; JCE_CMAKE_DIR="$SDK_DIR/cmake"
    fi
fi

# ------- Engine-workspace fallback -----------------------------------
ENGINE_ROOT=""
if [[ "$USE_SDK" == "0" ]]; then
    _p="$PROJECT_DIR"
    while [[ -n "$_p" && "$_p" != "/" ]]; do
        if [[ -f "$_p/engine/include/jce/api.h" ]]; then
            ENGINE_ROOT="$_p"; break
        fi
        _p="$(dirname "$_p")"
    done
fi

if [[ "$USE_SDK" == "0" && -z "$ENGINE_ROOT" ]]; then
    echo "[build-project] ERROR: cannot locate a JCE SDK." >&2
    echo "                 - pass --sdk <dir>" >&2
    echo "                 - set the JCE_SDK_DIR environment variable" >&2
    echo "                 - or set \"sdk\" in jce_project.json" >&2
    exit 4
fi

if [[ "$USE_SDK" == "0" ]]; then
    AUTO_SDK="$ENGINE_ROOT/dist/sdk/$PLATFORM-$ARCH"
    # Engine build tree uses its own short arch names; translate.
    case "$ARCH" in
        x86_64)  ENG_ARCH="x64"   ;;
        i686)    ENG_ARCH="x86"   ;;
        aarch64) ENG_ARCH="arm64" ;;
        *)       ENG_ARCH="$ARCH" ;;
    esac
    if [[ "$PLATFORM" == "darwin" ]]; then
        ENGINE_BUILD="$ENGINE_ROOT/build/desktop/macos-$ENG_ARCH"
    else
        ENGINE_BUILD="$ENGINE_ROOT/build/desktop/linux-$ENG_ARCH"
    fi
    if [[ ! -f "$AUTO_SDK/lib/cmake/JCE/JCEConfig.cmake" ]]; then
        if [[ ! -f "$ENGINE_BUILD/CMakeCache.txt" ]]; then
            echo "[build-project] ERROR: engine workspace at $ENGINE_ROOT" >&2
            echo "                has no build tree at $ENGINE_BUILD." >&2
            echo "                Run scripts/build-desktop.sh once to configure" >&2
            echo "                the engine, then retry." >&2
            exit 9
        fi
        echo "[build-project] Auto-installing JCE SDK from engine build tree"
        echo "                to: $AUTO_SDK"
        cmake --install "$ENGINE_BUILD" --prefix "$AUTO_SDK" || {
            echo "[build-project] ERROR: SDK install failed." >&2
            exit 10
        }
    fi
    if [[ -f "$AUTO_SDK/lib/cmake/JCE/JCEConfig.cmake" ]]; then
        SDK_DIR="$AUTO_SDK"
        USE_SDK=1
        JCE_CMAKE_DIR="$AUTO_SDK/lib/cmake/JCE"
        echo "[build-project] Using auto-installed SDK at: $AUTO_SDK"
    else
        echo "[build-project] ERROR: SDK install ran but JCEConfig.cmake" >&2
        echo "                still missing under $AUTO_SDK." >&2
        exit 11
    fi
fi

# ===================================================================
# SDK PATH
# ===================================================================
BUILD_DIR="$PROJECT_DIR/build/$PLATFORM-$ARCH-$VARIANT"
[[ "$DO_CLEAN" == "1" && -d "$BUILD_DIR" ]] && rm -rf "$BUILD_DIR"

case "$VARIANT" in
    debug) CMAKE_BUILD_TYPE=Debug ;;
    *)     CMAKE_BUILD_TYPE=Release ;;
esac

echo "[build-project] Configuring $TARGET ($PLATFORM/$ARCH/$VARIANT) with SDK at $SDK_DIR"
cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
      -DJCE_DIR="$JCE_CMAKE_DIR"

echo "[build-project] Building $TARGET"
cmake --build "$BUILD_DIR" --target "$TARGET"

EXE_PATH="$BUILD_DIR/$EXE_NAME"
[[ -f "$EXE_PATH" ]] || EXE_PATH="$BUILD_DIR/$CMAKE_BUILD_TYPE/$EXE_NAME"
if [[ ! -f "$EXE_PATH" ]]; then
    echo "[build-project] ERROR: build succeeded but artifact missing: $EXE_NAME" >&2
    echo "                Searched: $BUILD_DIR" >&2
    exit 7
fi

echo "[build-project] OK -> $EXE_PATH"
