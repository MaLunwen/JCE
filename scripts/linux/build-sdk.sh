#!/usr/bin/env bash
# Thin shim -> jce.py (cross-platform build driver). See scripts/jce.py.
# jce_common.sh installs the failure footer (beep + ":q to quit") via an ERR
# trap and sets REPO_ROOT; on success we call _jce_success_wait for the
# matching beep + ":q" countdown.
source "$(dirname "$0")/../lib/jce_common.sh"
python3 "$REPO_ROOT/scripts/jce.py" sdk "$@"
_jce_success_wait
