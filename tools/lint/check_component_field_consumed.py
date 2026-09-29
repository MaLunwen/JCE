#!/usr/bin/env python3
"""
check_component_field_consumed.py — a component field the engine never reads.

WHY THIS EXISTS.  check_authored_path_consumed.py asks "who reads this path?"
of every authored path on a scene component.  The same question asked of the
SCALAR fields finds more, and the pair that was chased to the bottom was real:
JceWheelColliderComponent.forward_friction and .sideways_friction were
authored, serialised, editable in the inspector, and thrown away -- the
scene-to-physics bridge passed a hardcoded friction_slip of 1000 instead.  A
designer could tune wheel grip and the vehicle handled identically.

The failure has no symptom.  An ignored field behaves EXACTLY like a field
holding its default, so nothing reports it and nothing looks wrong.  Only the
question finds it.

PER COMPONENT, NOT PER NAME, and that distinction is why this file was
rewritten within an hour of first shipping.  The first version asked whether
the NAME appeared anywhere, so a field was counted as read whenever any OTHER
component had a field of the same name that something read.  It reported 32
unread fields; keyed per component the answer is far larger, and the gap was
not academic -- JceReflectionProbeComponent has NINE fields the engine never
touches (mode, resolution, blend_distance, box_size, near_clip, far_clip,
box_projection, hdr, custom_hdr_path; only baked_cubemap_path, box_offset and
intensity are read) and the name-keyed version found four of them, because
`resolution`, `blend_distance`, `hdr` and `mode` are read on other components.

HOW A READ IS DETECTED.  For each component type, every variable in an engine
source declared as a pointer to it -- a local, a parameter, a callback's
argument -- and then `var->field` on one of those variables.  Assignment from
the getter is NOT the test: an earlier attempt used it and reported whole
components as dead because their readers take the component as a CALLBACK
PARAMETER (JceClothComponent, JceBillboardRendererComponent and
JceUIDropdownComponent all vanished from the list when this was fixed).

A RATCHET, NOT A VERDICT.  The baseline freezes what has no reader today; the
gate fails when a new one appears.  Entries are CANDIDATES:

  * an EDITOR-ONLY component is unread by the engine ON PURPOSE -- JceEditorMeta
    (tag, tag_color, layer) is in the baseline and is not a defect;
  * a text scan cannot see a reader that copies the component into another
    struct first;
  * only the wheel friction pair was verified end to end.

Do not cite the baseline as a list of bugs.  That mistake has already been
made on this branch, when three of five written-out exemption reasons turned
out to rest on searches that came back empty for the wrong reason.

IT READS THE GIT INDEX, so a NEW FILE that is not staged yet is invisible to
it.  Splitting a file and moving readers into an untracked TU makes those
fields look newly unread -- seven of them, on 2026-09-01, when the dropdown
and input field moved out of jce_ui_canvas.c.  `git add` the new file and the
gate goes green.  That is the right behaviour for a gate whose baseline is a
tracked contract, but it reads as a regression, so it is written down here.
(check_shader_uniform_bound.py made the opposite choice, scanning the
filesystem, because it has no baseline to keep honest.)

Two deliberate exclusions: `reserved`, which is forward-compat padding and is
SUPPOSED to have no reader, and jce_scene_components_*.c, because reading a
field only to write it back out is serialisation rather than consumption --
that distinction is the whole gate.

To clear an entry: wire a reader, then rerun --update-baseline and say in the
commit message which field gained one.  Shrinking is always allowed.

RULE 2 -- THE MIRROR QUESTION.  "Which fields does the engine READ that no
inspector panel mentions?"  It found a shipped defect the first time it was
asked: JceSpawnManagerComponent.ped_prefab_path gated the entire ped spawn
(jce_runtime.c:192) with no control anywhere in the editor, so a SpawnManager
placed from the inspector spawned nothing while max_peds, both radii and the
interval sat above it fully tunable.  Fixed in e0dad09c.

Its precision is deliberately low: 24 of the 25 it first returned were
runtime state that must NEVER be authored (loco_* on the skeletal animator,
*_handle_idx registry slots, started / opened_hash / prev_time, a
server-assigned net_id, a dropdown's `expanded`).  Those are baselined.  The
rule exists for the next ped_prefab_path, not to shrink the list.

Usage:
    python tools/lint/check_component_field_consumed.py
    python tools/lint/check_component_field_consumed.py --list
    python tools/lint/check_component_field_consumed.py --update-baseline
Exit 0 clean, 1 on any newly unread field.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]
HEADER = REPO_ROOT / "engine/include/jce/middleware/scene/jce_scene.h"
IMPL = REPO_ROOT / "engine/src/middleware/scene/jce_scene.c"
REGISTRY = REPO_ROOT / "engine/src/middleware/scene/jce_scene_components_json.c"
BASELINE = LINT_DIR / "component_field_baseline.json"

DEFAULT_NOTE = (
    "Two lists, both CANDIDATES rather than confirmed defects -- see the "
    "docstring in check_component_field_consumed.py before citing either.  "
    "`fields`: Component.field with no reader under engine/src outside the "
    "JSON serialisers; EVERY entry must carry a `reasons` entry saying why, "
    "and the gate fails on one that does not (an unexplained entry is how a "
    "field that could be wired in an afternoon sits beside one that needs a "
    "subsystem, with nothing to tell them apart).  `unauthorable`: "
    "Component.field the engine DOES read and no inspector panel mentions -- "
    "mostly runtime state that must not be authored, kept so the next "
    "ped_prefab_path is noticed."
)

SERIALIZERS = ("jce_scene_components_",)
IGNORED_NAMES = {"reserved"}

STRUCT = re.compile(r"typedef struct\s*\{(.*?)\}\s*(\w+)\s*;", re.S)
SCALAR = re.compile(
    r"^[ \t]*(?:const\s+)?(?:unsigned\s+|struct\s+)?"
    r"(?:float|double|bool|int|char|uint8_t|uint16_t|uint32_t|uint64_t"
    r"|int8_t|int16_t|int32_t|int64_t)\s+"
    r"(\w+)\s*(?:\[[^\]]*\])?\s*;", re.M)
COMP_IMPL = re.compile(r"JCE_COMP_IMPL\(\s*(\w+)\s*,\s*(\w+)\s*\)")
TRAILING_IDENT = re.compile(r"([A-Za-z_]\w*)\s*$")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def retired_components() -> set:
    """Registry names whose serialize hook is NULL.

    Such a row is a RETIRED component: the loader migrates it onto its
    successor and nothing ever saves it again, so its fields cannot be
    authored and "authored but unread" is the wrong question to ask about
    them.  Nine entries were sitting in the baseline for exactly this reason
    (JceAnimatorComponent and JceVfxGraphComponent, both marked RETIRED in
    jce_scene.h), which makes a list meant to be a work queue nine items
    longer than the work.

    REG(name, key, _, _, flag, has, remove, PARSE, SERIALIZE, get, set, size)
    -- serialize is argument index 8.
    """
    src = strip_comments(REGISTRY.read_text(encoding="utf-8", errors="replace"))
    out = set()
    for m in re.finditer(r'\bREG\s*\(', src):
        i, depth, args, start = m.end(), 1, [], m.end()
        while i < len(src) and depth:
            ch = src[i]
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    args.append(src[start:i])
                    break
            elif ch == "," and depth == 1:
                args.append(src[start:i])
                start = i + 1
            i += 1
        args = [a.strip() for a in args]
        if len(args) > 8 and args[8] == "NULL":
            out.add(args[0].strip().strip(chr(34)))
    if not out:
        raise SystemExit(
            "check_component_field_consumed: FAIL - the REG() shape changed "
            "and no retired component was recognised; a silently empty "
            "exclusion set would re-list every retired field as a gap")
    return out


def retired_fields() -> dict:
    """{component: {field}} marked RETIRED in a trailing comment.

    ONE LEVEL DOWN FROM retired_components().  That one drops a whole
    component whose registry serialize hook is NULL.  This one drops a single
    field that cannot be authored either, for the same reason and with the
    same consequence: "authored but unread" is the wrong question to ask about
    it, and leaving it in makes a work queue longer than the work.

    JceBehaviorTree.context_handle_idx was the first.  It asks a scene
    component to select one of several JceBtContexts; jce_bt.h gives an opaque
    create/destroy with no index space at all, and the runtime owns exactly
    one, published as jce_runtime_bt_context().  No writer, no JSON key in
    either direction, no inspector control -- it has been 0 in every process
    and every file that has ever existed.

    THE MARKER IS NOT FREE, and that is the design.  Claiming RETIRED costs
    the ability to ever author the field: the second half of this rule fails
    if a retired field is named by a serializer, an inspector panel or the
    component defaults.  Without that half, "RETIRED" would be a one-word
    escape hatch out of a ratchet gate, which is exactly the shape CLAUDE.md
    warns about; with it, the claim is a checkable commitment.

    Read from the RAW header on purpose.  component_fields() strips comments
    before matching fields, so the marker is gone by the time it looks -- a
    detector that reads the stripped text would find nothing and silently
    exclude nothing, which is the failure mode that looks exactly like
    "there is nothing to exclude".
    """
    raw = HEADER.read_text(encoding="utf-8", errors="replace").splitlines()
    close = re.compile(r"^\}\s*(\w+)\s*;")
    decl = re.compile(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;")
    out: dict = {}
    for i, line in enumerate(raw):
        if "RETIRED" not in line:
            continue
        code = line.split("/*")[0]
        m = None
        for m in decl.finditer(code):
            pass
        if not m:
            continue                      # a RETIRED comment on its own line
        field = m.group(1)
        for j in range(i + 1, min(i + 400, len(raw))):
            c = close.match(raw[j])
            if c:
                out.setdefault(c.group(1), set()).add(field)
                break
    return out


def retired_field_leaks(retired: dict) -> list:
    """A RETIRED field named on any authoring surface.

    This is what the marker costs.  A serializer that emits it, a panel that
    edits it or a defaults blob that sets it all mean the field IS authorable,
    so the retirement claim is false and the exclusion must not stand.
    """
    surfaces = []
    for pat in ("engine/src/middleware/scene/jce_scene_components_*.c",
                "editor/src/panels/*.cpp",
                "editor/src/core/jce_editor_component_defaults.cpp"):
        for p in sorted(REPO_ROOT.glob(pat)):
            surfaces.append((p.relative_to(REPO_ROOT).as_posix(),
                             strip_comments(p.read_text(encoding="utf-8",
                                                        errors="replace"))))
    if len(surfaces) < 10:
        raise SystemExit(
            "check_component_field_consumed: FAIL - only %d authoring "
            "surface file(s) found; a silently empty scan would make the "
            "RETIRED marker a free escape hatch" % len(surfaces))
    bad = []
    for comp, fields in sorted(retired.items()):
        for field in sorted(fields):
            hit = re.compile(r"(?:\.|->)\s*" + re.escape(field) + r"\b")
            for rel, text in surfaces:
                if hit.search(text):
                    bad.append((comp, field, rel))
                    break
    return bad


def component_fields() -> dict:
    """component type -> [field names], for types the scene exposes."""
    known = {m.group(1) for m in COMP_IMPL.finditer(
        IMPL.read_text(encoding="utf-8", errors="replace"))}
    src = strip_comments(HEADER.read_text(encoding="utf-8", errors="replace"))
    out: dict = {}
    retired = retired_components()
    retired_f = retired_fields()
    for m in STRUCT.finditer(src):
        body, comp = m.group(1), m.group(2)
        if comp not in known:
            continue
        # "JceFooComponent" -> the registry's "Foo"; a few types drop the
        # Component suffix (JceEditorMeta), so strip it only when present.
        reg = comp[3:]
        if reg.endswith("Component"):
            reg = reg[:-len("Component")]
        if reg in retired:
            continue
        gone = retired_f.get(comp, set())
        fields = [f for f in SCALAR.findall(body)
                  if f not in IGNORED_NAMES and f not in gone]
        if fields:
            out[comp] = fields
    return out


def engine_texts() -> dict:
    listing = subprocess.run(["git", "ls-files", "engine/src"],
                             cwd=REPO_ROOT, capture_output=True, text=True)
    out = {}
    for rel in listing.stdout.split():
        if not rel.endswith((".c", ".cpp")):
            continue
        if any(k in rel for k in SERIALIZERS):
            continue
        out[rel] = strip_comments(
            (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace"))
    return out


def panel_texts() -> dict:
    """Inspector panel sources.  The editor's authoring surface."""
    out = {}
    for p in sorted((REPO_ROOT / "editor/src/panels").glob("*.cpp")):
        out[p.name] = strip_comments(
            p.read_text(encoding="utf-8", errors="replace"))
    return out


# A top-level definition starts at column 0 with an identifier character --
# not a closing brace, not a preprocessor line, not whitespace.  Good enough
# to cut a C file into per-function chunks, which is all the scoping this
# needs.
TOPLEVEL = re.compile(r"^(?=[A-Za-z_])", re.M)


def scopes(text: str) -> list:
    """`text` cut into chunks that approximate one top-level definition each.

    WHY THIS EXISTS.  reads() used to collect the pointer-variable names for a
    component over a WHOLE FILE and then ask whether any `->field` in that
    file had one of those names in front of it.  Two components whose
    pointers are spelled the same in one file therefore credited each other's
    reads -- which is not hypothetical: jce_rt_physics.c calls both a
    JceRigidBodyComponent and a JceRigidBody2DComponent `rb`, so the 2D
    body's `rb->body_type` counted as a reader for the 3D component's
    body_type -- a field that is never parsed, never serialised, has no
    inspector control, and whose one real reader was removed in the same
    commit that added this.  The gate reported "all baselined" over a field
    with no reader at all.

    Chunking by function does not make the matching sound: a `#if` arm or a
    nested block can still fool it.  It removes the collision that actually
    occurs, which is two same-named locals in two functions.
    """
    cuts = [m.start() for m in TOPLEVEL.finditer(text)]
    if not cuts:
        return [text]
    cuts.append(len(text))
    return [text[cuts[k]:cuts[k + 1]] for k in range(len(cuts) - 1)]


def _self_test() -> None:
    """That collision, in eight lines, run on every invocation."""
    sample = (
        "void a(JceRigidBodyComponent *rb)\n"
        "{\n"
        "\tuse(rb->mass);\n"
        "}\n"
        "void b(JceRigidBody2DComponent *rb)\n"
        "{\n"
        "\tuse(rb->body_type);\n"
        "}\n")
    comps = {"JceRigidBodyComponent": ["mass", "body_type"],
             "JceRigidBody2DComponent": ["body_type"]}
    got = reads(comps, {"x.c": sample})
    assert ("JceRigidBodyComponent", "mass") in got, got
    assert ("JceRigidBody2DComponent", "body_type") in got, got
    assert ("JceRigidBodyComponent", "body_type") not in got, (
        "a same-named pointer to a DIFFERENT component was credited as a "
        "reader: " + repr(sorted(got)))


def reads(comps: dict, texts: dict) -> set:
    """{(component, field)} that some file in `texts` reads as var->field.

    A variable counts when it is DECLARED as a pointer to the component --
    local, parameter or callback argument.  See the docstring above for why
    assignment-from-getter was not enough.

    ALSO counts a component copied BY VALUE into a wrapper struct and read
    through it.  jce_vcam_system.c does exactly that -- `ctx->best = *vc;`
    then `ctx.best.damping` -- and pointer-declaration matching alone reported
    JceVirtualCameraComponent.damping as a field nothing reads, while the
    damping implementation sat right there.  A list meant to be a work queue
    cannot afford entries that are already done.
    """
    # One entry per (file, scope), not per file: see scopes().
    #
    # A chunk whose first line has no "(" is not a function -- a typedef, a
    # struct, a file-scope variable -- and the names it declares ARE visible
    # from every function in the file.  jce_rt_audio.c's listener scan reads
    # `als.lc->volume`, where `lc` is a POINTER MEMBER of a struct declared at
    # file scope; cutting the file into function scopes without this would put
    # that declaration out of reach and report four AudioListener fields the
    # engine plainly reads.  Function-local names stay local, which is the
    # whole point.
    file_scoped: dict = {}
    func_chunks = []
    for text in texts.values():
        for chunk in scopes(text):
            if "(" in chunk.split("\n", 1)[0]:
                func_chunks.append(chunk)
            else:
                file_scoped.setdefault(id(text), []).append(chunk)
    outer = {}
    for text in texts.values():
        outer[id(text)] = "\n".join(file_scoped.get(id(text), []))

    ptr: dict = {}
    for comp in comps:
        decl = re.compile(re.escape(comp)
                          + r"\s*\*\s*(?:const\s+)?([A-Za-z_]\w*)")
        outer_names = {k: set(decl.findall(v)) for k, v in outer.items()}
        for text in texts.values():
            base = outer_names.get(id(text), set())
            for chunk in scopes(text):
                if "(" not in chunk.split("\n", 1)[0]:
                    continue
                names = set(decl.findall(chunk)) | base
                if names:
                    ptr.setdefault(comp, []).append((chunk, names))
            if base:
                # Reads that sit outside any function (initialisers, macros).
                ptr.setdefault(comp, []).append((outer[id(text)], base))

    # Struct members declared BY VALUE with a component type: the member name
    # is then a legitimate path to the fields.
    VALUE_MEMBER: dict = {}
    for comp in comps:
        mdecl = re.compile(re.escape(comp) + r"\s+([A-Za-z_]\w*)\s*;")
        for rel, text in texts.items():
            names = set(mdecl.findall(text))
            if names:
                VALUE_MEMBER.setdefault(comp, []).append((text, names))

    out = set()
    for comp in comps:
        files = ptr.get(comp, [])
        for text, names in VALUE_MEMBER.get(comp, []):
            for field in comps[comp]:
                if (comp, field) in out:
                    continue
                for n in names:
                    if re.search(r"\." + re.escape(n) + r"\s*\.\s*"
                                 + re.escape(field) + r"\b", text) or \
                       re.search(r"->\s*" + re.escape(n) + r"\s*\.\s*"
                                 + re.escape(field) + r"\b", text):
                        out.add((comp, field))
                        break
        for field in comps[comp]:
            arrow = re.compile(r"->\s*" + re.escape(field) + r"\b")
            for text, names in files:
                hit = False
                for m in arrow.finditer(text):
                    pre = text[max(0, m.start() - 64):m.start()]
                    ident = TRAILING_IDENT.search(pre)
                    if ident and ident.group(1) in names:
                        hit = True
                        break
                if hit:
                    out.add((comp, field))
                    break
    return out


def unread_fields() -> list:
    """[(component, field)] with no `var->field` on a var of that type."""
    comps, texts = component_fields(), engine_texts()
    if len(comps) < 40:
        raise SystemExit(
            "check_component_field_consumed: FAIL - matched only %d component "
            "type(s); the header or JCE_COMP_IMPL shape changed and this "
            "extractor no longer sees them" % len(comps))
    engine = reads(comps, texts)
    return [(c, f) for c in sorted(comps) for f in comps[c]
            if (c, f) not in engine]


def unauthorable_fields() -> list:
    """[(component, field)] the ENGINE reads and NO inspector panel mentions.

    The mirror question, and it found a shipped defect the first time it was
    asked: JceSpawnManagerComponent.ped_prefab_path gated the whole ped spawn
    (jce_runtime.c:192, `if (!smc || !smc->ped_prefab_path[0]) return 0;`)
    and had no control in the inspector, so a SpawnManager placed from the
    editor spawned nothing while max_peds, both radii and the interval sat
    above it fully tunable.  e0dad09c added the control.

    TWO WAYS IT REPORTS A FIELD THAT IS ACTUALLY AUTHORABLE, both found by
    checking its output rather than trusting it:

      * authored through a SETTER API instead of a component pointer.
        JceLayerComponent.layer is set by jce_scene_set_entity_layer(), which
        the inspector calls -- this scan looks for `var->field` on a variable
        declared as a pointer to the component and sees none.
      * authored through an INLINE CALL result:
        `jce_scene_get_spot_light(scene, e)->radius = radius;` in
        jce_panel_inspector_lighting.cpp has no declared variable to match.

    So a hit here is a question, not a verdict.  Two of the three chased on
    2026-09-01 were these; the third,  JceWaterComponent.depth_write, was
    real and got a control.

    PRECISION IS LOW ON PURPOSE.  Most of what this returns is RUNTIME STATE
    that must never be authored -- loco_* on the skeletal animator, the
    *_handle_idx registry slots, started / opened_hash / prev_time,
    JceNetworkObjectComponent's server-assigned net_id and its cached
    is_owner, a dropdown's `expanded`.  Twenty-four of the twenty-five it
    first returned were of that kind and are baselined.  The gate exists for
    the next ped_prefab_path, not to shrink the list.
    """
    comps = component_fields()
    engine = reads(comps, engine_texts())
    panels = reads(comps, panel_texts())
    return sorted(engine - panels)



# ── Reasons ──────────────────────────────────────────────────────────
#
# A bare list of names tells the next reader nothing.  Some entries are a
# missing WIRE someone could close in an afternoon; some need a SUBSYSTEM that
# does not exist (an HRTF path, a humanoid-rig loader, multi-camera
# rendering); one is a DECIDED NO whose control should arguably go rather than
# gain a reader.  Undistinguished they all read the same, and the tool that
# surfaces them to a scene author (tools/jce_scene_kit.py) could only say
# "inert".
#
# This mirrors tools/lint/authored_path_exempt.txt, whose own header records
# why: "an exemption list that outlives its reason is how the next reader
# learns the wrong thing."  It also records that THREE of its first reasons
# were wrong the same way -- a search came back empty for a reason other than
# the one claimed -- so every reason here names the file it was read from.

MIN_REASON_CHARS = 80


def reason_problems() -> list:
    """Every baselined field needs a reason; every reason needs a field."""
    if not BASELINE.is_file():
        return []
    d = json.loads(BASELINE.read_text(encoding="utf-8"))
    reasons = d.get("reasons") or {}
    fields = set(d.get("fields") or [])
    out = []
    for f in sorted(fields):
        why = (reasons.get(f) or "").strip()
        if not why:
            out.append(
                "%s has no entry in `reasons`.  A field with no engine reader "
                "is either a missing WIRE or a missing SUBSYSTEM, and telling "
                "them apart is the whole value of this list -- say which, and "
                "name the file you read it from.  (--update-baseline records a "
                "NEW field with an empty reason on purpose, so landing one is "
                "a decision rather than an omission.)" % f)
        elif len(why) < MIN_REASON_CHARS:
            out.append(
                "%s: the reason is %d characters, which cannot be more than a "
                "restatement of the list.  Say what would have to exist, and "
                "cite where you read it." % (f, len(why)))
    for f in sorted(set(reasons) - fields):
        out.append(
            "%s has a reason but is not in `fields` -- it gained a reader, or "
            "the field is gone.  Delete the reason: one that outlives the "
            "thing it explains is how the next reader learns the wrong thing."
            % f)
    return out


def load_baseline() -> tuple:
    if not BASELINE.is_file():
        return set(), set()
    d = json.loads(BASELINE.read_text(encoding="utf-8"))
    return set(d.get("fields", [])), set(d.get("unauthorable", []))


def write_baseline(rows, unauth) -> None:
    """Rewrite the baseline, CARRYING THE REASONS FORWARD.

    A field that is still unread keeps the reason someone wrote for it; a
    field that gained a reader loses both entries together (a reason for a
    field that is no longer listed fails the gate, so it cannot be left
    behind); a NEW field gets an empty reason, which fails the gate on the
    next run -- deliberately, so that adding a dead field is a decision
    somebody makes rather than a line that appears."""
    keys = ["%s.%s" % (c, f) for c, f in rows]
    prev = {}
    if BASELINE.is_file():
        prev = (json.loads(BASELINE.read_text(encoding="utf-8"))
                .get("reasons") or {})
    prev_note = ""
    if BASELINE.is_file():
        prev_note = (json.loads(BASELINE.read_text(encoding="utf-8"))
                     .get("_note") or "")

    BASELINE.write_text(json.dumps({
        # KEEP WHAT SOMEONE WROTE.  This note used to be a literal here, so
        # every regeneration reverted the file's copy -- and the file's copy
        # had gained a sentence explaining why an unexplained entry is
        # dangerous.  Same shape as check_file_size.py's --raise overwriting a
        # stored reason: a generated file may own its DATA, but the prose
        # beside it belongs to whoever last explained something.
        "_note": prev_note or DEFAULT_NOTE,
        "fields": keys,
        "unauthorable": ["%s.%s" % (c, f) for c, f in unauth],
        "reasons": {k: prev.get(k, "") for k in sorted(keys)},
    }, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    _self_test()
    for path in (HEADER, IMPL):
        if not path.is_file():
            print("check_component_field_consumed: FAIL - missing %s"
                  % path.relative_to(REPO_ROOT).as_posix(), file=sys.stderr)
            return 1

    # What the RETIRED marker costs.  Checked BEFORE the ratchet so a false
    # retirement can never be recorded into the baseline by --update-baseline:
    # that would freeze the escape hatch open.
    leaks = retired_field_leaks(retired_fields())
    if leaks:
        for comp, field, rel in leaks:
            print("  %s.%s is marked RETIRED in jce_scene.h, and %s names it.  "
                  "A field a serializer emits, a panel edits or the defaults "
                  "blob sets IS authorable, so the retirement is false and the "
                  "exclusion must not stand -- either drop the marker or drop "
                  "the authoring." % (comp, field, rel), file=sys.stderr)
        print("check_component_field_consumed: FAIL - %d RETIRED field(s) are "
              "still authorable." % len(leaks), file=sys.stderr)
        return 1

    missing_reasons = reason_problems()
    if missing_reasons and "--update-baseline" not in sys.argv:
        for m in missing_reasons:
            print("  " + m, file=sys.stderr)
        print("check_component_field_consumed: FAIL - %d baseline entr(ies) "
              "without a usable reason." % len(missing_reasons),
              file=sys.stderr)
        return 1

    rows = unread_fields()
    unauth = unauthorable_fields()

    if "--update-baseline" in sys.argv:
        write_baseline(rows, unauth)
        print("baseline updated: %d unread, %d unauthorable"
              % (len(rows), len(unauth)))
        return 0

    if "--list" in sys.argv:
        print("-- no engine reader --")
        for comp, field in rows:
            print("%s.%s" % (comp, field))
        print("-- engine reads it, no inspector control --")
        for comp, field in unauth:
            print("%s.%s" % (comp, field))

    base_r, base_u = load_baseline()
    now_r = {"%s.%s" % (c, f) for c, f in rows}
    now_u = {"%s.%s" % (c, f) for c, f in unauth}
    new_r, new_u = sorted(now_r - base_r), sorted(now_u - base_u)
    gone = sorted((base_r - now_r) | (base_u - now_u))

    ok = True
    for key in new_r:
        ok = False
        print("  %s: authored on a scene component, serialised, and read by "
              "NOTHING under engine/src outside jce_scene_components_*.c.  A "
              "designer can set it and the engine cannot act on it, with no "
              "error -- an ignored field behaves exactly like one holding its "
              "default.  Wire a reader, or record it with --update-baseline "
              "and say why in the commit message." % key, file=sys.stderr)
    for key in new_u:
        ok = False
        print("  %s: the ENGINE reads this field and no inspector panel "
              "mentions it, so a designer cannot set it -- the capability "
              "exists and cannot be reached, which is how "
              "JceSpawnManagerComponent.ped_prefab_path made every authored "
              "SpawnManager spawn nothing.  Add a control, or record it with "
              "--update-baseline if it is runtime state that must not be "
              "authored." % key, file=sys.stderr)
    for key in gone:
        ok = False
        print("  %s: baselined as unreachable, and it is NOT any more -- the "
              "code moved and the allow-list did not.  This has to fail, not "
              "print: a name left in the baseline is a PERMANENT exemption, "
              "so if the reader is deleted tomorrow the field goes back to "
              "doing nothing and this gate stays green.  Its `reasons` "
              "paragraph is the second cost -- it argues the field cannot be "
              "wired, in the present tense, after it was.  Rerun with "
              "--update-baseline (which drops both) and say what wired it in "
              "the commit message." % key, file=sys.stderr)
    if not ok:
        print("check_component_field_consumed: FAIL - %d newly unread, %d "
              "newly unauthorable, %d cleared but still baselined."
              % (len(new_r), len(new_u), len(gone)), file=sys.stderr)
        return 1

    print("check_component_field_consumed: OK (%d with no engine reader, %d "
          "the engine reads but no panel exposes, all baselined)"
          % (len(now_r), len(now_u)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
