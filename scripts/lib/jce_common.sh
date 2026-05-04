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
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"
