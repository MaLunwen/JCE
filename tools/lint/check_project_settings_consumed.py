#!/usr/bin/env python3
"""A Project Settings knob that stops at the editor window.

THE CLASS THIS EXISTS FOR, measured rather than imagined.  In one week this
tree shipped six separate defects of exactly one shape: a field in Project
Settings > Quality or > Graphics that the panel authors, the serialiser
round-trips, and NOTHING else ever reads --

    quality tier ......... written to .jce/editor-state.json, which is
                           machine-local editor state nothing exports
    texture quality ...... two callers, both under editor/src/
    anisotropic .......... same
    Target Framerate ..... the engine had no frame limiter at all
    Pixel Light Count .... no engine call accepted the number
    per-level AA ......... the resolver overwrote it with the project default

Every one of them looked correct in the viewport and shipped the wrong value,
which is the failure mode this repository warns about most: right in the
editor, wrong in the exe.  None of them had a symptom -- an ignored setting
behaves exactly like one holding its default.

THE RULE.  Each field of JceProjectQualityLevel and JceProjectGraphics must be
read somewhere outside the panel that authors it and the file that serialises
it, or carry a recorded reason saying what closing it would take.

MATCHING IS BY QUALIFIED PATH, not by field name.  `hdr`, `name` and
`anisotropic` are far too common to search for bare -- the first version of the
component-field gate made that mistake and credited one component's readers to
another.  So a graphics field counts only as `graphics.FIELD` / `graphics
->FIELD`, and a quality field only through `levels[...].FIELD` or a variable
declared `JceProjectQualityLevel *` in the same function.

WHAT THIS GATE DOES NOT CHECK, said out loud rather than implied: that the
value reaches the SHIPPED exe.  A field read only by another editor file
passes here.  jce_editor_effective_render_settings() is the one composition
both the viewport and the packager go through, so a reader outside it is worth
a second look -- but "is this reader on the shipping path" is a question about
call graphs, and a text scan cannot answer it.  check_runtime_desc_parity.py
guards the other end.
"""
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
HEADER    = REPO_ROOT / "editor/src/core/jce_project_settings.h"
BASELINE  = Path(__file__).resolve().parent / "project_settings_baseline.json"

# The files that AUTHOR and STORE the settings.  A read here is not a
# consumer: it is the panel drawing the control and the serialiser writing the
# key, which is exactly the state every one of the six defects was in.
AUTHORING = (
    "jce_project_settings.h",
    "jce_project_settings.cpp",
    "jce_panel_project_settings.cpp",
)

STRUCTS = {
    "JceProjectGraphics":     "graphics",
    "JceProjectQualityLevel": "quality",
}

# `int   foo;` / `bool bar;` / `char baz[64];` / `float qux;`
FIELD = re.compile(r"^\s*(?:const\s+)?"
                   r"(?:int|bool|float|double|char|unsigned|uint\d+_t|int\d+_t)\s+"
                   r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;", re.M)

TOPLEVEL = re.compile(r"^(?=[A-Za-z_])", re.M)


def strip_comments(text: str) -> str:
    """Comments are prose, not readers.  A field named in a comment that
    explains why nothing reads it would otherwise satisfy this gate."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            q = c
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def struct_fields(src: str, name: str) -> list:
    """The scalar fields of `typedef struct { ... } name;`."""
    # `[^{}]*`, not `.*?` with DOTALL: the non-greedy form still starts at the
    # FIRST `typedef struct {` in the file and swallows every struct between it
    # and the named one, so the first version of this gate reported audio,
    # input and player fields as unread quality settings.  These structs have
    # no nested braces, so refusing them is exact.
    m = re.search(r"typedef\s+struct\s*\{([^{}]*)\}\s*" + re.escape(name) + r"\s*;",
                  src, re.S)
    if not m:
        return []
    return FIELD.findall(m.group(1))


def scopes(text: str) -> list:
    cuts = [m.start() for m in TOPLEVEL.finditer(text)]
    if not cuts:
        return [text]
    cuts.append(len(text))
    return [text[cuts[i]:cuts[i + 1]] for i in range(len(cuts) - 1)]


def consumer_texts() -> dict:
    out = {}
    for root in ("editor/src", "engine/src", "tools", "scripts"):
        base = REPO_ROOT / root
        if not base.is_dir():
            continue
        for p in base.rglob("*"):
            if p.suffix not in (".c", ".cpp", ".h", ".hpp", ".py"):
                continue
            if p.name in AUTHORING:
                continue
            # A GATE IS NOT A CONSUMER, and this line is not hypothetical: the
            # first run of this checker reported color_space as CONSUMED
            # because its own self-test fixture below contains the string
            # `ps->graphics.color_space`.  Committing a gate would then put
            # its own text into the set it searches and permanently exempt
            # the very field it was written for -- which is precisely how a
            # previous gate in this tree was disarmed by being shipped.
            if "tools/lint" in p.as_posix() or "tools/audit" in p.as_posix():
                continue
            try:
                out[p.relative_to(REPO_ROOT).as_posix()] = strip_comments(
                    p.read_text(encoding="utf-8", errors="replace"))
            except OSError:
                pass
    return out


def readers(fields: dict, texts: dict) -> dict:
    """{(struct, field): [files]} for every qualified read found."""
    hit = {}
    ptr_decl = re.compile(r"JceProjectQualityLevel\s*\*\s*(?:const\s+)?([A-Za-z_]\w*)")
    for field in fields.get("JceProjectGraphics", []):
        pat = re.compile(r"graphics\s*(?:\.|->)\s*" + re.escape(field) + r"\b")
        for rel, text in texts.items():
            if pat.search(text):
                hit.setdefault(("JceProjectGraphics", field), []).append(rel)
    for field in fields.get("JceProjectQualityLevel", []):
        idx = re.compile(r"levels\s*\[[^\]]*\]\s*(?:\.|->)\s*"
                         + re.escape(field) + r"\b")
        arrow = re.compile(r"->\s*" + re.escape(field) + r"\b")
        for rel, text in texts.items():
            if idx.search(text):
                hit.setdefault(("JceProjectQualityLevel", field), []).append(rel)
                continue
            # A local pointer to the level, matched inside ONE function only:
            # `lvl` in two functions pointing at two different structs is the
            # collision the component-field gate was fixed for.
            for chunk in scopes(text):
                names = set(ptr_decl.findall(chunk))
                if not names:
                    continue
                for m in arrow.finditer(chunk):
                    pre = chunk[max(0, m.start() - 64):m.start()]
                    ident = re.search(r"([A-Za-z_]\w*)\s*$", pre)
                    if ident and ident.group(1) in names:
                        hit.setdefault(("JceProjectQualityLevel", field),
                                       []).append(rel)
                        break
                if ("JceProjectQualityLevel", field) in hit:
                    break
    return hit


def self_test() -> None:
    """The two mistakes this gate could plausibly make, on every run."""
    texts = {
        "good.cpp": "void f(void){ use(ps->graphics.color_space); }\n",
        # A BARE field name must not count: `hdr` appears in a hundred places
        # that have nothing to do with Project Settings.
        "bare.cpp": "void g(void){ int hdr = 1; use(hdr); }\n",
        # Two functions, two different `lvl` types: the same collision the
        # component-field gate shipped with for months.
        "coll.cpp": ("void a(JceProjectQualityLevel *lvl){ use(lvl->lod_bias); }\n"
                     "void b(SomethingElse *lvl){ use(lvl->target_framerate); }\n"),
    }
    fields = {"JceProjectGraphics": ["color_space", "hdr"],
              "JceProjectQualityLevel": ["lod_bias", "target_framerate"]}
    got = readers(fields, texts)
    assert ("JceProjectGraphics", "color_space") in got, got

    # AND THE GATE MUST NOT BE ITS OWN CONSUMER.  consumer_texts() excludes
    # tools/lint and tools/audit for this reason; assert the exclusion here
    # so removing it fails loudly instead of quietly exempting whatever field
    # this file happens to name.
    scanned = consumer_texts()
    assert not any(k.startswith("tools/lint/") for k in scanned), (
        "a lint script is in the consumer set: this file's own fixtures would "
        "count as readers")
    assert ("JceProjectGraphics", "hdr") not in got, (
        "a bare `hdr` was counted as a Project Settings read: " + repr(got))
    assert ("JceProjectQualityLevel", "lod_bias") in got, got
    assert ("JceProjectQualityLevel", "target_framerate") not in got, (
        "a same-named pointer to a DIFFERENT struct was credited: " + repr(got))


def load_baseline() -> dict:
    if not BASELINE.is_file():
        return {"unread": [], "reasons": {}}
    return json.loads(BASELINE.read_text(encoding="utf-8"))


def main() -> int:
    self_test()
    if not HEADER.is_file():
        print("check_project_settings_consumed: FAIL - missing %s"
              % HEADER.relative_to(REPO_ROOT).as_posix(), file=sys.stderr)
        return 1

    src = strip_comments(HEADER.read_text(encoding="utf-8", errors="replace"))
    fields = {s: struct_fields(src, s) for s in STRUCTS}
    total = sum(len(v) for v in fields.values())
    if total < 15:
        # The header was refactored out from under this gate.  Reporting "0
        # unread" would be the same silent pass a broken parser always gives.
        print("check_project_settings_consumed: FAIL - parsed only %d field(s) "
              "from %s; the gate cannot be right about a header it did not read"
              % (total, HEADER.relative_to(REPO_ROOT).as_posix()),
              file=sys.stderr)
        return 1

    texts = consumer_texts()
    hit   = readers(fields, texts)
    unread = sorted("%s.%s" % (s, f)
                    for s, fs in fields.items() for f in fs
                    if (s, f) not in hit)

    base    = load_baseline()
    known   = set(base.get("unread", []))
    reasons = base.get("reasons", {})

    if "--update-baseline" in sys.argv:
        BASELINE.write_text(json.dumps(
            {"_note": ("Project Settings fields the panel authors and the "
                       "serialiser stores, and NOTHING else reads.  Every "
                       "entry must carry a reason saying what closing it "
                       "would take -- an unexplained entry is how a field "
                       "that could be wired in an afternoon sits beside one "
                       "that needs a subsystem, with nothing to tell them "
                       "apart."),
             "unread": unread,
             "reasons": {k: reasons.get(k, "") for k in unread}},
            ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8", newline="\n")
        print("baseline updated: %d unread" % len(unread))
        return 0

    problems = []
    for k in unread:
        if k not in known:
            problems.append("%s: authored in Project Settings, serialised, and "
                            "read by NOTHING outside the panel and the "
                            "serialiser.  A designer sets it, the viewport may "
                            "honour it, and the built game does not -- with no "
                            "error, because an ignored setting behaves exactly "
                            "like one holding its default." % k)
    for k in sorted(known - set(unread)):
        problems.append("%s: baselined as unread but it HAS a reader now. "
                        "Remove it from the baseline -- a stale entry hides "
                        "the next regression on the same field." % k)
    for k in unread:
        if k in known and not (reasons.get(k) or "").strip():
            problems.append("%s: on the baseline with NO reason.  Say what "
                            "closing it would take." % k)

    if problems:
        print("check_project_settings_consumed: FAIL - %d problem(s):"
              % len(problems), file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        print("\nWire a reader, or record it with --update-baseline and write "
              "the reason into the baseline.", file=sys.stderr)
        return 1

    print("check_project_settings_consumed: OK (%d field(s) across %d struct(s); "
          "%d with no reader outside the panel, all baselined with reasons)"
          % (total, len(fields), len(unread)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
