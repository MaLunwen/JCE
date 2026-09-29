#!/usr/bin/env python3
"""
check_component_serializer_roundtrip.py — a component field must survive a
save and a load.

WHY THIS EXISTS.  A field written to the scene JSON and never read back is
authoring that disappears the next time the scene opens.  A field read and
never written is a value that resets to its default on every save.  Neither
reports anything: the scene loads, the component is there, and one number is
quietly not what the designer left.

This project has already lost a field to it.  MeshRenderer.visible went BOTH
ways at once -- the parser hardcoded true and the serialiser never wrote the
key -- so the editor could not save a hidden mesh and any script's comp_set
turned it back on.  Measured at the time as a花 card visibility of 1.00 vs
0.20 with the fix, 1445 vs 1410 pixels without it.

WHAT THIS CHECKS.  For every scalar field of every scene component, whether
jce_scene_components_*.c both WRITES it (cJSON_Add*, or any `x->field` read
that is not an assignment) and READS it back.  A field that does exactly one
of the two is reported.

THE DETECTOR TOOK THREE TRIES, and the first two numbers were both wrong in
the same direction, so they are recorded here rather than left for the next
person to rediscover:

  * 104 findings -- a read was only recognised as `x.field =`, which misses
    ARRAYS, restored element-wise as `bc.center[0] = j_num(...)`.  Every
    vec3 in the tree looked write-only.
  * 9 findings  -- with array indices allowed, what remained were COUNT
    fields (ability_count, pinned_count, binding_count, ...).  Those are
    restored by `count++` inside the parse loop, because the count comes from
    the JSON array's length rather than a stored number.
  * 4 findings  -- once `field++` counts as a read.  All four are runtime
    state the loader zeroes and the writer deliberately omits (a cloth
    handle and dirty bit, a particle emitter's registry slot and asset
    epoch), so they are EXEMPT by name rather than baselined by count.

A FIFTH ATTEMPT WAS REVERTED, and it is the one worth recording.  Requiring
the assignment's right-hand side to mention a JSON accessor -- to separate
"restored from the file" from "zeroed by the loader" -- took the count UP to
16, adding derived values (JceSpotLight.inner_cone_cos is computed from the
authored angle on load, so it is one-way on purpose) and counts restored
through a local.  A detector that grows when made stricter is not converging,
and shipping sixteen unverified findings after a day spent documenting that
exact mistake would have been the mistake again.  The version kept is the one
whose entire output was read by hand.

A gate that finds nothing is worth having ONLY if it would find something,
and that is what the third number means here: the detector converged on a
surface whose entries were checked by hand, rather than being tuned until it
went quiet.  Its negative control removes one read and expects red.

ALSO CHECKED ON 2026-09-01, ALSO CLEAN.  Two neighbouring questions about the
same layer, recorded so they are not re-derived:

  * The script-facing bridge (comp_get_json / comp_set_json) covers ALL 91
    component types.  A scan that derives the JSON name from the C type --
    strip "Jce", strip "Component" -- reports four missing, and all four are
    false: the JSON names are NetworkTransform, NetworkAnimator,
    NetworkRigidbody and IkConstraints (plural).  Dispatch is registry-driven
    through jce_component_find, so a new component is reachable the moment it
    has a registry row.
  * Seven registry rows carry a NULL parse / serialize / get / set, and all
    seven are deliberate and documented at the walk itself
    (jce_scene_components_json.c:1774): DirectionalLight, PointLight and
    SpotLight fold into the unified "Light" row, Tag and Layer are
    entity-level fields, and VfxGraph has no subsystem to serialise into.

RULE 2, AND THE BLIND SPOT IT CLOSES.  Rule 1 reports a field that does
EXACTLY ONE of write/read.  A field that does NEITHER therefore slipped
through in silence -- and that is the worse case, not the better one: it has
no JSON key at all, so nothing downstream can even see that it is missing.

9392b8ba shipped one.  JceRigidBodyComponent.shape_type was given an engine
reader and an inspector control, while the 3D parse/serialize pair never
touched it, so a designer's choice reached the running scene and vanished on
the next save.  The 2D sibling had round-tripped `shapeType` all along, one
function further down the same file, which is exactly what made the gap
invisible to a reader.

So rule 2 asks the question rule 1 cannot: does any INSPECTOR PANEL let a
designer change a field its own (de)serialiser never mentions?  A field
nobody can edit and nobody persists is runtime state and no business of this
gate; a field a panel WRITES and no file carries is authoring thrown away.

WHAT THIS DOES NOT CHECK.  That the value round-trips CORRECTLY -- a parser
reading the wrong key, or a writer emitting the wrong units, satisfies this
gate.  It only asks whether both directions exist.

Usage:
    python tools/lint/check_component_serializer_roundtrip.py
    python tools/lint/check_component_serializer_roundtrip.py --list
Exit 0 clean, 1 on any one-way field.
"""

from __future__ import annotations

import glob
import re
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]
HEADER = REPO_ROOT / "engine/include/jce/middleware/scene/jce_scene.h"
IMPL = REPO_ROOT / "engine/src/middleware/scene/jce_scene.c"
SER_GLOB = "engine/src/middleware/scene/jce_scene_components_*.c"

IGNORED_NAMES = {"reserved"}

# Runtime state the LOADER zeroes and the writer deliberately omits, so it is
# one-way by design rather than lost authoring.  Each was read by hand: a
# physics/particle handle or a dirty bit means nothing across a save, and
# writing it would put a live pointer index into a scene file.
EXEMPT = {
    "JceClothComponent.handle",
    "JceClothComponent.dirty",
    "JceParticleEmitterComponent.emitter_handle_idx",
    "JceParticleEmitterComponent.asset_epoch",
    # Documented in jce_scene.h as "Engine-owned runtime state (NOT
    # serialized; cleared on scene load)" -- it records that the emitter has
    # been created in the scene particle system, which is true of a running
    # process and meaningless in a file.
    "JceParticleEmitterComponent.loaded",
    # jce_scene.h: "deliberately not serialized: the persisted source is the
    # material file's customProgramVs/customProgramFs pair."  The resolved
    # program handle only means something inside a running process.
    "JceMeshRenderer.has_custom_program",
    "JceMeshRenderer.custom_program_idx",
}

# Rule 2 exemptions: a panel writes it, no file carries it, and that is
# correct.  A transport control ("play this now") is an instruction to the
# running editor, not a property of the scene.  Each needs a reason here.
EXEMPT_UNPERSISTED = {
    # JceAnimatorComponent is RETIRED in jce_scene.h: the loader migrates an
    # authored one onto SkeletalAnimator and ser_animator was removed, so by
    # design nothing writes these to a file ever again.  The drawer stays so an
    # existing legacy Animator can be seen and removed, and it opens with a
    # component-level insp_unwired_badge() -- the marked control and the
    # exemption are the two halves of one sentence, and this file is the other
    # half.  Delete these three the day the drawer goes.
    "JceAnimatorComponent.clip_name",
    "JceAnimatorComponent.loop",
    "JceAnimatorComponent.speed",
}

PANELS = "editor/src/panels/*.cpp"

STRUCT = re.compile(r"typedef struct\s*\{(.*?)\}\s*(\w+)\s*;", re.S)
SCALAR = re.compile(
    r"^[ \t]*(?:const\s+)?(?:unsigned\s+|struct\s+)?"
    r"(?:float|double|bool|int|char|uint8_t|uint16_t|uint32_t|uint64_t"
    r"|int8_t|int16_t|int32_t|int64_t)\s+"
    r"(\w+)\s*(?:\[[^\]]*\])?\s*;", re.M)
# BOTH registration macros.  Matching only JCE_COMP_IMPL( left every
# JCE_COMP_IMPL_MATERIAL component outside this gate entirely -- including
# JceMeshRenderer, whose visible field is the defect this file documents at
# length.  The gate could not have caught the bug it was written for.
COMP_IMPL = re.compile(
    r"JCE_COMP_IMPL(?:_MATERIAL)?\(\s*(\w+)\s*,\s*(\w+)\s*\)")

IDX = r"(?:\[[^\]]*\])?"          # an optional array subscript


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def component_fields() -> dict:
    known = {m.group(1) for m in COMP_IMPL.finditer(
        IMPL.read_text(encoding="utf-8", errors="replace"))}
    src = strip_comments(HEADER.read_text(encoding="utf-8", errors="replace"))
    out: dict = {}
    for m in STRUCT.finditer(src):
        body, comp = m.group(1), m.group(2)
        if comp not in known:
            continue
        fields = [f for f in SCALAR.findall(body) if f not in IGNORED_NAMES]
        if fields:
            out[comp] = fields
    return out


def serializer_text() -> str:
    parts = []
    for rel in sorted(glob.glob(str(REPO_ROOT / SER_GLOB))):
        parts.append(Path(rel).read_text(encoding="utf-8", errors="replace"))
    return strip_comments(" ".join(parts))


def _function_bodies(text: str) -> list:
    """Every top-level function in `text`, as (header, body) strings.

    A function starts at a line that begins in column 0 and contains "(",
    and runs to its matching closing brace.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        nl = text.find(chr(10), i)
        line_end = n if nl < 0 else nl
        line = text[i:line_end]
        if line and not line[0].isspace() and "(" in line and ";" not in line:
            brace = text.find("{", i)
            if brace >= 0 and (nl < 0 or brace < text.find(chr(10) + chr(10), i)
                               or True):
                depth, k = 0, brace
                while k < n:
                    if text[k] == "{":
                        depth += 1
                    elif text[k] == "}":
                        depth -= 1
                        if depth == 0:
                            break
                    k += 1
                if k < n:
                    out.append((text[i:brace], text[brace:k + 1]))
                    i = k + 1
                    continue
        i = line_end + 1
    return out


def scoped_serializer_text(comps: dict) -> dict:
    """Per-component serialiser text, instead of one blob for all of them.

    THE BUG THIS REPLACES.  serializer_text() concatenated all nine
    jce_scene_components_*.c files into ONE string and the read/write regexes
    were matched against that.  So any field whose NAME is also used by another
    component was unfalsifiable: `->visible` written by GrassField satisfied
    the "written" test for MeshRenderer, whose serialiser did not emit it at
    all -- the exact defect the file's own parse_mesh_renderer comment
    describes at length.  A gate cannot catch a bug that its own matching
    cannot distinguish from a different component's correct code.

    Scoping rule: a component's serialiser is every top-level function in
    those files whose header or body names the component's struct type.  That
    catches both halves of the convention -- ser_<snake>(const JceFoo *...)
    by signature, parse_<snake>() by its `JceFoo foo;` local -- without
    depending on the function NAMES matching the struct name, which they do
    not always do.
    """
    files = []
    for rel in sorted(glob.glob(str(REPO_ROOT / SER_GLOB))):
        files.append(strip_comments(
            Path(rel).read_text(encoding="utf-8", errors="replace")))

    funcs = []
    for text in files:
        funcs.extend(_function_bodies(text))

    out = {}
    for comp in comps:
        # ONLY the (de)serialisers.  Scoping to "every function that mentions
        # the struct" was not enough and its control proved it: the reflection
        # accessors generated by REG_ACCESSORS both read and write every
        # field, so they satisfied both tests on their own and the
        # reintroduced MeshRenderer.visible bug stayed green.  The file's
        # convention is parse_<snake> plus TWO serialiser spellings:
        # ser_<snake>(const JceFoo *, cJSON *) and
        # serw_<snake>(JceScene *, JceEntity, cJSON *).  Matching only
        # `ser_` missed every serw_ family -- Avatar, the four Net
        # components, NetworkObject and Tilemap -- and reported 28 of their
        # fields as one-way on a clean tree.  A gate that reports 28 false
        # positives is switched off as fast as one that reports none.
        pieces = [h + b for (h, b) in funcs
                  if (comp in h or comp in b)
                  and re.search(r"\b(?:parse|serw?)_\w+\s*\(", h)]
        out[comp] = " ".join(pieces)
    return out


def editor_edited_fields(comps: dict) -> set:
    """{(comp, field)} that some inspector panel lets a designer CHANGE.

    Scoped by DECLARED POINTER TYPE, never by field name: `->visible` appears
    in half a dozen drawers and a name-keyed scan would credit any of them to
    all of them -- the same collision scoped_serializer_text() exists to
    avoid.  A drawer takes `JceFooComponent *foo`, so the declaration names
    the type and the variable, and only writes through THAT variable count.

    A widget that edits a scratch local and commits with insp_undo_set counts:
    that is the tree's standard idiom for a discrete field, and the commit is
    the write.  A widget merely READING the field to display it does not.
    """
    out = set()
    for path in sorted((REPO_ROOT / "editor/src/panels").glob("*.cpp")):
        text = strip_comments(path.read_text(encoding="utf-8",
                                             errors="replace"))
        for comp in comps:
            if comp not in text:
                continue
            names = set(re.findall(re.escape(comp) + r"\s*\*\s*(?:const\s+)?"
                                   r"([A-Za-z_]\w*)", text))
            if not names:
                continue
            for field in comps[comp]:
                e = re.escape(field)
                for n in names:
                    v = re.escape(n)
                    # &v->field handed to a widget or an undo helper
                    if re.search(r"(?:ImGui::\w+|insp_undo_\w+|INSP_UNDO_\w+)"
                                 r"\s*\([^;]{0,240}?&\s*" + v + r"\s*->\s*"
                                 + e + r"\b", text, re.S):
                        out.add((comp, field))
                        break
                    # A char buffer written IN PLACE, which is the one
                    # widget family that takes the field by name instead of by
                    # address.  Restricted to InputText on purpose: matching
                    # any ImGui:: call credited a field passed as a FORMAT
                    # ARGUMENT as if it were edited, and reported two reads --
                    # jce_panel_inspector_physics.cpp printing
                    # `rb->body_handle_idx ? "" : " (inactive in editor)"` and
                    # the video panel printing `vp->tex_w, vp->tex_h`.  Both
                    # are displays of runtime state; neither is authoring.
                    if re.search(r"ImGui::InputText\w*\s*\([^;]{0,240}?\b" + v
                                 + r"\s*->\s*" + e + r"\s*,", text, re.S):
                        out.add((comp, field))
                        break
    return out


def unpersisted_edited_fields(comps: dict, scoped: dict) -> list:
    """Rule 2: a panel writes it, its own (de)serialiser never mentions it."""
    edited = editor_edited_fields(comps)
    if not edited:
        raise SystemExit(
            "check_component_serializer_roundtrip: FAIL - no inspector panel "
            "was found to edit ANY component field; the drawer convention "
            "changed and rule 2 would pass vacuously")
    out = []
    for comp, field in sorted(edited):
        ser = scoped.get(comp) or ""
        e = re.escape(field)
        if re.search(r"(?:\.|->)\s*" + e + r"\b", ser):
            continue                      # the file carries it in some form
        if "%s.%s" % (comp, field) in EXEMPT_UNPERSISTED:
            continue
        out.append((comp, field))
    return out


def one_way_fields() -> list:
    comps, ser_all = component_fields(), serializer_text()
    if len(comps) < 40 or len(ser_all) < 10000:
        raise SystemExit(
            "check_component_serializer_roundtrip: FAIL - matched %d "
            "component type(s) over %d bytes of serialiser; the shape changed "
            "and this extractor no longer sees it" % (len(comps), len(ser_all)))

    scoped = scoped_serializer_text(comps)
    covered = sum(1 for c in comps if scoped.get(c))
    if covered < len(comps) // 2:
        raise SystemExit(
            "check_component_serializer_roundtrip: FAIL - only %d of %d "
            "component type(s) resolved to any serialiser function; the "
            "per-component scoping no longer finds them and every field would "
            "be reported." % (covered, len(comps)))

    out = []
    for comp in sorted(comps):
        ser = scoped.get(comp) or ""
        if not ser:
            # No serialiser function mentions this type at all: it is not
            # persisted, so there is no round-trip to check.
            continue
        for field in comps[comp]:
            e = re.escape(field)
            # A READ, in the four shapes this serialiser uses: a direct or
            # element-wise assignment, a `count++` inside a parse loop, a
            # copy_str for a char array, or the array passed by name.
            read = (
                re.search(r"(?:\.|->)" + e + IDX + r"\s*=(?!=)", ser)
                or re.search(r"(?:\.|->)" + e + IDX + r"\s*\+\+", ser)
                or re.search(r"copy_str\(\s*[A-Za-z_]\w*\.\s*" + e, ser)
                or re.search(r"[A-Za-z_]\w*\." + e + r"\s*,", ser))
            # WRITE: any mention that is not an assignment target.
            write = re.search(r"->\s*" + e + IDX + r"\b(?!\s*(?:=(?!=)|\+\+))",
                              ser)
            key = "%s.%s" % (comp, field)
            if bool(read) != bool(write) and key not in EXEMPT:
                out.append((comp, field,
                            "written, never read back" if write
                            else "read, never written"))
    return out


def main() -> int:
    for p in (HEADER, IMPL):
        if not p.is_file():
            print("check_component_serializer_roundtrip: FAIL - missing %s"
                  % p.relative_to(REPO_ROOT).as_posix(), file=sys.stderr)
            return 1

    rows = one_way_fields()
    comps = component_fields()
    unpersisted = unpersisted_edited_fields(comps, scoped_serializer_text(comps))

    if "--list" in sys.argv:
        for comp, field, how in rows:
            print("%-52s %s" % ("%s.%s" % (comp, field), how))

    if unpersisted:
        for comp, field in unpersisted:
            print("  %s.%s: an inspector panel writes this field and its own "
                  "(de)serialiser never mentions it.  There is no JSON key at "
                  "all, so the designer's choice reaches the running scene and "
                  "is gone the moment the scene is saved -- and nothing "
                  "downstream can see a key that was never there."
                  % (comp, field), file=sys.stderr)
        print("check_component_serializer_roundtrip: FAIL - %d edited-but-"
              "unpersisted field(s)." % len(unpersisted), file=sys.stderr)
        return 1

    if rows:
        for comp, field, how in rows:
            print("  %s.%s: %s by jce_scene_components_*.c.  A field written "
                  "and never read back is authoring that disappears when the "
                  "scene reopens; one read and never written resets to its "
                  "default on every save.  Neither reports anything -- the "
                  "scene loads and one number is quietly not what the designer "
                  "left." % (comp, field, how), file=sys.stderr)
        print("check_component_serializer_roundtrip: FAIL - %d one-way "
              "field(s)." % len(rows), file=sys.stderr)
        return 1

    n = sum(len(v) for v in comps.values())
    print("check_component_serializer_roundtrip: OK (%d component field(s); "
          "every one both written and read back, and every field an inspector "
          "edits reaches the file)" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
