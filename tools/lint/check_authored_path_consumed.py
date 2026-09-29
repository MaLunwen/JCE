#!/usr/bin/env python3
"""
check_authored_path_consumed.py — an authored asset path must have a reader.

WHY THIS EXISTS.  On 2026-09-01 JceSpotLight carried an authored `ies_path`
AND an `ies_lut_texture`, the PBR shader carried an s_iesLut sampler on stage
14, jce_lighting_system.c bound it, and the inspector offered a file picker.
Nothing in engine/ or editor/ ever assigned ies_lut_texture anything but
JCE_TEXTURE_INVALID, so the draw path's

    (path is set AND texture is valid) ? texture : INVALID

could not pass.  A designer picked an .ies file, the scene saved the key, and
the light never changed.  The identical gap sat on cookie_path for both
directional and spot lights.  Every layer shipped except the one that turns a
path into bytes.

That failure has no symptom.  There is no error, no warning, and no missing
asset -- the feature renders exactly as if it had not been authored, which is
also exactly how it renders when it IS authored.  The only way to see it is to
ask, of every authored path, "who reads this?"

WHAT THIS CHECKS.  For every `char <name>_path[N]` field on a component in
engine/include/jce/middleware/scene/jce_scene.h, some file under engine/src
must mention `.<name>` or `-><name>` OUTSIDE the JSON (de)serializers in
jce_scene_components_*.c.  Reading a path only to write it back out is not
consuming it.

WHAT THIS DOES NOT CHECK.  That the reader loads the right thing, or that the
result reaches the GPU.  A text scan sees a mention.  It is the difference
between "nobody reads this field" -- which was true of six fields when this
was written -- and "this feature works", which needs a run.

RULE 2 -- AN EXEMPTED FIELD THAT THE INSPECTOR DRAWS MUST SAY SO.  Marking
the control and exempting the field are two halves of one statement, and this
repository's own record is that updating one side of a two-sided contract is
worse than updating neither: a badge without an exemption hides a fixed field,
an exemption without a badge leaves a designer setting a control that cannot
act.  So if an exempted field name appears in editor/src/panels, an
insp_unwired_field_badge() call must appear within a few lines of it.  (A
field the inspector does not draw at all -- graph_path today -- needs no
badge, and is not asked for one.)  EITHER badge satisfies the rule:
insp_unwired_badge() marks a WHOLE component and sits at the top of its
drawer, which is what draw_comp_net_animator and draw_comp_net_rigidbody
use -- neither of those components has an engine reader at all, so
demanding a per-field badge inside a drawer that already says the whole
component is inert would be a false positive.

EXEMPTIONS.  tools/lint/authored_path_exempt.txt, one `field reason` per
line.  A line with no reason fails, and so does an exemption for a field that
either no longer exists or has since gained a reader: an exemption list that
outlives its reason is how the next reader learns the wrong thing.

Usage:  python tools/lint/check_authored_path_consumed.py
        python tools/lint/check_authored_path_consumed.py --list
Exit 0 clean, 1 on any finding.
"""

import re
import subprocess
import sys
from pathlib import Path

# See tools/lint/lint_git_files.py for why this is the working tree.
import importlib.util as _ilu
from pathlib import Path as _P
_spec = _ilu.spec_from_file_location(
    "lint_git_files", str(_P(__file__).resolve().parent / "lint_git_files.py"))
_lgf = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(_lgf)


LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]
HEADER = REPO_ROOT / "engine/include/jce/middleware/scene/jce_scene.h"
EXEMPT = LINT_DIR / "authored_path_exempt.txt"

# The JSON round-trip.  Mentioning a path here is serialisation, not use.
SERIALIZERS = ("jce_scene_components_",)

STRUCT = re.compile(r"typedef struct\s*\{(.*?)\}\s*(\w+)\s*;", re.S)
PATH_FIELD = re.compile(r"^[ \t]*char\s+(\w*_path)\s*\[", re.M)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def authored_paths() -> dict:
    """field name -> sorted list of components declaring it."""
    src = strip_comments(HEADER.read_text(encoding="utf-8", errors="replace"))
    out = {}
    for m in STRUCT.finditer(src):
        body, comp = m.group(1), m.group(2)
        for f in PATH_FIELD.findall(body):
            out.setdefault(f, set()).add(comp)
    return {k: sorted(v) for k, v in out.items()}


def engine_sources():
    # WORKING TREE, not tracked-only: `git ls-files` hides a brand-new file
    # until it is committed, so a defect introduced in a new file passes this
    # checker on the commit that introduces it.  check_shader_uniform_bound.py
    # already found this the hard way -- it reported four dead cloud uniforms
    # because the file that creates them was untracked at the time.
    for rel in _lgf.working_tree():
        if not rel.startswith("engine/src"):
            continue
        if rel.endswith((".c", ".cpp")):
            yield rel


def read_exemptions() -> dict:
    out = {}
    if not EXEMPT.is_file():
        return out
    for raw in EXEMPT.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        field, _, reason = line.partition(" ")
        out[field.strip()] = reason.strip()
    return out


def main() -> int:
    if not HEADER.is_file():
        print("check_authored_path_consumed: FAIL - missing %s"
              % HEADER.relative_to(REPO_ROOT).as_posix(), file=sys.stderr)
        return 1

    fields = authored_paths()
    if len(fields) < 10:
        print("check_authored_path_consumed: FAIL - parsed only %d path "
              "field(s); the header shape changed and this extractor no "
              "longer sees it" % len(fields), file=sys.stderr)
        return 1

    texts = {}
    for rel in engine_sources():
        if any(k in rel for k in SERIALIZERS):
            continue
        texts[rel] = strip_comments(
            (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace"))

    exempt = read_exemptions()
    rows, unread = [], []
    for field, comps in sorted(fields.items()):
        # Two things this pattern has to get right, and the first version
        # got the second one wrong:
        #
        #  1. `->albedo_path` must not be satisfied by `->layer_albedo_path`,
        #     so the member access is part of the pattern, not the bare name.
        #
        #  2. AN EMPTINESS GUARD DOES NOT COUNT.  The broken code this gate
        #     exists for had one:
        #
        #         sl.ies_lut_texture = (slc->ies_path[0] != 0
        #                               && jce_texture_valid(...)) ? ... ;
        #
        #     The first version counted that mention, so the gate would have
        #     PASSED on the exact defect that motivated it.  Its own negative
        #     control found this: deleting the reader left the gate green.
        #     A path is consumed when it is used as a VALUE -- passed to a
        #     loader, copied, compared.  Asking whether it is empty is not
        #     use, so a mention followed by `[0]` is not a reader.  Only
        #     `[0]` -- `layer_albedo_path[li]` indexes the LAYER array and
        #     is real use, so excluding every `[` would report a consumed
        #     field as unread, which the first attempt at this fix did.
        pat = re.compile(r"(?:\.|->)%s\b(?!\s*\[\s*0\s*\])" % re.escape(field))
        readers = sorted(rel for rel, t in texts.items() if pat.search(t))
        rows.append((field, comps, readers, field in exempt))
        if not readers and field not in exempt:
            unread.append((field, comps))

    if "--list" in sys.argv:
        print("%-26s %-34s %s" % ("field", "component(s)", "readers"))
        for field, comps, readers, ex in rows:
            state = ("EXEMPT" if ex else ("%d" % len(readers)) if readers
                     else "NONE")
            print("%-26s %-34s %s" % (field, ",".join(comps), state))

    dead = [f for f in exempt if f not in fields]
    now_read = [f for f, _c, r, ex in rows if ex and r]

    # Rule 2: an exempted field the inspector DRAWS must carry the badge.
    BADGES = ("insp_unwired_field_badge", "insp_unwired_badge")
    NEAR = 10         # lines EITHER SIDE of a mention.  Symmetric because the
                      # badge sits beside the control, and the control's field
                      # is named both before it (the widget) and after it (a
                      # `if (x->path[0])` clear button).  A forward-only window
                      # missed the second case.
    unbadged = []
    panels = sorted((REPO_ROOT / "editor/src/panels").glob("*.cpp"))
    for f in sorted(exempt):
        drawn_at = []
        for panel in panels:
            lines = panel.read_text(encoding="utf-8",
                                    errors="replace").splitlines()
            pat = re.compile(r"(?:\.|->)%s\b" % re.escape(f))
            for i, line in enumerate(lines):
                if pat.search(line):
                    lo = i - NEAR if i > NEAR else 0
                    window = lines[lo:i + NEAR + 1]
                    drawn_at.append((panel.name, i + 1,
                                     any(b in w for w in window
                                         for b in BADGES)))
        if drawn_at and not any(ok for _n, _l, ok in drawn_at):
            unbadged.append((f, drawn_at[0][0], drawn_at[0][1]))

    ok = True
    for field, comps in unread:
        ok = False
        print("  %s (%s): authored and serialised, and NOTHING under "
              "engine/src reads it outside jce_scene_components_*.c.  A "
              "designer can set it and the engine cannot act on it -- with no "
              "error, because an unread path renders exactly like an unset "
              "one.  Wire a reader, or exempt it in "
              "tools/lint/authored_path_exempt.txt with a reason."
              % (field, ", ".join(comps)), file=sys.stderr)
    for f in sorted(exempt):
        if not exempt[f]:
            ok = False
            print("  %s: exempted with no reason.  An exemption without a "
                  "stated reason is indistinguishable from an oversight." % f,
                  file=sys.stderr)
    for f in dead:
        ok = False
        print("  %s: exempted but no longer an authored path field.  Drop the "
              "exemption." % f, file=sys.stderr)
    for f, panel, line in unbadged:
        ok = False
        print("  %s: exempted as unread, but %s:%d draws it with no "
              "insp_unwired_field_badge() nearby.  A designer sets the "
              "control and nothing happens, with no way to tell that from the "
              "control working.  Badge it, or delete the exemption if it "
              "gained a reader." % (f, panel, line), file=sys.stderr)
    for f in now_read:
        ok = False
        print("  %s: exempted, but it now HAS a reader.  Delete the exemption "
              "-- a list that outlives its reason teaches the next reader the "
              "wrong thing." % f, file=sys.stderr)

    if not ok:
        print("check_authored_path_consumed: FAIL", file=sys.stderr)
        return 1

    print("check_authored_path_consumed: OK (%d authored path field(s); %d "
          "read; %d exempted with a reason)"
          % (len(fields), len(fields) - len(exempt), len(exempt)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
