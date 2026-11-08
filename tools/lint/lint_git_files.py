#!/usr/bin/env python3
"""
lint_git_files.py — which files a checker should look at.

THE TRAP THIS EXISTS FOR.  Sixteen checkers enumerate their input with
`git ls-files`, which lists TRACKED files only.  A brand-new file is therefore
invisible to them until it is committed -- so a defect introduced in a new file
PASSES on the commit that introduces it, and only surfaces on the next run,
attributed to whatever came after.

Measured, not theorised (2026-09-05): engine/src/application/jce_engine_frame_cap.h
declared an internal function JCE_API.  check_exported_symbol_declared.py exists
for exactly that and reported OK across a full run immediately before the commit
that added the file -- because at that moment the file was untracked.  It failed
on the very next run, one commit too late.  The one thing that gate is for, on
the one commit where it mattered.

TWO SETS, AND THE DIFFERENCE IS THE QUESTION BEING ASKED.

  working_tree()  tracked PLUS untracked-not-ignored, MINUS Git-declared working
                  tree deletions. This is the right set for "is this
                  code correct" -- a rule about the code should not wait a
                  commit to apply to new code.

  tracked()       tracked only.  Correct for a checker comparing against a
                  RECORDED BASELINE (file-size ratchet, dedup counts, the ABI
                  snapshot): those baselines were taken over tracked files, and
                  counting untracked ones against them reports a regression
                  that a clean clone does not have.

--exclude-standard is what keeps build/, dist/ and every other ignored tree out
of working_tree(); without it this would hand a checker a build directory.
Call working_tree(strict=True) when an unavailable or incomplete Git inventory
must fail the caller instead of retaining the legacy empty-list behavior.
"""

import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


class GitInventoryError(RuntimeError):
    """Git could not provide the requested source inventory."""


def _run(args: list, *, strict: bool = False) -> list:
    try:
        out = subprocess.run(["git"] + args, cwd=REPO_ROOT,
                             capture_output=True, text=True, check=False,
                             **({"encoding": "utf-8"} if strict else {}))
    except OSError as error:
        if strict:
            raise GitInventoryError(f"git {' '.join(args)} failed: {error}") from error
        return []
    if out.returncode != 0:
        if strict:
            raise GitInventoryError(f"git {' '.join(args)} failed: {out.stderr.strip()}")
        return []
    separator = "\0" if "-z" in args else "\n"
    return [line for line in out.stdout.split(separator) if line.strip()]


def tracked() -> list:
    """Repo-relative paths git has under version control."""
    return _run(["ls-files"])


def _working_tree_deleted(*, strict: bool = False) -> list:
    """Index entries Git explicitly reports deleted from the working tree."""
    return _run(["ls-files", "--deleted"] + (["-z"] if strict else []), strict=strict)


def working_tree(*, strict: bool = False) -> list:
    """Current maintained files; tracked baseline enumeration stays unchanged."""
    # --cached retains unstaged deletions after a source owner moves. Exclude
    # only Git-declared deletions, never arbitrary missing/unreadable inputs:
    # source readers must still fail if an admitted file cannot be consumed.
    deleted = set(_working_tree_deleted(strict=strict))
    seen, out = set(), []
    args = ["ls-files", "--cached", "--others", "--exclude-standard"]
    for rel in _run(args + (["-z"] if strict else []), strict=strict):
        if rel not in deleted and rel not in seen:
            seen.add(rel)
            out.append(rel)
    return out


def self_test() -> int:
    """The two sets must differ in the one way that matters, and only that way.

    A helper whose whole reason is "these two enumerations are not the same"
    should say so out loud rather than be believed.  Runs on import-free
    invocation: python tools/lint/lint_git_files.py
    """
    t, w = set(tracked()), set(working_tree())
    deleted = set(_working_tree_deleted())
    problems = []
    if not t:
        problems.append("tracked() is empty -- git is not answering, and every "
                        "checker built on this would report a clean tree")
    if not t - deleted <= w:
        missing = sorted(t - deleted - w)[:5]
        problems.append("working_tree() omits tracked files not declared deleted: " +
                        ", ".join(missing))
    if deleted & w:
        problems.append("working_tree() retained Git-declared deleted files: " +
                        ", ".join(sorted(deleted & w)[:5]))
    for rel in w - t:
        if rel.startswith(("build/", "dist/", ".git/")):
            problems.append("working_tree() leaked an ignored tree: " + rel)
    for p in problems:
        print("  " + p)
    if problems:
        print("lint_git_files: SELF-TEST FAIL - %d" % len(problems))
        return 1
    print("lint_git_files: OK (%d tracked, %d in the working tree, "
          "%d untracked-not-ignored, %d Git-declared deletions)" %
          (len(t), len(w), len(w - t), len(deleted)))
    return 0


if __name__ == "__main__":
    raise SystemExit(self_test())
