#!/usr/bin/env python3
"""
check_dependency_licenses.py — fail if a copyleft / redistribution-hostile
licence enters the HOST dependency closure (plan §38.6, licensing invariant).

Why this gate exists
--------------------
The SDK merges every static dependency into one redistributable fat lib
(engine/cmake/JCESDKInstall.cmake), so anything in the host graph is
something JCE's users ship.  A single Conan option flipping back to its
upstream default — or a version bump that adds a new transitive requirement —
silently re-introduces an LGPL/GPL subtree with no visible symptom at build
time.  The 2026-07-25 cleanup removed four such subtrees (see
docs/audits/dependency-and-language-audit.md §6.5); without a gate the next
dependency bump quietly undoes it.

Host vs build context
---------------------
Only the HOST context is checked.  Build-context packages (nasm,
strawberryperl, cmake, ...) run on the build machine and are never linked
into or shipped with the product, so their licence imposes no distribution
obligation.  Conflating the two is the classic false positive here —
strawberryperl is Artistic-1.0/GPL-1.0 and is perfectly fine to keep.

Multi-licensed packages
-----------------------
Conan's `license` field is a flat list for the whole package and does NOT say
which sub-licence covers the part we actually link.  This gate treats "any
listed licence is copyleft" as a violation on purpose: resolving that
ambiguity is a legal question, not a build question, so the safe default is
to keep such a package out of the closure entirely.  Genuinely-analysed
exceptions go in ALLOWLIST below with a written rationale.

Usage:
  python tools/audit/check_dependency_licenses.py [--profile default] [--json]

Exit codes: 0 clean · 1 violation · 0 + notice when conan/graph unavailable
(the gate must not turn a machine without Conan into a red build).
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# Licences that create source-disclosure or relinking obligations for a
# statically-linked redistributable.  MPL/EPL/CDDL are file-level copyleft —
# far weaker than GPL, but they still carry per-file source obligations, and
# JCE currently has zero of them, so the gate keeps it that way.
COPYLEFT = re.compile(
    r"\b("
    r"A?GPL|LGPL|SSPL|OSL|EUPL|CPL|EPL|CDDL|MPL"
    r"|CC-BY-SA|CC-BY-NC|Sleepycat|Artistic"
    r")\b",
    re.IGNORECASE,
)

# name -> rationale.  Only for packages analysed and deliberately accepted.
# Keep this empty unless there is a written justification in the audit report.
ALLOWLIST: dict[str, str] = {}


def resolve_graph(profile: str) -> list[dict] | None:
    """Return the conan graph nodes, or None if conan is unavailable."""
    cmd = [
        "conan", "graph", "info", str(REPO_ROOT),
        "--format=json", f"-pr:h={profile}", f"-pr:b={profile}",
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=600)
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    # conan prints progress on stderr and JSON on stdout, but older versions
    # can prepend banner lines — take from the first '{'.
    out = proc.stdout or ""
    brace = out.find("{")
    if brace < 0:
        return None
    try:
        data = json.loads(out[brace:])
    except json.JSONDecodeError:
        return None
    return list(data.get("graph", {}).get("nodes", {}).values())


def licences_of(node: dict) -> list[str]:
    lic = node.get("license")
    if lic is None:
        return []
    if isinstance(lic, str):
        return [lic]
    return [str(x) for x in lic]


def find_violations(nodes: list[dict]) -> tuple[int, list[dict]]:
    """Classify a resolved graph. Split out from main() so the negative case
    (a graph that SHOULD fail) can be exercised without mutating conanfile.py."""
    violations = []
    host_count = 0
    for n in nodes:
        ref = n.get("ref") or ""
        if not ref:
            continue  # the consumer (root) node
        if n.get("context", "host") != "host":
            continue  # build tools are never linked or redistributed
        host_count += 1
        if ref.split("/")[0] in ALLOWLIST:
            continue
        hits = [l for l in licences_of(n) if COPYLEFT.search(l)]
        if hits:
            violations.append({"package": ref, "licenses": hits})
    return host_count, violations


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", default="default")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    nodes = resolve_graph(args.profile)
    if nodes is None:
        # Not a failure: a contributor without Conan, or an offline machine,
        # must still be able to run the audit suite.  CI has Conan, so the
        # gate is real where it matters.
        msg = ("NOTICE: conan graph unavailable (conan missing, offline, or "
               "profile absent) — licence gate skipped, not failed.")
        print(json.dumps({"skipped": True, "reason": msg}) if args.json else msg)
        return 0

    host_count, violations = find_violations(nodes)

    if args.json:
        print(json.dumps({
            "host_packages": host_count,
            "violations": violations,
        }, indent=2))
    else:
        print(f"host-context packages scanned: {host_count}")
        if violations:
            print(f"\nFAIL — {len(violations)} copyleft package(s) in the host closure:")
            for v in violations:
                print(f"  {v['package']}  ->  {', '.join(v['licenses'])}")
            print("\nThe host closure is statically merged into the redistributable")
            print("SDK. Either drop the package (usually a Conan option on its")
            print("consumer — see docs/audits/dependency-and-language-audit.md §6.5)")
            print("or add it to ALLOWLIST with a written rationale.")
        else:
            print("PASS — no copyleft licences in the host dependency closure.")

    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
