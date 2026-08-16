#!/usr/bin/env python3
"""
gen_script_bindings.py — one tool, three modes, for the scripting surface.

  (default) --check   regenerate in memory, diff against the committed output,
                      run every manifest assertion, exit non-zero on failure
  --write             emit script-api.json and every backend's artefacts
  --update            re-baseline after an intentional change

ONE tool, not a generator plus a separate checker.  A checker that
re-implements generation in order to check generation is a second
implementation of the same contract, and two implementations drift -- which is
the failure this whole design exists to prevent.  Same --update/default shape
check_abi_snapshot.py already uses.

THIS FILE IS THE DRIVER AND NOTHING ELSE.  It knows about modes, exit codes
and the summary line; it knows nothing about Lua, Python, Java or C++.  The
knowledge is split in two:

  scriptgen_core.py   the manifest, the C decomposition, the seven shapes and
                      nine modifiers, the neutral conditions, script-api.json,
                      and the backend seam.  READ ITS DOCSTRING FIRST -- it
                      carries the worked example for adding a backend and the
                      rules a backend author must not break.
  emit_<language>.py  one per target language.  emit_lua.py today.  Discovered
                      by a sorted glob, so adding emit_python.py / emit_java.py
                      / emit_cpp.py means writing ONE NEW FILE and editing
                      none -- including this one.

That is the whole reason for the split: batch 1 had four tasks queue up behind
this file because concurrent edits to a single 948-line module overwrite each
other, and three backends are that collision three ways.

Python 3.12, standard library only: ci.yml:25 pins 3.12 and the fast lint tier
at ci.yml:40 runs BEFORE `pip install conan` at :54, so a gate needing
pycparser or libclang could not sit there.

Its tests: test_script_bindings_gate.py
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import scriptgen_core as core                              # noqa: E402


def all_artefacts(members, man) -> list[core.Artefact]:
    """Every committed file this tool owns: each backend's, then the core's.

    Backends first, core last, because that is the order the three artefacts
    have been reported in since batch 1 and a check-mode failure listing them
    in a new order reads as a change that did not happen."""
    out: list[core.Artefact] = []
    for backend in core.discover_backends():
        out += backend.artefacts(members, man)
    return out + core.core_artefacts(members, man)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--update", action="store_true")
    args = ap.parse_args()

    members = core.parse_host_members(core.HEADER.read_text(encoding="utf-8"))
    man = core.load_manifest()
    c_text = core.SCRIPT_C.read_text(encoding="utf-8")

    problems = core.validate(members, man, c_text)
    if problems:
        print("script-bindings: FAILED")
        for p in problems:
            print("    " + p)
        return 1

    t = man["declared_totals"]
    print(f"script-bindings: OK - {t['host_members']} host members, "
          f"{t['expose']} generatable + {t['hand_written']} hand-written = "
          f"{t['expose'] + t['hand_written']} registered, "
          f"{t['constants']} constant, {t['table_keys']} table keys; "
          f"{t['members_reached_by_generated']} members reached by generated "
          f"bindings, {t['members_reached_only_by_hand_written']} only by "
          f"hand-written, 0 unclaimed")

    artefacts = all_artefacts(members, man)
    want = [(a.path, a.render(members, man)) for a in artefacts]

    if args.write or args.update:
        for path, text in want:
            core.write_lf(path, text)
        print("wrote " + ", ".join(
            path.relative_to(core.REPO_ROOT).as_posix() for path, _ in want))
        return 0

    # An artefact whose whole tree is absent is UNANSWERABLE, not passing.  It
    # is separated from the failures so it can be printed rather than dropped:
    # a count that silently shrinks is how "checked everything" gets said about
    # a run that checked less.
    emitted: list[str] = []
    unverifiable: list[str] = []
    for path, text in want:
        if core.artefact_is_unverifiable(path):
            unverifiable.append(path.relative_to(core.REPO_ROOT).as_posix())
            continue
        emitted += core.diff_artefact(path, text)
    if core.API_JSON.is_file():
        emitted += core.check_append_only(
            json.loads(core.API_JSON.read_text(encoding="utf-8")),
            json.loads(core.emit_api_json(members, man)))
    if emitted:
        print("script-bindings: FAILED")
        for p in emitted:
            print("    " + p)
        return 1
    if unverifiable:
        print(f"script-bindings: {len(unverifiable)} artefact(s) NOT CHECKED — "
              f"tests/ is not part of this repository, so they have nowhere to "
              f"live here and nothing to compare against. Verified where the "
              f"suite is present: " + ", ".join(unverifiable))
    # The artefact count is DERIVED, not typed: a backend that lands four more
    # files must not leave this sentence saying 3.
    print(f"script-bindings: generated output verified — "
          f"{len(man['expose'])} functions, {len(man['constants'])} constant, "
          f"{len(want) - len(unverifiable)} artefacts byte-identical to a "
          f"fresh emit")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
