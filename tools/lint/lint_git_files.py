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

  working_tree()  tracked PLUS untracked-not-ignored: what a commit is about to
                  contain.  This is the right set for a checker asking "is this
                  code correct" -- a rule about the code should not wait a
                  commit to apply to new code.

  tracked()       tracked only.  Correct for a checker comparing against a
                  RECORDED BASELINE (file-size ratchet, dedup counts, the ABI
                  snapshot): those baselines were taken over tracked files, and
                  counting untracked ones against them reports a regression
                  that a clean clone does not have.

--exclude-standard is what keeps build/, dist/ and every other ignored tree out
of working_tree(); without it this would hand a checker a build directory.
"""

import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def _run(args: list) -> list:
    try:
        out = subprocess.run(["git"] + args, cwd=REPO_ROOT,
                             capture_output=True, text=True, check=False)
    except OSError:
        return []
    if out.returncode != 0:
        return []
    return [line for line in out.stdout.split("\n") if line.strip()]


def tracked() -> list:
    """Repo-relative paths git has under version control."""
    return _run(["ls-files"])


def working_tree() -> list:
    """tracked() plus untracked-but-not-ignored: what a commit will contain."""
    seen, out = set(), []
    for rel in _run(["ls-files", "--cached", "--others", "--exclude-standard"]):
        if rel not in seen:
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
    problems = []
    if not t:
        problems.append("tracked() is empty -- git is not answering, and every "
                        "checker built on this would report a clean tree")
    if not t <= w:
        missing = sorted(t - w)[:5]
        problems.append("working_tree() does not contain tracked(): missing " +
                        ", ".join(missing))
    for rel in w - t:
        if rel.startswith(("build/", "dist/", ".git/")):
            problems.append("working_tree() leaked an ignored tree: " + rel)
    for p in problems:
        print("  " + p)
    if problems:
        print("lint_git_files: SELF-TEST FAIL - %d" % len(problems))
        return 1
    print("lint_git_files: OK (%d tracked, %d in the working tree, "
          "%d untracked-not-ignored)" % (len(t), len(w), len(w - t)))
    return 0


if __name__ == "__main__":
    raise SystemExit(self_test())
