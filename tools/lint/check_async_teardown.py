#!/usr/bin/env python3
"""
check_async_teardown.py — "the task is done" is not "its callback has run".

THE BUG THIS EXISTS FOR.  Seven sites in engine/ and editor/ tore an async
task down like this:

    (void)jce_async_task_cancel(task);
    jce_async_task_wait(task);
    jce_async_task_release(task);
    free(job);              /* <-- the completion callback reads this */

jce_async_task_wait* returns when the task reaches a TERMINAL STATE.  Terminal
state is published by the worker; the completion callback is queued and runs
later, from the owner-thread pump (async_pump_completions).  Every terminal
task is queued that way -- succeeded, failed, and CANCELLED alike -- and the
caller's release does not unlink it, because the executor holds a reference of
its own (refs starts at 2).

So the callback is still ahead of the caller at the moment the caller frees
the memory it reads.  Reproduced 2026-09-03 in test_jce_async: cancel, wait,
release, then pump -- the callback ran, after the owner had finished tearing
down.  jce_async_task_discard() exists for this: it disarms the callback
before returning.

WHAT IS CHECKED.  In engine/, editor/, tools/ and scripting/, the full
three-step idiom on one expression, in order, inside a short window:

    jce_async_task_cancel(X) ... jce_async_task_wait*(X) ... jce_async_task_release(X)

The CANCEL is the discriminator, and it is not decoration.  Its first cut
matched wait-then-release with no cancel, and reported a false positive within
the hour: jce_editor_wait_for_mesh_predecode retains the shard handles, waits
on them, and releases its own references -- it is not tearing anything down,
the completion callback must still run, and disarming it there would be a bug.
Cancelling first is what makes a wait a tear-down: the caller has decided the
result is not wanted and is about to free what produced it.  A gate that fires
on correct code gets switched off, so it asks for all three.

The cost of that precision: a tear-down that frees callback-owned memory
WITHOUT cancelling first is not reported, because it is syntactically
identical to the legitimate "wait for a result, then drop my handle".

This does NOT check for the other half of the same misunderstanding -- reading
state that the completion callback publishes, right after a wait -- because
that has no syntactic signature.  It bit the mesh-prewarm timer in the same
hour and could only be found by the number coming out zero.

Usage:
    python tools/lint/check_async_teardown.py
    python tools/lint/check_async_teardown.py --list
Exit 0 clean, 1 when a teardown still uses the unsafe idiom.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# Enumerate the WORKING TREE, not tracked-only: `git ls-files` hides a
# brand-new file until it is committed, so a defect introduced in a new file
# passes this checker on the commit that introduces it.  Measured: an internal
# function marked JCE_API sailed through a full run and failed on the next one,
# one commit late.  See tools/lint/lint_git_files.py.
import importlib.util as _ilu
_spec = _ilu.spec_from_file_location(
    "lint_git_files", str(Path(__file__).resolve().parent / "lint_git_files.py"))
_lgf = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(_lgf)


REPO_ROOT = Path(__file__).resolve().parents[2]

SCAN_PREFIXES = ("engine/", "editor/", "tools/", "scripting/")
SOURCE_SUFFIXES = (".c", ".cpp", ".h", ".hpp", ".inc")

# jce_async.c is the implementation of the primitive itself.
SKIP_FILES = ("engine/src/os/core/jce_async.c",)

CANCEL = re.compile(r"jce_async_task_cancel\s*\(\s*([^,;)]+?)\s*\)")
WAIT = re.compile(r"jce_async_task_wait(?:_timeout)?\s*\(\s*([^,;)]+)")
RELEASE = re.compile(r"jce_async_task_release\s*\(\s*([^,;)]+?)\s*\)")

# How far apart the three steps may sit and still read as one tear-down.
WINDOW = 6


def tracked() -> list:
    out = _lgf.working_tree()
    return [f for f in out
            if f.startswith(SCAN_PREFIXES) and f.endswith(SOURCE_SUFFIXES)
            and f not in SKIP_FILES]


def main() -> int:
    findings = []
    for rel in tracked():
        try:
            lines = (REPO_ROOT / rel).read_text(
                encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for i, line in enumerate(lines):
            mw = WAIT.search(line)
            if not mw:
                continue
            target = mw.group(1).strip()

            # A cancel of the same expression just above is what makes this a
            # tear-down rather than a wait for a result.
            cancelled = False
            for j in range(max(0, i - WINDOW), i):
                mc = CANCEL.search(lines[j])
                if mc and mc.group(1).strip() == target:
                    cancelled = True
                    break
            if not cancelled:
                continue

            for j in range(i + 1, min(i + 1 + WINDOW, len(lines))):
                mr = RELEASE.search(lines[j])
                if mr and mr.group(1).strip() == target:
                    findings.append((rel, i + 1, target))
                    break

    if "--list" in sys.argv:
        for rel, line, target in findings:
            print("%-58s %s" % ("%s:%d" % (rel, line), target))

    if findings:
        for rel, line, target in findings:
            print("  %s:%d cancels %s, waits on it, then releases it.  The "
                  "wait returns on terminal state, which is BEFORE the "
                  "completion callback runs, so whatever this tear-down frees "
                  "next is still owned by a queued callback.  Use "
                  "jce_async_task_discard(%s), which disarms it."
                  % (rel, line, target, target), file=sys.stderr)
        print("check_async_teardown: FAILED", file=sys.stderr)
        return 1

    print("check_async_teardown: OK (no cancel-wait-release tear-down; "
          "jce_async_task_discard is the one that disarms the callback)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
