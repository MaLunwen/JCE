#!/usr/bin/env bash
# ================================================================
# _beep.sh -- BIOS-style audio cue for build scripts.
#   Usage: "$(dirname "$0")/../_beep.sh" success
#          "$(dirname "$0")/../_beep.sh" fail
# success -> 1 short bell  (BIOS one-beep "POST OK")
# fail    -> 3 quick bells (BIOS error code "system error")
# Silent no-op if the terminal swallows BEL.
# ================================================================
case "${1:-}" in
    success)
        printf '\a'
        ;;
    fail)
        printf '\a'
        sleep 0.08 2>/dev/null || true
        printf '\a'
        sleep 0.08 2>/dev/null || true
        printf '\a'
        ;;
esac
exit 0
