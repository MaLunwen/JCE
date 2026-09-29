#!/usr/bin/env python3
"""
check_private_tech_untracked.py -- unpublished work must stay unpublished.

WHAT THIS PROTECTS.  `private/` holds the AI authoring work that is not ready
to publish: scene design, AI-assisted physics, and the CLI loop that drives a
whole project lifecycle through the editor and the SDK.  The MECHANISM those
sit on is public and stays public -- jce_llm.h treats a provider as a PROGRAM,
so the engine never needs to know what the program does -- but the policy is
not, and this repository is public.

WHY A GATE AND NOT JUST A .gitignore RULE.  Three reasons, and none of them is
hypothetical in this tree:

  1. `git add -A` is one keystroke, `git add -f` is two more, and a file added
     once is in the history permanently.  Removing it afterwards is a history
     rewrite and a force-push, not a revert -- .gitignore itself records that
     lesson about a 77 MB commercial audio file.

  2. .gitignore rules get edited.  The rule this depends on could be deleted,
     narrowed, or shadowed by a later negation, and nothing would say so --
     this file has already recorded an unanchored `tests/` swallowing three
     other suites and a blanket `.*` swallowing CI.  So the rule's EXISTENCE is
     checked here too, not only its effect.

  3. The failure is silent and delayed.  A leaked file does not break a build.
     It sits there until somebody reads the repository.

TWO RULES:

  1. `git ls-files private/` must be EMPTY.  This is the effect.

  2. `.gitignore` must still contain an anchored `/private/` rule.  This is the
     cause.  Checking only the effect would pass on an empty working tree with
     the rule already deleted -- green today, leaking the moment the directory
     is recreated.

NOT CHECKED, said here rather than left to be discovered: whether the content
under private/ is ACTUALLY unpublishable, and whether something equally
sensitive has been written somewhere else entirely.  This gate enforces a
boundary; it cannot decide what belongs on which side of it.
"""
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PRIVATE_DIR = "private/"
RULE = "/private/"

failures = []


def main():
    gitignore = REPO_ROOT / ".gitignore"
    if not gitignore.is_file():
        print("check_private_tech_untracked: FAIL - .gitignore is missing.",
              file=sys.stderr)
        return 1

    # Rule 2 (the cause) -- an uncommented, anchored rule.
    rules = [ln.strip() for ln in
             gitignore.read_text(encoding="utf-8", errors="replace").splitlines()
             if ln.strip() and not ln.strip().startswith("#")]
    if RULE not in rules:
        failures.append(
            ".gitignore no longer carries an anchored `%s` rule. Checking only "
            "whether the directory is tracked TODAY would pass on a tree where "
            "it happens to be absent -- and leak everything the moment it is "
            "recreated." % RULE)

    # A later negation would re-include it, which the effect check below would
    # only catch after the fact.
    for r in rules:
        if r.startswith("!") and "private" in r:
            failures.append(
                ".gitignore has a negation that re-includes the private tree: "
                "`%s`." % r)

    # Rule 1 (the effect).
    out = subprocess.run(["git", "ls-files", "--", PRIVATE_DIR],
                         cwd=REPO_ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        print("check_private_tech_untracked: FAIL - `git ls-files` failed; the "
              "scan is broken, not the tree.", file=sys.stderr)
        return 1

    leaked = [p for p in out.stdout.splitlines() if p.strip()]
    if leaked:
        failures.append(
            "%d file(s) under %s are TRACKED. They are in the repository and, "
            "once pushed, in its history permanently -- removing them needs a "
            "rewrite and a force-push, not a revert:\n      %s%s"
            % (len(leaked), PRIVATE_DIR, "\n      ".join(leaked[:10]),
               "\n      ... and %d more" % (len(leaked) - 10)
               if len(leaked) > 10 else ""))

    if failures:
        print("check_private_tech_untracked: FAIL - %d problem(s):"
              % len(failures), file=sys.stderr)
        for f in failures:
            print("  " + f, file=sys.stderr)
        print("  If a file under %s is meant to be public, MOVE it out of that "
              "directory rather than force-adding it in place." % PRIVATE_DIR,
              file=sys.stderr)
        return 1

    present = (REPO_ROOT / "private").is_dir()
    print("check_private_tech_untracked: OK (`%s` rule present; 0 tracked "
          "file(s) under %s; the directory %s on this machine)"
          % (RULE, PRIVATE_DIR,
             "exists" if present else "does not exist yet"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
