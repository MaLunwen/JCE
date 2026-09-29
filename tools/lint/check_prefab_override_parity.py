#!/usr/bin/env python3
"""
check_prefab_override_parity.py — both halves of the prefab-instance format
must exist: the editor writes it, and the ENGINE must be able to load it.

WHY THIS EXISTS.  A prefab instance is stored in a scene file as a reference
plus a diff, not as a copy.  When the editor saves an instance whose source
file loads (editor/src/io/jce_editor_scene_serial.cpp), it emits:

    "prefabInstance": true,
    "prefabPath":     "assets/prefabs/crate.prefab.json",
    "overrides":      ["Transform"],        <- names of the differing rows
    "components":     [ {Transform ...} ],  <- ONLY those rows
    "children":       []                    <- deliberately EMPTY

The empty children array is not an oversight; the editor's own comment says
"an instance root's children come ENTIRELY from the source on load".  The
encoding is compact and correct -- provided whoever loads it instantiates the
prefab source first and treats `components` as an overlay.

WHAT WAS ACTUALLY TRUE (2026-09-01).  The editor could.  The engine could not:

    grep -c '"overrides"' engine/src/resource/jce_scene_serial.c              -> 0
    grep -c '"overrides"' engine/src/middleware/scene/jce_scene_components_json.c -> 0

The engine read prefabInstance and prefabPath ONLY to populate JceEditorMeta,
then built the entity from the `components` array verbatim.  So in a shipped
game every placed prefab loaded as a single bare entity carrying just the
overridden rows -- no MeshRenderer, no collider, no children.  A twenty-node
enemy prefab became one invisible Transform.

MEASURED, before the fix, by loading the editor's exact output shape through
the ENGINE loader: the overridden Transform arrived (position 5 as authored),
the source's MeshRenderer did not exist, and the source's child entity did not
exist.  Two controls in the same run passed -- a legacy full-snapshot node and
a node whose source is missing -- which is what makes the one failure a finding
about the engine rather than about the fixture.

Nothing in the build noticed for the same reason this whole family of defects
goes unnoticed: the editor loads its own format correctly, so the only
configuration in which the level is wrong is the one nobody opens while
authoring.  It would have surfaced the first time somebody packaged a level
that used prefabs -- which for ck is the first week of production.

WHAT THIS CHECKS.

  RULE 1  The engine's scene loader handles the "overrides" key.  Specifically
          jce_scene_components_json.c must contain a function that reads it AND
          calls jce_prefab_instantiate*.  Reading the key without instantiating
          the source would be the same bug with a different shape.

  RULE 2  The engine's overlay is reached from the entity-node parser, not left
          defined-and-uncalled -- the "complete implementation, zero callers"
          pattern this repository has repeatedly shipped.

  RULE 3  The editor still WRITES the format (write_override_node is called
          from its scene serializer).  If the editor ever stops emitting it,
          the engine-side code becomes dead and this gate says so rather than
          quietly passing because both sides vanished together.  A parity gate
          that goes green when the feature is deleted from both ends is not a
          parity gate.

WHAT THIS DOES NOT CHECK.  That the overlay produces the RIGHT entity -- that
is what tests/middleware/scene/test_jce_prefab_override_engine_load.c does, and
that file is gitignored on `main`, which is exactly why this checker carries the
rationale rather than pointing at it.

Usage:  python tools/lint/check_prefab_override_parity.py
Exit 0 clean, 1 on any finding.
"""

import re
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]

ENGINE_PARSER = (REPO_ROOT / "engine" / "src" / "middleware" / "scene"
                 / "jce_scene_components_json.c")
EDITOR_SERIAL = (REPO_ROOT / "editor" / "src" / "io"
                 / "jce_editor_scene_serial.cpp")

OVERRIDES_KEY = re.compile(r'"overrides"')
INSTANTIATE = re.compile(r"\bjce_prefab_instantiate(?:_file)?\s*\(")
WRITE_NODE = re.compile(r"\bwrite_override_node\s*\(")


def strip_comments(text: str) -> str:
    """Blank out // and /* */ so a key NAMED in prose is not evidence.

    Without this the gate would pass on a file that merely DISCUSSES
    "overrides" -- and the pre-fix engine had exactly that kind of comment
    about prefabs elsewhere.  Evidence has to be code."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def main() -> int:
    findings = []

    for required in (ENGINE_PARSER, EDITOR_SERIAL):
        if not required.is_file():
            print("check_prefab_override_parity: FAIL - %s not found"
                  % required.relative_to(REPO_ROOT).as_posix(), file=sys.stderr)
            return 1

    engine = strip_comments(ENGINE_PARSER.read_text(encoding="utf-8",
                                                    errors="replace"))
    editor = strip_comments(EDITOR_SERIAL.read_text(encoding="utf-8",
                                                    errors="replace"))
    eng_rel = ENGINE_PARSER.relative_to(REPO_ROOT).as_posix()
    ed_rel = EDITOR_SERIAL.relative_to(REPO_ROOT).as_posix()

    # ── RULE 1: the engine reads the key AND rebuilds from the source ──
    reads = len(OVERRIDES_KEY.findall(engine))
    instantiates = len(INSTANTIATE.findall(engine))
    if reads == 0:
        findings.append((eng_rel, "rule 1",
                         'the engine scene loader never reads "overrides", so '
                         "a prefab instance is built from the diff alone -- in "
                         "a shipped game every placed prefab loses every "
                         "component and child the source provided"))
    if instantiates == 0:
        findings.append((eng_rel, "rule 1",
                         "the engine scene loader never calls "
                         "jce_prefab_instantiate*, so nothing rebuilds the "
                         "subtree the scene file deliberately omits"))

    # ── RULE 2: the overlay is actually called ────────────────────────
    m = re.search(r"\bstatic\s+bool\s+(\w*prefab\w*overlay\w*)\s*\(", engine)
    if not m:
        findings.append((eng_rel, "rule 2",
                         "no prefab-overlay function found in the entity-node "
                         "parser"))
    else:
        name = m.group(1)
        calls = len(re.findall(r"\b%s\s*\(" % re.escape(name), engine)) - 1
        if calls <= 0:
            findings.append((eng_rel, "rule 2",
                             "%s() is defined but never called -- a complete "
                             "implementation with zero callers is how this "
                             "defect looked from the outside in the first "
                             "place" % name))

    # ── RULE 3: the editor still writes the format ────────────────────
    if len(WRITE_NODE.findall(editor)) == 0:
        findings.append((ed_rel, "rule 3",
                         "the editor no longer calls write_override_node(), so "
                         "the engine-side overlay is now dead code.  If the "
                         "override format was retired on purpose, retire both "
                         "halves and this gate together"))

    if findings:
        for rel, rule, why in findings:
            print("  %s  [%s] %s" % (rel, rule, why), file=sys.stderr)
        print("check_prefab_override_parity: FAIL - %d finding(s).  The scene "
              "file's prefab-instance encoding is a two-sided contract; a half "
              "of it is a shipped-only content failure."
              % len(findings), file=sys.stderr)
        return 1

    print("check_prefab_override_parity: OK (engine reads \"overrides\" in %d "
          "place(s) and instantiates the source in %d; the overlay is called "
          "from the entity-node parser; the editor still writes the format)"
          % (reads, instantiates))
    return 0


if __name__ == "__main__":
    sys.exit(main())
