#!/usr/bin/env python3
"""
run_dedup_audit.py — single entry point for the JCE engine/editor
deduplication audit (.docs/way/JCE_ENGINE_EDITOR_DEDUP_REFACTOR_AUDIT_PLAN_STRICT.md §7.4).

Design note — this runner deliberately does NOT reimplement checks that the
repo already owns.  `tools/lint/` already ships the boundary enforcers the
audit plan asks for (layer dependencies, editor-consumer purity == the
"editor bypasses the runtime" check, platform-macro containment, raw
allocator/native-IO bans).  Re-writing them under tools/audit/ would create
exactly the duplicate-authority problem this audit exists to remove, so they
are *invoked* here instead.  Only genuinely new dedup detectors live in
tools/audit/.

Gates (boundary — hard fail, these already pass today):
  tools/lint/check_layer_dependencies.py      low layer must not include high
  tools/lint/check_editor_consumer_purity.py  editor reaches engine only via <jce/...>
  tools/lint/check_public_api_purity.py       public headers stay C99/ABI-clean
  tools/lint/check_platform_macros.py         platform macros confined to os/platform
  tools/lint/check_engine_native_io.py        no raw fopen/printf in the engine
  tools/lint/check_raw_allocator.py           no raw malloc/free in the engine
  tools/audit/check_single_decoder.py           image decode only in jce_image

Detectors (dedup — advisory by default, baseline-gated with --check):
  tools/audit/find_duplicate_symbols.py         same function name in 2+ TUs
  tools/audit/find_similar_code.py              token-identical function bodies
  tools/audit/check_singleton_ownership.py      file-scope mutable global state

Usage:
  python tools/audit/run_dedup_audit.py                 # full report
  python tools/audit/run_dedup_audit.py --check         # CI: non-zero on regression
  python tools/audit/run_dedup_audit.py --gates-only    # boundary gates only (fast)
  python tools/audit/run_dedup_audit.py --write-baseline # record current counts
"""

from __future__ import annotations

import argparse
import json
import re
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LINT_DIR = REPO_ROOT / "tools" / "lint"
AUDIT_DIR = REPO_ROOT / "tools" / "audit"
BASELINE_PATH = AUDIT_DIR / "dedup_baseline.json"

# Boundary gates: already-owned lint scripts. Non-zero exit == violation.
GATES = [
    ("layer-dependencies", LINT_DIR / "check_layer_dependencies.py"),
    ("editor-consumer-purity", LINT_DIR / "check_editor_consumer_purity.py"),
    ("public-api-purity", LINT_DIR / "check_public_api_purity.py"),
    ("platform-macros", LINT_DIR / "check_platform_macros.py"),
    ("engine-native-io", LINT_DIR / "check_engine_native_io.py"),
    ("raw-allocator", LINT_DIR / "check_raw_allocator.py"),
    # New, and a gate rather than a detector: the second image decoder is
    # DELETED, not merely counted down, so any reappearance is a hard failure.
    # It is a single-owner rule at FILE granularity, which the directory-scoped
    # whitelists in dependency-ownership.yml cannot express.
    ("single-image-decoder", AUDIT_DIR / "check_single_decoder.py"),
]

# Dedup detectors: emit a count we track against a baseline so CI can block
# *new* duplication without demanding the existing debt be zero on day one.
DETECTORS = [
    ("duplicate-symbols", AUDIT_DIR / "find_duplicate_symbols.py", [],
     re.compile(r"(\d+)\s+hard-signal")),
    ("similar-code", AUDIT_DIR / "find_similar_code.py", ["--min-tokens", "40"],
     re.compile(r"(\d+)\s+clone group")),
    ("global-state", AUDIT_DIR / "check_singleton_ownership.py", [],
     re.compile(r"(\d+)\s+declaration")),
]


# CPython's own message for a corrupt compiled-pattern state inside the sre
# engine.  No pattern in this repository can request it and no checker's source
# can raise it -- it is the environment fault recorded below wearing a
# traceback instead of dying silently.  Deliberately the ONLY loud signature
# here: a TypeError or an AssertionError could be a real defect, and a retry
# rule that swallowed those would be the thing this machinery exists to catch.
# Each entry is a signature: a string that must appear, or a TUPLE of strings
# that must ALL appear.  The tuple form exists so a signature can be pinned to
# the stdlib frame that raised it -- see the ast one below, whose exception
# TYPE is ordinary and whose LOCATION is not.
_INTERPRETER_FAULTS = (
    "RuntimeError: internal error in regular expression engine",
    # CPython walking an AST it has just built and finding a LIST where a node
    # type's _fields must hold strings.  _fields is a tuple of str on the node
    # CLASS; a checker can choose which tree to walk and nothing else, so this
    # says the node object is not what the interpreter put there.  Pinned to
    # ast.py on purpose: `TypeError: attribute name must be string` raised from
    # a checker's own getattr WOULD be a real defect, and this rule must not
    # cover that.
    ("ast.py", "TypeError: attribute name must be string, not"),
)


def _fault_matches(text):
    """True when `text` carries one of the signatures above."""
    for sig in _INTERPRETER_FAULTS:
        if isinstance(sig, tuple):
            if all(s in text for s in sig):
                return True
        elif sig in text:
            return True
    return False


def _crash_shaped(rc, out, skip_code):
    """A failure with nothing to say -- or nothing its source could say -- is
    the process dying, not a finding.

    KEEP IN SYNC with the identical copy in tools/lint/run_all.py and tools/audit/run_architecture_audit.py."""
    if rc in (0, skip_code):
        return False
    text = (out or "").strip()
    if not text:
        return True
    return _fault_matches(text)


# THE CHILDREN DO NOT WRITE BYTECODE, AND A CRASHED CHILD IS RE-RUN ONCE.
#
# Six gate runs in one session failed with results that cannot be true:
# "[FAIL] editor-consumer-purity" with NO output at all (three times), a
# TypeError from check_raw_allocator.py claiming `list.append` was "a generator
# object [that] is not callable", and i18n_audit.py exiting 3221225477 --
# 0xC0000005, an ACCESS VIOLATION in the interpreter.  Every one passed when
# re-run alone, and none is a bug the source can have.
#
# PYTHONDONTWRITEBYTECODE WAS THE FIRST GUESS AND IT WAS WRONG: the access
# violation happened with it already set.  It is kept anyway (a runner whose
# ~20 short children race on one __pycache__ is worse for no gain), but the
# cause is upstream of this repository -- something in this Windows environment
# kills a freshly spawned CPython often enough to be seen several times an hour
# under a burst of ~20 spawns.
#
# SO A CRASH-SHAPED FAILURE IS RE-RUN ONCE, AND THE RETRY IS ANNOUNCED.
# Crash-shaped means EMPTY OUTPUT with an exit code that is neither success nor
# the SKIPPED convention: every gate here prints before it fails, so a silent
# failure carries no finding to lose.  A checker that prints anything is never
# retried, and a second crash is reported as a failure -- this makes a flaky
# environment legible, it does not make a red gate green.
#
# Five gate runs in one session failed with results that cannot be true:
# "[FAIL] editor-consumer-purity" with NO output at all (three times), and a
# TypeError from check_raw_allocator.py claiming `list.append` was "a generator
# object [that] is not callable".  Every one passed when re-run alone, and the
# last one is not a bug that source can have.
#
# THIS IS NOT A PROVEN DIAGNOSIS.  What is known: these runners spawn ~20 short
# python children, often while another python is running in the same tree, and
# the one piece of shared MUTABLE state those children all touch is
# __pycache__.  A torn .pyc explains both shapes -- an interpreter that dies
# before printing, and one that runs something that is not the source.  So the
# shared thing is removed from the picture rather than reasoned about; the cost
# is re-compiling a handful of small scripts per run, which is noise next to
# what they do.
#
# If a spurious failure recurs after this, the cache was not it, and that is
# worth more than a guess that happened to be followed by quiet.
def run(script: Path, extra: list[str] | None = None) -> tuple[int, str]:
    if not script.is_file():
        return 127, f"(missing: {script})"
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"

    def _spawn():
        pr = subprocess.run(
            [sys.executable, str(script), *(extra or [])],
            cwd=REPO_ROOT, capture_output=True, text=True,
            encoding="utf-8", errors="replace",
            env=env,
        )
        return pr.returncode, ((pr.stdout or "") + (pr.stderr or "")).rstrip()

    rc, out = _spawn()
    if _crash_shaped(rc, out, 127):
        print(f"  [retry] {script.name} exited {rc} with no output -- "
              f"crash-shaped, re-running once", flush=True)
        rc, out = _spawn()
    return rc, out


def load_baseline() -> dict:
    try:
        d = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
        # "_reasons" is metadata, not a detector: strip it so a future
        # detector named _reasons is the only way this could collide, and so
        # the compare loop below never treats it as a count.
        d.pop("_reasons", None)
        return d
    except (OSError, ValueError):
        return {}


def load_reasons() -> dict:
    """detector -> why its count was RAISED, recorded by --write-baseline.

    Separate from the counts so the compare loop never sees a non-integer, and
    so an old baseline with no reasons loads exactly as before."""
    try:
        d = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
        r = d.get("_reasons")
        return r if isinstance(r, dict) else {}
    except (OSError, ValueError):
        return {}


def main() -> int:
    ap = argparse.ArgumentParser()
    # --check USED TO BE THE OPT-IN, and nothing opted in.  This runner had no
    # caller anywhere -- not run_all.py, not run_architecture_audit.py, only a
    # gitignored plan document -- so the flag that makes it fail was never
    # passed.  Run by hand it printed "DUPLICATION REGRESSIONS ... Fix it" and
    # then exited 0.  Failing is now the default and --report-only is the
    # opt-out, because a tool that announces a regression and returns success
    # is worse than no tool: it reads like a clean run to anything that checks
    # an exit code, including a person.
    ap.add_argument("--check", action="store_true",
                    help="(accepted for compatibility; failing is now default)")
    ap.add_argument("--report-only", action="store_true",
                    help="print findings and always exit 0")
    ap.add_argument("--gates-only", action="store_true")
    ap.add_argument("--write-baseline", action="store_true")
    ap.add_argument("--reason", default="",
                    help="why a RAISED count is justified; required to raise one, stored beside the number")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    failed_gates: list[str] = []
    print("=" * 72)
    print("JCE dedup audit — boundary gates")
    print("=" * 72)
    for name, script in GATES:
        rc, out = run(script)
        # A CHECKER THAT SAID NOTHING DID NOT FIND ANYTHING.  Every gate here
        # prints on the way to a non-zero exit -- there is no silent `return 1`
        # in any of them -- so a failure with EMPTY output is the process
        # failing to run, not a violation.  Reported three times in one session
        # as a bare "[FAIL] editor-consumer-purity" that passed when re-run by
        # hand, which costs whoever sees it an investigation into their own
        # change.  run_architecture_audit.py already separates "CRASHED ... this
        # is not an architecture finding" at the top level; this is the same
        # distinction one layer down.
        crashed = rc not in (0, 127) and not out.strip()
        status = ("PASS" if rc == 0 else "SKIP" if rc == 127
                  else "CRASH" if crashed else "FAIL")
        print(f"  [{status}] {name}")
        if crashed:
            print(f"        the checker exited {rc} and printed NOTHING.  Every "
                  f"gate here prints before it fails, so this is the process "
                  f"not running -- re-run it on its own before believing it:")
            print(f"        python {script.relative_to(REPO_ROOT).as_posix()}")
        if rc not in (0, 127):
            failed_gates.append(name)
        if args.verbose or (rc not in (0, 127) and not crashed):
            for line in out.splitlines():
                print(f"        {line}")

    if args.gates_only:
        print()
        if failed_gates:
            print(f"gates: FAILED ({', '.join(failed_gates)})")
            return 1
        print("gates: all PASS")
        return 0

    print()
    print("=" * 72)
    print("JCE dedup audit — duplication detectors")
    print("=" * 72)
    baseline = load_baseline()
    reasons  = load_reasons()
    counts: dict[str, int] = {}
    regressions: list[str] = []
    broken: list[str] = []
    for name, script, extra, pat in DETECTORS:
        rc, out = run(script, extra)
        if rc == 127:
            print(f"  [SKIP] {name} (missing)")
            continue
        m = None
        for line in out.splitlines():
            m = pat.search(line) or m

        # A DETECTOR THAT DID NOT PRODUCE A COUNT IS BROKEN, NOT IMPROVED.
        # This used to fall back to n = -1, which is below every baseline, so
        # it printed "improved by <baseline+1>" and the ratchet passed.  A
        # reworded summary line, an added flag, or a crash -- whose exit code
        # was not checked either, only 127 for "missing" -- all took that
        # path.  The count is the ONLY thing this ratchet reads, so failing to
        # read it has to be louder than any regression, not quieter.
        if rc != 0:
            broken.append(f"{name}: exited {rc} without producing a count")
            print(f"  {name}: BROKEN (exit {rc})")
            for line in out.splitlines()[-6:]:
                print(f"        {line}")
            continue
        if m is None:
            broken.append(f"{name}: no line matched {pat.pattern!r}")
            print(f"  {name}: BROKEN (summary line did not parse)")
            for line in out.splitlines()[-6:]:
                print(f"        {line}")
            continue

        n = int(m.group(1))
        counts[name] = n
        base = baseline.get(name)
        if base is None:
            print(f"  {name}: {n}  (no baseline)")
        elif n > base:
            print(f"  {name}: {n}  (BASELINE {base} -> +{n - base} REGRESSION)")
            regressions.append(f"{name} {base}->{n}")
        elif n < base:
            print(f"  {name}: {n}  (baseline {base}, improved by {base - n})")
        else:
            why = reasons.get(name)
            if why:
                print(f"  {name}: {n}  (at baseline; raised because: {why})")
            else:
                print(f"  {name}: {n}  (at baseline)")
        if args.verbose:
            for line in out.splitlines():
                print(f"        {line}")

    if args.write_baseline:
        # A RAISE is a claim: it says "this new duplication is justified".
        # Lowering, or writing a first baseline, is not, so neither needs a
        # reason.  Without this, raising the number is one keystroke and the
        # next reader cannot tell a justified process-global from a silenced
        # regression.
        raised = sorted(k for k, v in counts.items()
                        if baseline.get(k) is not None and v > baseline[k])
        if raised and not args.reason:
            print("\nREFUSED: --write-baseline would RAISE " +
                  ", ".join("%s %d->%d" % (k, baseline[k], counts[k])
                            for k in raised) +
                  ".\n  Pass --reason \"<why this duplication is justified>\". "
                  "A bare number records nothing:\n  the next reader cannot "
                  "tell a deliberate global from a silenced regression.")
            return 1
        merged = dict(counts)
        keep = {k: v for k, v in reasons.items() if k in counts}
        for k in raised:
            # APPEND, never replace.  A detector is raised more than once over
            # its life, and overwriting the note would erase why the PREVIOUS
            # increment was accepted -- leaving a number whose history reads as
            # one decision when it was several.
            prev = keep.get(k, "").strip()
            keep[k] = (prev + "  ||  " + args.reason.strip()) if prev                       else args.reason.strip()
        # A DECREASE RETIRES ONE JUSTIFICATION, NOT THE CHAIN.  This used
        # to pop the whole entry, on the grounds that "a reason for a count
        # that came back DOWN is stale -- the thing it justified is gone."
        # True of the most recent justification, false of the rest: the entry
        # is `||`-joined and global-state's held six.  It was emptied by a
        # re-baseline that took 859 -> 855 because twelve statics in a new
        # panel were folded into one struct -- the globals that went away were
        # the ones added minutes earlier, and all six recorded justifications
        # were still in the tree.  Six live notes deleted to retire none.
        #
        # Keeping a stale reason costs a reader one confusing sentence.
        # Deleting a live one costs them the whole rationale.  So the chain
        # stays and the human is told a prune may be due -- the sibling gate's
        # baseline writer makes the same argument in its own comment.
        lowered = [k for k in keep
                   if baseline.get(k) is not None
                   and counts.get(k, 0) < baseline[k]]
        for k in lowered:
            print("\n  NOTE: %s came down %d -> %d and its reasons were KEPT."
                  "\n  If one of them justified something that is now gone, "
                  "edit it out by hand -- this used to drop the whole chain, "
                  "which erased five live justifications to retire one."
                  % (k, baseline[k], counts[k]))
        if keep:
            merged["_reasons"] = keep
        # newline="\n" is not optional: Path.write_text on Windows expands "\n"
        # to CRLF, and this repository pins LF through .gitattributes.  The
        # first --write-baseline of this session produced a CRLF file and git
        # warned about it on the way in.
        BASELINE_PATH.write_text(json.dumps(merged, indent=2) + "\n",
                                 encoding="utf-8", newline="\n")
        print(f"\nwrote baseline -> {BASELINE_PATH.relative_to(REPO_ROOT).as_posix()}")
        print(json.dumps(merged, indent=2))
        return 0

    print()
    print("=" * 72)
    ok = not failed_gates and not regressions
    if failed_gates:
        print(f"BOUNDARY GATE FAILURES: {', '.join(failed_gates)}")
    if broken:
        print()
        print("=" * 72)
        print("BROKEN DETECTOR(S) -- no count was produced, so the ratchet "
              "checked nothing:")
        for b in broken:
            print("  " + b)
        print("=" * 72)
        return 1

    if regressions:
        print(f"DUPLICATION REGRESSIONS: {', '.join(regressions)}")
        print("A detector count rose above the recorded baseline: new duplication")
        print("was introduced. Fix it, or justify and re-run --write-baseline.")
    if ok:
        print("dedup audit: OK (no gate violations, no duplication regressions)")
    print("=" * 72)

    if not ok and not args.report_only:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
