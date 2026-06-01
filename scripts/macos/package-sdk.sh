#!/usr/bin/env bash
# ================================================================
# macos/package-sdk.sh -- Build and package the JCE SDK for macOS
#
# Usage:
#   package-sdk.sh [--arch x86_64|aarch64] [--variant release|dist|both]
#                  [--skip-debug] [--release-only]
# Arch aliases: x64=x86_64, arm64=aarch64
# Output: dist/sdk/darwin-<arch>/           (release variant)
#         dist/sdk/darwin-<arch>-dist/      (dist variant)
#
# release variant: patented codecs baked in; in-house use only.
# dist variant:    JCE_ENABLE_PATENTED_CODECS forced OFF; royalty-free.
#
# For release variant the matching build-macos-*.sh must already have
# been run (the script reuses the existing configured build tree).
# For dist variant a fresh build tree is always configured here.
#
# macOS `uname -m` returns "arm64" (Apple Silicon) or "x86_64" (Intel).
# ================================================================
source "$(dirname "$0")/../lib/jce_common.sh"

ARCH=""
VARIANT="both"
SKIP_DEBUG=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)         ARCH="${2:-}"; shift 2 ;;
        --variant)      VARIANT="${2:-}"; shift 2 ;;
        --skip-debug)   SKIP_DEBUG=1; shift ;;
        --release-only) VARIANT="release"; SKIP_DEBUG=1; shift ;;
        *) echo "[package-sdk] unknown argument: $1"; exit 2 ;;
    esac
done

# Auto-detect arch from host when not specified.
if [[ -z "$ARCH" ]]; then
    _HOST_ARCH="$(uname -m)"
    case "$_HOST_ARCH" in
        arm64|aarch64) ARCH="aarch64" ;;
        x86_64)        ARCH="x86_64"  ;;
        *)
            echo "ERROR: Unsupported build-machine architecture: $_HOST_ARCH"
            exit 1 ;;
    esac
fi

# Normalise arch (explicit case matching works on bash 3 and 5).
case "$ARCH" in
    x64|x86_64|amd64)
        ARCH="x86_64"; ENG_ARCH="x64"
        CONAN_PROFILE="conan/profiles/macos-x64"
        OSX_ARCH="x86_64"
        OSX_DEPLOYMENT_TARGET="10.15" ;;
    arm64|aarch64)
        ARCH="aarch64"; ENG_ARCH="arm64"
        CONAN_PROFILE="conan/profiles/macos-arm64"
        OSX_ARCH="arm64"
        OSX_DEPLOYMENT_TARGET="11.0" ;;
    *)
        echo "[package-sdk] unsupported --arch '$ARCH'"
        echo "               expected: x86_64|aarch64  (aliases: x64|arm64)"
        exit 2 ;;
esac

# Build machine profile — auto-detect from current host.
_BUILD_UNAME="$(uname -m)"
case "$_BUILD_UNAME" in
    x86_64)       BUILD_PROFILE="conan/profiles/macos-x64"   ;;
    arm64|aarch64) BUILD_PROFILE="conan/profiles/macos-arm64" ;;
    *)
        echo "ERROR: Unsupported build-machine architecture: $_BUILD_UNAME"
        exit 1 ;;
esac

CONAN_DIR="build/desktop/darwin-${ENG_ARCH}-conan"
CONAN_TOOLCHAIN_REL="$CONAN_DIR/build/Release/generators/conan_toolchain.cmake"
CONAN_TOOLCHAIN_DBG="$CONAN_DIR/build/Debug/generators/conan_toolchain.cmake"

# Use sysctl (macOS) instead of nproc (Linux).
_NJOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"

DO_RELEASE=0; DO_DIST=0
case "$VARIANT" in
    release) DO_RELEASE=1 ;;
    dist)    DO_DIST=1 ;;
    both)    DO_RELEASE=1; DO_DIST=1 ;;
    *)
        echo "[package-sdk] unsupported --variant '$VARIANT'"
        echo "               expected: release|dist|both"
        exit 2 ;;
esac

echo "[package-sdk] root:       $REPO_ROOT"
echo "[package-sdk] arch:       $ARCH  (engine-tree: $ENG_ARCH)"
echo "[package-sdk] osx_target: $OSX_DEPLOYMENT_TARGET"
echo "[package-sdk] variant:    $VARIANT"
echo "[package-sdk] debug:      $SKIP_DEBUG  (0=include, 1=skip)"
echo ""

pack_variant() {
    local v_name="$1"    # release | dist
    local v_suffix="$2"  # "" | "-dist"

    local build_rel="build/desktop/darwin-${ENG_ARCH}${v_suffix}"
    local build_dbg="build/desktop/darwin-${ENG_ARCH}${v_suffix}-debug"
    local install_dir="dist/sdk/darwin-${ARCH}${v_suffix}"
    local patented_flag=""
    [[ "$v_name" == "dist" ]] && patented_flag="-DJCE_ENABLE_PATENTED_CODECS=OFF"

    echo ""
    echo "==============================================================="
    echo "[package-sdk] [$v_name] BUILD_REL = $build_rel"
    echo "[package-sdk] [$v_name] INSTALL   = $install_dir"
    echo "[package-sdk] [$v_name] PATENTED  = ${patented_flag:-default ON}"
    echo "==============================================================="

    # ---- Release build ----
    if [[ ! -f "$build_rel/CMakeCache.txt" ]]; then
        if [[ "$v_name" == "release" ]]; then
            echo "[package-sdk] Release build dir missing: $build_rel"
            echo "[package-sdk] Run scripts/macos/build-macos-${ENG_ARCH}.sh first."
            exit 1
        fi
        # dist variant: we own the build tree, run Conan + fresh configure.
        if [[ ! -f "$CONAN_TOOLCHAIN_REL" ]]; then
            echo "[package-sdk] [$v_name] Conan install ($CONAN_PROFILE, Release)"
            conan install . -pr:h "$CONAN_PROFILE" -pr:b "$BUILD_PROFILE" \
                --output-folder="$CONAN_DIR" --build=missing
        fi
        [[ -f "$CONAN_TOOLCHAIN_REL" ]] || {
            echo "ERROR: Conan toolchain not found: $CONAN_TOOLCHAIN_REL"; exit 1
        }
        echo "[package-sdk] [$v_name] configuring fresh Release build tree"
        # shellcheck disable=SC2086
        cmake -S . -B "$build_rel" -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE="$CONAN_TOOLCHAIN_REL" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_OSX_ARCHITECTURES="$OSX_ARCH" \
            -DCMAKE_OSX_DEPLOYMENT_TARGET="$OSX_DEPLOYMENT_TARGET" \
            -DJCE_BUILD_VARIANT="$v_name" \
            $patented_flag \
            -DJCE_ENABLE_SDK_INSTALL=ON \
            -DCMAKE_INSTALL_PREFIX="$install_dir"
    else
        echo "[package-sdk] [$v_name] reconfiguring existing Release tree"
        # shellcheck disable=SC2086
        cmake "$build_rel" \
            -DCMAKE_OSX_ARCHITECTURES="$OSX_ARCH" \
            -DCMAKE_OSX_DEPLOYMENT_TARGET="$OSX_DEPLOYMENT_TARGET" \
            $patented_flag \
            -DJCE_ENABLE_SDK_INSTALL=ON \
            -DCMAKE_INSTALL_PREFIX="$install_dir"
    fi

    echo "[package-sdk] [$v_name] building Release fat lib"
    cmake --build "$build_rel" --target jce_sdk_fat_lib -j "$_NJOBS"
    echo "[package-sdk] [$v_name] installing Release"
    cmake --install "$build_rel"

    # ---- Debug build (optional) ----
    if [[ "$SKIP_DEBUG" == "0" ]]; then
        if [[ ! -f "$build_dbg/CMakeCache.txt" ]]; then
            if [[ "$v_name" == "release" ]]; then
                echo "[package-sdk] Debug build dir missing: $build_dbg"
                echo "[package-sdk] Run the matching debug build first, or pass --skip-debug."
                exit 1
            fi
            if [[ ! -f "$CONAN_TOOLCHAIN_DBG" ]]; then
                echo "[package-sdk] [$v_name] Conan install ($CONAN_PROFILE, Debug)"
                conan install . -pr:h "$CONAN_PROFILE" -pr:b "$BUILD_PROFILE" \
                    --output-folder="$CONAN_DIR" --build=missing \
                    -s build_type=Debug
            fi
            [[ -f "$CONAN_TOOLCHAIN_DBG" ]] || {
                echo "ERROR: Conan toolchain not found: $CONAN_TOOLCHAIN_DBG"; exit 1
            }
            echo "[package-sdk] [$v_name] configuring fresh Debug build tree"
            # shellcheck disable=SC2086
            cmake -S . -B "$build_dbg" -G Ninja \
                -DCMAKE_TOOLCHAIN_FILE="$CONAN_TOOLCHAIN_DBG" \
                -DCMAKE_BUILD_TYPE=Debug \
                -DCMAKE_OSX_ARCHITECTURES="$OSX_ARCH" \
                -DCMAKE_OSX_DEPLOYMENT_TARGET="$OSX_DEPLOYMENT_TARGET" \
                -DJCE_BUILD_VARIANT="$v_name" \
                $patented_flag \
                -DJCE_ENABLE_SDK_INSTALL=ON \
                -DCMAKE_INSTALL_PREFIX="$install_dir"
        else
            echo "[package-sdk] [$v_name] reconfiguring existing Debug tree"
            # shellcheck disable=SC2086
            cmake "$build_dbg" \
                -DCMAKE_OSX_ARCHITECTURES="$OSX_ARCH" \
                -DCMAKE_OSX_DEPLOYMENT_TARGET="$OSX_DEPLOYMENT_TARGET" \
                $patented_flag \
                -DJCE_ENABLE_SDK_INSTALL=ON \
                -DCMAKE_INSTALL_PREFIX="$install_dir"
        fi
        echo "[package-sdk] [$v_name] building Debug fat lib"
        cmake --build "$build_dbg" --target jce_sdk_fat_lib -j "$_NJOBS"
        echo "[package-sdk] [$v_name] installing Debug"
        cmake --install "$build_dbg"
    fi

    # ---- VERSION.txt ----
    local sha; sha="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    mkdir -p "$install_dir"
    {
        echo "JCE SDK build"
        echo "commit:  $sha"
        echo "host:    darwin-$ARCH"
        echo "variant: $v_name"
    } > "$install_dir/VERSION.txt"

    echo "[package-sdk] [$v_name] done -> $install_dir"
}

[[ "$DO_RELEASE" == "1" ]] && pack_variant release ""
[[ "$DO_DIST"    == "1" ]] && pack_variant dist "-dist"

echo ""
echo "[package-sdk] [SUCCESS]"
[[ "$DO_RELEASE" == "1" ]] && echo "[package-sdk]   release SDK: $REPO_ROOT/dist/sdk/darwin-$ARCH"
[[ "$DO_DIST"    == "1" ]] && echo "[package-sdk]   dist    SDK: $REPO_ROOT/dist/sdk/darwin-$ARCH-dist"

_jce_success_wait
