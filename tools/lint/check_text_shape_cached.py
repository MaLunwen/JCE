#!/usr/bin/env python3
"""
check_text_shape_cached.py -- text shaping goes through the cache, or it goes
back to being done twice per string per frame.

THE STATE THIS ENDS.  jce_text_draw_scaled_view and jce_text_measure each did
hb_buffer_create -> add_utf8 -> guess_segment_properties -> hb_shape ->
hb_buffer_destroy, every call.  Shaping is a pure function of (font, string) --
`scale` is applied to the returned fixed-point values, not inside HarfBuzz --
so a label whose text has not changed had exactly one right answer and computed
it again every frame; and because a UI measures before it draws, it computed it
TWICE per frame per visible string, with two HarfBuzz buffer allocations.

WHY A GATE AND NOT ONLY A TEST.  tests/renderer/test_jce_text_shape_cache.c
covers the cache itself, including the one assertion that matters most (the
second lookup must not run HarfBuzz).  What it cannot cover is whether
jce_text.c still ASKS: someone adding a third text entry point, or "simplifying"
one of the two back to a local hb_buffer, gets identical pixels and pays the
cost again.  Nothing looks wrong -- that is the entire failure mode, and it is
the same one the cache was written to end.

Also note `tests/` is gitignored on this branch, so that suite does not travel
with a clone.  This checker does.

THREE RULES:

  1. Nothing under engine/src may call hb_shape() or hb_buffer_create() except
     the cache itself (jce_text_shape.c).  A second shaper is a second cache
     miss policy, silently.

  2. jce_text.c must CALL jce_text_shape().  Rule 1 is satisfied by deleting
     the text renderer entirely; this is the half that says the caller is still
     connected to the thing rule 1 protects.

  3. jce_text.c's draw path must read the shaped glyph's `font_slot`.  The
     shaper itemizes a mixed-script string into runs and tags each glyph with
     the font that produced it; a draw loop that ignores the tag rasterises
     every fallback glyph out of the PRIMARY's atlas -- correct spacing,
     correct line breaks, and a tofu box for every CJK character, which reads
     as a missing font rather than as a missing line of code.  This is a
     regression that produces a plausible picture, which is the whole family
     this checker guards.

NOT CHECKED, said here rather than left to be discovered: that the cache is
INVALIDATED correctly.  jce_font_close must call jce_text_shape_forget, and a
missing call is a use-after-free waiting for an allocator to reuse an address,
not a wrong picture -- no static rule this cheap can see it.  The test does
(test_forget_drops_that_owners_runs), which is why that half lives there.
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ENGINE_SRC = REPO_ROOT / "engine/src"

CACHE_IMPL = "jce_text_shape.c"
CALLER = "jce_text.c"
ENTRY = "jce_text_shape"
SLOT_FIELD = "font_slot"

failures = []


def strip_comments(text):
    """Block and line comments out.

    A rule that counted a MENTION would be satisfied by the very comment
    explaining it -- this tree has been bitten by exactly that: a gate's own
    docstring entered the identifier set it searched and permanently exempted
    the one name it was written for.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def main():
    scanned = 0
    strays = []
    caller_ok = False
    slot_ok = False

    for ext in ("*.c", "*.cpp", "*.h"):
        for p in ENGINE_SRC.rglob(ext):
            try:
                src = strip_comments(p.read_text(encoding="utf-8",
                                                 errors="replace"))
            except OSError:
                continue
            scanned += 1
            rel = p.relative_to(REPO_ROOT).as_posix()

            if p.name != CACHE_IMPL:
                for fn in ("hb_shape", "hb_buffer_create"):
                    for m in re.finditer(r"\b%s\s*\(" % fn, src):
                        strays.append("%s:%d (%s)"
                                      % (rel, src[:m.start()].count("\n") + 1,
                                         fn))

            if p.name == CALLER and re.search(r"\b%s\s*\(" % ENTRY, src):
                caller_ok = True
            if p.name == CALLER and re.search(r"\.\s*%s\b" % SLOT_FIELD, src):
                slot_ok = True

    if scanned < 50:
        print("check_text_shape_cached: FAIL - only %d engine source(s) "
              "scanned; the scan is broken, not the tree." % scanned,
              file=sys.stderr)
        return 1

    for site in strays:
        failures.append(
            "%s shapes text outside %s. Every entry point that shapes has to "
            "go through the cache or it re-shapes an unchanged string every "
            "frame -- which draws the correct picture, so nothing else in this "
            "tree would report it." % (site, CACHE_IMPL))

    if not caller_ok:
        failures.append(
            "%s does not call %s(). Rule 1 is satisfied by a renderer that "
            "shapes nothing at all; this is the half that says the caller is "
            "still connected." % (CALLER, ENTRY))

    if not slot_ok:
        failures.append(
            "%s never reads a shaped glyph's .%s. The shaper itemizes a "
            "mixed-script string and tags each glyph with the font that "
            "produced it; a draw that ignores the tag pulls every fallback "
            "glyph out of the PRIMARY's atlas -- right spacing, right line "
            "breaks, and a tofu box for every CJK character."
            % (CALLER, SLOT_FIELD))

    if failures:
        print("check_text_shape_cached: FAIL - %d problem(s):" % len(failures),
              file=sys.stderr)
        for f in failures:
            print("  " + f, file=sys.stderr)
        return 1

    print("check_text_shape_cached: OK (%d engine source(s); shaping confined "
          "to %s, %s still calls %s and still reads .%s)"
          % (scanned, CACHE_IMPL, CALLER, ENTRY, SLOT_FIELD))
    return 0


if __name__ == "__main__":
    sys.exit(main())
