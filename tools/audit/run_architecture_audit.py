#!/usr/bin/env python3
"""
run_architecture_audit.py — single entry point for the JCE dependency /
multi-language architecture audit gate (plan §4.3 / §33 / §38.6).

It runs, in one shot, the checks that enforce the dependency-ownership and
ABI-boundary invariants and returns a non-zero exit code if ANY of them
finds a NEW violation.  It deliberately REUSES the existing structural lints
under tools/lint/ (they already encode the boundary doctrine — no point
re-implementing them) and adds the audit-specific data-driven checks under
tools/audit/.

Checks run (each a child process so one failure doesn't hide the others):

  Reused structural lints (tools/lint/):
    - check_public_api_purity.py      third-party #includes in public headers
    - check_editor_consumer_purity.py editor reaches engine only via <jce/...>
    - check_layer_dependencies.py     no reverse-direction layer includes
    - check_raw_allocator.py          malloc/new bypassing jce_alloc
    - check_platform_macros.py        raw platform macros outside os/

  Audit-specific data-driven checks (tools/audit/):
    - check_dependency_boundaries.py  ownership-matrix include/link whitelist
    - check_public_abi.py             third-party TYPE names in public ABI
    - check_dependency_licenses.py    no copyleft in the HOST dep closure
    - check_abi_snapshot.py           public C ABI removals / signature changes
    - check_view_reservations.py      bgfx view-id reservations narrowed / bases too close
    - check_binding_parity.py --version-only
                                      JNI EXPECTED_API_VERSION mirrors
                                      project(JCE VERSION) (GATES)
    - check_binding_parity.py         C/Lua/JNI core-parity (report-only)
    - check_script_language_catalog.py  a claimed script extension has an
                                        offline catalog row (cooker/editor/pak)
    - check_script_vm_parity.py       every public lifecycle fn has a VM slot
    - check_sdk_scripting_export.py   everything scripting/ builds ships or
                                        says why not

Gating: all checks above gate EXCEPT the check_binding_parity MATRIX run
(informational until the parity contract is frozen — pass --strict-parity
to gate it too).  Its --version-only run DOES gate: a drifted constant is a
factual mismatch, not a coverage judgement.
check_engine_native_io.py is intentionally NOT gated here yet: it currently
flags the pre-existing ai_dispatch stdio + a Win32 audio-loopback TU that
are tracked as accepted/known in the audit report; add it once those are
resolved.

Usage:
  python tools/audit/run_architecture_audit.py [--strict-parity]
  # exit 0 = all gating checks clean, 1 = at least one violation.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LINT_DIR = REPO_ROOT / "tools" / "lint"
AUDIT_DIR = REPO_ROOT / "tools" / "audit"

# (script path, args, gating?)
CHECKS = [
    (LINT_DIR / "check_public_api_purity.py", [], True),
    (LINT_DIR / "check_editor_consumer_purity.py", [], True),
    (LINT_DIR / "check_layer_dependencies.py", [], True),
    (LINT_DIR / "check_raw_allocator.py", [], True),
    (LINT_DIR / "check_platform_macros.py", [], True),
    (AUDIT_DIR / "check_dependency_boundaries.py", [], True),
    (AUDIT_DIR / "check_public_abi.py", [], True),
    (AUDIT_DIR / "check_dependency_licenses.py", [], True),
    (AUDIT_DIR / "check_abi_snapshot.py", [], True),
    # The SAME checker against HEAD, and it is not redundant: the working-tree
    # run above answers "am I about to break the ABI?", and by design it reports
    # an APPEND or an ADDITION as compatible-and-OK.  So a commit could add a
    # public struct member and leave contracts/abi-snapshot.txt stale while this
    # whole suite stayed green -- which is exactly what happened on 2026-08-31
    # (10dc9009, then 6bdf2f68).  --committed reads headers AND baseline out of
    # the object database and requires them to agree exactly, which is the
    # answer CI gets, and it is immune to a neighbouring session's half-finished
    # headers because it never looks at the working tree.
    (AUDIT_DIR / "check_abi_snapshot.py", ["--committed"], True),
    # Gating for the same reason check_abi_snapshot.py gates, on the same shape
    # of contract: a bgfx view id claimed by two owners does not error, it makes
    # one of them silently produce no pixels.  The frozen reservation set is a
    # union that may only grow; a static_assert written in terms of the
    # constants it checks cannot see a NARROWING, and no per-viewport assert can
    # see the two editor bases drifting closer than one base's full claim.
    (AUDIT_DIR / "check_view_reservations.py", [], True),
    # Gating for the same reason check_abi_snapshot.py gates: "the host grew a
    # member and nobody decided whether scripts see it" is a fact, not a
    # judgement.  Its totality condition is the anti-drift lock.
    (REPO_ROOT / "tools" / "scriptgen" / "gen_script_bindings.py", [], True),
    # The C ABI backend of the same surface, gating for the same reason: the
    # shared library's export set IS the sandbox boundary, and it is generated.
    # A hand-edited .def or a stale forwarder is a fact about what a script can
    # reach, not a judgement call.
    (REPO_ROOT / "tools" / "scriptgen" / "gen_script_c_abi.py", [], True),
    # Gating: "dist is royalty-free, release carries the codecs, release can
    # opt out" is a licensing contract. Two of its three clauses drifted for
    # months because nothing checked them and the symptoms were invisible
    # (a preset silently re-stamping a cache entry; a message(WARNING) CMake
    # does not repeat on incremental reconfigures).
    (AUDIT_DIR / "check_patented_codec_gate.py", [], True),
    # The version-handshake mirror gates; the parity MATRIX does not.
    # A drifted EXPECTED_API_VERSION makes the JNI binding reject a
    # correctly paired engine at runtime, which is a fact, not a judgement.
    (AUDIT_DIR / "check_binding_parity.py", ["--version-only"], True),
    (AUDIT_DIR / "check_binding_parity.py", [], False),  # report-only by default
    # ── The scripting-language contracts ──────────────────────────────
    #
    # All three were TRACKED AND INVOKED BY NOTHING.  They passed, which is the
    # trap and not the reassurance: a gate no runner runs cannot go red, and
    # this repository has recorded that failure twice already.  Compounding it,
    # jce_script_vm.h:20-25 tells its reader these were "no longer checked --
    # tools/audit/ was removed", while tools/audit/ is present and tracked.
    #
    # Gating, not report-only, because each states a FACT about a shipped
    # artefact rather than a judgement:
    #
    #   catalog  — a backend may claim an extension at run time, but the
    #     cooker, the bundle manifest, the editor's asset kind and the Script
    #     picker all read the OFFLINE table in engine/src/resource/
    #     jce_asset_ext.c.  A language whose backend registers and whose row is
    #     missing gets labelled 'binary', is absent from the picker, and a
    #     packaged build may ship without it -- invisible until someone
    #     installs the game.  Mutation-tested: deleting the .py row makes it
    #     exit 1 naming .py, python and the file; restoring makes it exit 0.
    #
    #   vm_parity — every public lifecycle function must have a vtable slot, or
    #     a 20th lifecycle function reaches Lua alone and no other language.
    #
    #   sdk_scripting_export — everything scripting/ builds either ships in the
    #     SDK or says why not.  The failure it prevents is a backend that
    #     builds and is silently absent from the redistributable.
    (AUDIT_DIR / "check_script_language_catalog.py", [], True),
    (AUDIT_DIR / "check_script_vm_parity.py", [], True),
    (AUDIT_DIR / "check_sdk_scripting_export.py", [], True),
    # Gating, and it took until 2026-09-01 for anything to RUN it.  This
    # runner owns three checks -- the single-image-decoder boundary, the
    # file-scope-global-state inventory, and the duplication ratchet -- and it
    # had no caller anywhere: not here, not run_all.py, only a gitignored plan
    # document.  Run by hand it printed "DUPLICATION REGRESSIONS ... Fix it"
    # and exited 0, because failing was behind a --check flag nobody passed.
    # Both halves are fixed: failing is its default now, and it is listed
    # here.
    (AUDIT_DIR / "run_dedup_audit.py", [], True),
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

    KEEP IN SYNC with the identical copy in tools/lint/run_all.py and tools/audit/run_dedup_audit.py."""
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
# SEVENTH, 2026-09-17, and it is the same shape as the second:
# check_dependency_boundaries.py died with
#
#     TypeError: 'list_iterator' object is not callable
#
# from `any(p.search(inc) for p in pats)` -- `any` is a builtin and `pats` is a
# list of compiled patterns, verified by probing the module's own table.  It
# passed on the next three consecutive runs, standalone and under this runner,
# with and without PYTHONDONTWRITEBYTECODE.  Same family as the `list.append`
# one: a BUILTIN reported as a non-callable iterator object.
#
# IT WAS NOT RETRIED, AND NO SIGNATURE WAS ADDED FOR IT.  The retry rule below
# requires either empty output or a signature from _INTERPRETER_FAULTS, and
# this printed a traceback.  A signature could be written -- but not one that
# meets the standard the list already holds itself to: both entries there are
# messages a checker's SOURCE cannot produce, and `'X' object is not callable`
# is a mistake somebody could genuinely write.  A retry rule that swallowed it
# would also swallow the real defect it resembles, which is exactly the trade
# this machinery exists to refuse.  Recorded as evidence instead, so the next
# person to see it has the precedent rather than a fresh mystery.
#
# EIGHTH, 2026-09-17, hours later and in a DIFFERENT shape, which is the part
# worth recording.  check_public_api_purity.py died with
#
#     AttributeError: 'tuple' object has no attribute 'endswith'
#
# from `return name.endswith((".h", ".hpp", ".hh", ".hxx"))`, where `name` is
# a str the caller took from os.scandir.  The local was holding the ARGUMENT
# TUPLE of the very call it was making.  The previous seven were all one
# family -- a callable reported as a non-callable -- and could just about be
# imagined as one exotic interpreter bug; a local slot holding the wrong
# object entirely cannot, and it is what the hardware diagnosis below
# predicts.  Five consecutive standalone re-runs exited 0, and the full audit
# passed on the next run.  No signature added, for the reason given above:
# `name.endswith` failing is also what a genuine type error looks like.
#
# AND THE CAUSE IS NO LONGER A GUESS.  On 2026-09-17 the owner of this machine
# reported that it BLUE-SCREENS frequently and loses user-mode processes, and
# believes the cause is hardware.  That is the whole diagnosis: every fault in
# this list is a freshly spawned short-lived CPython dying or corrupting on an
# unstable machine, not a race this repository can fix and not a defect in any
# checker.  Two practical consequences, stated so nobody re-derives them:
#   * ON THIS MACHINE, an isolated crash-shaped gate failure is not evidence
#     about the tree.  Re-run it alone.  A failure that REPRODUCES still is.
#   * A gate failure that PRINTS A FINDING is never in this category.  The
#     machine corrupts processes; it does not compose a coherent violation
#     report.
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
def run_one(path: Path, args: list[str]) -> tuple[int, str]:
    if not path.is_file():
        return 127, f"(missing: {path.name})"
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"

    def _spawn():
        pr = subprocess.run(
            [sys.executable, str(path), *args],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            env=env)
        return pr.returncode, (pr.stdout or "") + (pr.stderr or "")

    rc, out = _spawn()
    if _crash_shaped(rc, out, 2):
        print(f"  [retry] {path.name} exited {rc} crash-shaped "
              f"(no output, or an interpreter fault) -- re-running once",
              flush=True)
        rc, out = _spawn()
    return rc, out


DEFAULT_LOG = REPO_ROOT / "reports" / "architecture-audit-failure.txt"


def write_transcript(entries, explicit, failed):
    """Persist every check's argv, exit code and output.

    Written whenever --log is given, and ALWAYS on failure even when it is
    not.  A run of this orchestrator is not reproducible: it shells out to
    fourteen checkers that read the working tree, and a caller who keeps only
    the summary line has destroyed the only copy of the diagnosis.  That
    happened on 2026-08-27 -- one FAILED run, eight green runs after it, and
    nothing left to look at.  The failing line names this file so that even
    `... | tail -1` carries the pointer.
    """
    if explicit is None and not failed:
        return None
    target = Path(explicit) if explicit else DEFAULT_LOG
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        parts = [
            f"# JCE architecture audit transcript ({datetime.now().isoformat(timespec='seconds')})",
            f"# {len(entries)} check(s); python {sys.version.split()[0]}",
            "",
        ]
        for name, argv, gating, rc, out in entries:
            parts.append("=" * 72)
            parts.append(f"{name} {' '.join(argv)}".rstrip())
            parts.append(f"gating={gating}  exit={rc}")
            parts.append("-" * 72)
            parts.append(out.rstrip() or "(no output)")
            parts.append("")
        target.write_text("\n".join(parts) + "\n", encoding="utf-8",
                          newline="\n")
        return target
    except OSError as exc:
        print(f"(could not write transcript to {target}: {exc})")
        return None


def is_crash(rc: int) -> bool:
    """A checker that DIED, as opposed to one that reported violations.

    A gate answers with a small exit code it chose.  Anything else is the
    process being killed: on Windows an NTSTATUS (0xC0000409 =
    STATUS_STACK_BUFFER_OVERRUN is the one seen here, twice, with EMPTY
    output), on POSIX a negative value meaning a signal.  Reporting that as
    'reported violations' sends the reader looking for an architecture
    problem that does not exist -- the two need different actions, so they
    get different words.
    """
    return rc < 0 or rc >= 0x80000000


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict-parity", action="store_true",
                    help="also gate on binding-parity core gaps")
    ap.add_argument("--quiet", action="store_true",
                    help="only print the summary and failing check output")
    ap.add_argument("--log", metavar="PATH", default=None,
                    help="write a full transcript (every check's argv, exit "
                         "code and output) here.  On FAILURE a transcript is "
                         "written whether or not this is given -- see "
                         "DEFAULT_LOG.")
    args = ap.parse_args()

    print("=" * 72)
    print("JCE architecture audit — dependency ownership + ABI boundaries")
    print("=" * 72)

    failures = 0
    crashes = 0
    transcript: list[tuple[str, list[str], bool, int, str]] = []
    for path, extra, gating in CHECKS:
        check_args = list(extra)
        if (path.name == "check_binding_parity.py" and args.strict_parity
                and "--version-only" not in check_args):
            # Only the parity MATRIX run takes --strict; the version-mirror
            # run already gates and short-circuits before parity is computed.
            check_args.append("--strict")
            gating = True
        rc, out = run_one(path, check_args)
        transcript.append((path.name, check_args, gating, rc, out))
        if rc == 0:
            status = "PASS"
        elif is_crash(rc):
            status = f"CRASH(0x{rc:08X})"
        else:
            status = f"FAIL({rc})" if gating else f"warn({rc})"
        gate = "" if gating else "  [report-only]"
        # Show the arguments: the same checker is registered twice on purpose
        # (check_abi_snapshot.py, working tree vs --committed), and two
        # identical lines make the pair read as an accidental duplicate.
        shown = " ".join(check_args)
        label = f"{path.name} {shown}" if shown else path.name
        print(f"[{status}] {label}{gate}")
        if rc != 0 and (gating or not args.quiet):
            for line in out.rstrip().splitlines():
                print(f"    {line}")
        if rc != 0 and gating:
            if is_crash(rc):
                crashes += 1
            else:
                failures += 1

    print("=" * 72)
    # `failures or crashes`, not just `failures`: a CRASH is precisely the
    # case this transcript exists for -- an empty-output process death that
    # is not reproducible.  Passing only `failures` made the guard skip the
    # one run it was written to capture.
    log_path = write_transcript(transcript, args.log,
                                bool(failures or crashes))
    if failures or crashes:
        tail = f"  Full transcript: {log_path}" if log_path else ""
        what = []
        if failures:
            what.append(f"{failures} gating check(s) reported violations")
        if crashes:
            what.append(f"{crashes} gating check(s) CRASHED (see the exit "
                        f"code; this is not an architecture finding)")
        print(f"AUDIT FAILED — {' and '.join(what)}.{tail}")
        return 1
    if log_path:
        print(f"AUDIT PASSED — all gating checks clean.  "
              f"Transcript: {log_path}")
        return 0
    print("AUDIT PASSED — all gating checks clean.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
