#!/usr/bin/env bash
# ================================================================
# lib/jce_common.sh -- shared bootstrap for every build script.
#
#   Source this from the very top of a build script:
#       #!/usr/bin/env bash
#       source "$(dirname "$0")/../lib/jce_common.sh"
#
# Provides:
#   * `set -euo pipefail`
#   * `_jce_on_fail` (auto-installed via `trap ... ERR`)
#       - 1 BIOS beep × 3 + 15s countdown w/ pause-on-keypress, ":q" to quit
#   * `_jce_success_wait` (call after final SUCCESS line)
#       - 1 BIOS beep + 3s countdown, ":q" to quit
#   * SCRIPT_DIR -- absolute dir of the calling script (linux/, macos/, ...)
#   * REPO_ROOT  -- absolute repo root (two levels up from SCRIPT_DIR)
#   * cwd is changed to REPO_ROOT
#
# Pure POSIX shell utilities only — works on any host with bash.
# ================================================================
set -euo pipefail

# On macOS, SSH sessions inherit a minimal PATH (/usr/bin:/bin:/usr/sbin:/sbin).
# Extend it here so every build script gets conan, cmake, ninja, etc.
if [[ "$(uname -s)" == "Darwin" ]]; then
    # Homebrew (Apple Silicon at /opt/homebrew, Intel at /usr/local)
    for _brew_bin in /opt/homebrew/bin /usr/local/bin; do
        [[ -d "$_brew_bin" ]] && PATH="$_brew_bin:$PATH"
    done
    # pip-installed tools (conan lives here when installed via pip)
    for _py_bin in "$HOME"/Library/Python/*/bin; do
        [[ -d "$_py_bin" ]] && PATH="$_py_bin:$PATH"
    done
    export PATH
    unset _brew_bin _py_bin
fi

# Absolute path of the directory containing THIS file (works when sourced).
_JCE_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "${JCE_FAIL_TOKEN:-}" ]]; then
    JCE_FAIL_TOKEN="$(date +%s)-$$"
    export JCE_FAIL_TOKEN
fi
_JCE_FAIL_FILE="/tmp/jce_fail_${JCE_FAIL_TOKEN}"
export _JCE_FAIL_FILE

_jce_on_fail() {
    if [[ -f "$_JCE_FAIL_FILE" ]]; then
        return
    fi
    : >"$_JCE_FAIL_FILE" 2>/dev/null || true
    trap - ERR
    "$_JCE_LIB_DIR/jce_beep.sh" fail || true
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
    "$_JCE_LIB_DIR/jce_beep.sh" success
    [[ -t 1 ]] || return 0
    local t=3 e=0 key="" key2=""
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

# Resolve caller-script-relative paths.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$_JCE_LIB_DIR/../.." && pwd)"
cd "$REPO_ROOT"

# ----------------------------------------------------------------------------
# jce_shaderc [require] -- locate a host shaderc and set JCE_SHADERC.
#
# The shell twin of `lib/jce_build_common.bat shaderc`; keep the two search
# orders identical.
#
# Deliberately NOT via build/host-conan's generator files.  Those record the
# package folder of the conan install that WROTE them, and Conan garbage-
# collects package folders.  Measured 2026-08-27 on the Windows side: three
# build scripts each carried their own copy of that approach, all parsed
# bgfx-release-*-data.cmake correctly, and all landed on a package directory
# that no longer existed while the live cache held six shaderc copies.  A miss
# then compiled zero shaders, packed an empty pak, and failed hundreds of lines
# later with "generated archive failed BOM integrity audit".
#
# "require" makes a miss fatal HERE instead.
# ----------------------------------------------------------------------------
jce_shaderc() {
    local require="${1:-}"
    local conan all cand pat
    JCE_SHADERC=""

    # 1. An explicit override always wins.
    if [[ -n "${JCE_SHADERC_EXECUTABLE:-}" && -f "${JCE_SHADERC_EXECUTABLE}" ]]; then
        JCE_SHADERC="${JCE_SHADERC_EXECUTABLE}"
    fi

    # 2. The LIVE Conan cache, preferring a build tree over the packaged copy.
    conan="${CONAN_HOME:-$HOME/.conan2}/p/b"
    if [[ -z "$JCE_SHADERC" && -d "$conan" ]]; then
        all=$(find "$conan" -type f \( -name shaderc -o -name shaderc.exe \) 2>/dev/null | grep -i bgfx || true)
        for pat in '/b/build/Debug/' '/b/build/Release/' '/p/bin/'; do
            cand=$(printf '%s\n' "$all" | grep -F -- "$pat" | head -1 || true)
            if [[ -n "$cand" ]]; then JCE_SHADERC="$cand"; break; fi
        done
    fi

    # 3. An installed SDK ships one in bin/.
    if [[ -z "$JCE_SHADERC" && -d "${REPO_ROOT}/dist/sdk" ]]; then
        cand=$(find "${REPO_ROOT}/dist/sdk" -type f -path '*/bin/*' \( -name shaderc -o -name shaderc.exe \) 2>/dev/null | head -1 || true)
        if [[ -n "$cand" ]]; then JCE_SHADERC="$cand"; fi
    fi

    # 4. A host-tools build, if one was made.
    if [[ -z "$JCE_SHADERC" ]]; then
        for cand in "${REPO_ROOT}/build/host/tools/shaderc" "${REPO_ROOT}/build/host/tools/shaderc.exe"; do
            if [[ -f "$cand" ]]; then JCE_SHADERC="$cand"; break; fi
        done
    fi

    if [[ -n "$JCE_SHADERC" ]]; then
        echo "  Using shaderc: $JCE_SHADERC"
        return 0
    fi
    if [[ "$require" == "require" ]]; then
        echo "ERROR: host shaderc not found, and this target needs it." >&2
        echo "       Without it zero shaders are compiled and the build fails much" >&2
        echo "       later with an unrelated-looking pak integrity error." >&2
        echo "       Searched: JCE_SHADERC_EXECUTABLE, ${conan}," >&2
        echo "                 ${REPO_ROOT}/dist/sdk/*/bin, ${REPO_ROOT}/build/host/tools" >&2
        echo "       Fix: build the SDK once (python scripts/jce.py sdk), or set" >&2
        echo "       JCE_SHADERC_EXECUTABLE to an existing shaderc binary." >&2
        return 1
    fi
    echo "  WARNING: shaderc not found; shaders will not be compiled."
    return 0
}
