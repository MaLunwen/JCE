#!/usr/bin/env python3
"""
check_ssgi_denoise_guided.py — the SSGI composite denoises, and it must stay
GUIDED: the shader's depth/normal samplers must have something bound to them.

THE FAILURE THIS GUARDS, and it is a shape this tree keeps producing: a shader
declares an input, nothing binds it, and the result is not an error.  An
unbound sampler reads undefined data — on D3D11 usually zeros — so the filter
would still run, still change every pixel, still look plausible in a screenshot,
and would have become a plain box blur that drags a red wall's bounce out onto
the floor in front of it.  That is the exact picture the effect exists to
produce, which is where the mistake would be least visible.

It is also the shape of a REGRESSION rather than of a first mistake.  The
denoise lives inside fs_ssgi_composite.sc instead of in a pass of its own
because SSGI owns exactly two base-relative view ids (26 march, 27 composite;
jce_views.h) and 28/29 are taken.  So the composite is the only pass that can
carry it, and "simplify the composite back to one fetch" is a natural-looking
edit that silently removes the denoiser — the picture stays lit, only grainier,
which no gate in this tree measures.

WHY A GATE AND NOT A TEST.  The claim "it denoises" is a claim about pixels and
is measured with envshot against a fixture (a lit red wall over a grey floor):
the isolated bounce's high-frequency energy must DROP while its level SURVIVES,
both against a floor taken from two captures of one build.  Nothing in the unit
suite drives a JceSceneRenderer — `grep -rl jce_scene_renderer_create tests/`
is empty — so that measurement cannot live there.  What CAN be checked here is
the half that would make the measurement meaningless: that the shader still
asks for the guides and the C side still binds them.

THREE RULES, each removing one way the filter degenerates into a blur:

  1. fs_ssgi_composite.sc must sample s_depth and s_normal at TWO DIFFERENT
     coordinates -- the centre pixel and the tap.  "Samples it somewhere" was
     the first version of this rule and it did not fire: deleting the per-tap
     fetch, which is the whole guide, leaves the centre fetch behind and the
     rule passed on a filter that had become a plain blur.  The mutation was
     run; the rule was rewritten because of what it did, not what it said.

  2. jce_ssgi_composite() must bind sampler stages 1 and 2.  Stage numbers,
     not names: the shader's SAMPLER2D(...) stage indices are what the binds
     have to agree with, and they are the part a rename cannot fix.

  3. The composite must not weight by the RT's ALPHA.  That channel carries hit
     coverage, which looks like a confidence and is not one — bounce.rgb is
     already normalised by rays CAST, so weighting by rays that HIT is a
     positive bias.  Measured: a version that did this RAISED the bounce's
     high-frequency energy by 37.6% while being called a denoiser.

NOT CHECKED, and said here rather than left to be discovered: that the weights
are the RIGHT ones.  A kernel with a broken depth tolerance still samples
s_depth and still passes every rule above.  Only the envshot measurement can
tell that, and its numbers belong in the parity ledger, not in a lint.
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SHADER = REPO_ROOT / "engine/shaders/ssgi/fs_ssgi_composite.sc"
SOURCE = REPO_ROOT / "engine/src/renderer/jce_ssgi.c"
COMPOSITE_FN = "jce_ssgi_composite"

failures = []


def strip_comments(text):
    """Block and line comments out.

    A rule that counted a MENTION would be satisfied by the very comment that
    explains it -- this tree has been bitten by exactly that: a gate's own
    docstring entered the identifier set it searched and permanently exempted
    the one name it was written for.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def composite_body(src):
    """The text of jce_ssgi_composite's body, brace-matched.

    Scoped on purpose: jce_ssgi_render binds the same three stages, so a rule
    written over the whole file would be satisfied by the MARCH and would pass
    with the composite's binds deleted -- a gate that cannot fail.
    """
    m = re.search(r"\b%s\s*\([^)]*\)\s*\{" % re.escape(COMPOSITE_FN), src)
    if not m:
        return None
    i = m.end() - 1
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return None


def main():
    if not SHADER.is_file():
        print("check_ssgi_denoise_guided: FAIL - %s is missing; the scan is "
              "broken, not the tree." % SHADER.name, file=sys.stderr)
        return 1
    if not SOURCE.is_file():
        print("check_ssgi_denoise_guided: FAIL - %s is missing; the scan is "
              "broken, not the tree." % SOURCE.name, file=sys.stderr)
        return 1

    sh = strip_comments(SHADER.read_text(encoding="utf-8", errors="replace"))
    for sampler in ("s_depth", "s_normal"):
        # The COORDINATE of every fetch, normalised for whitespace.  Two
        # distinct ones means the centre and the tap; one means the guide is
        # comparing a pixel with itself, which weighs every tap equally --
        # a blur wearing a bilateral's shape.
        coords = {re.sub(r"\s+", "", m.group(1))
                  for m in re.finditer(
                      r"texture2D\s*\(\s*%s\s*,([^)]*)\)" % sampler, sh)}
        if len(coords) < 2:
            failures.append(
                "%s samples %s at %d distinct coordinate(s); the guide needs "
                "the CENTRE and the TAP. With one, every tap weighs the same "
                "and the gather is a plain blur -- which drags a wall's bounce "
                "onto the floor in front of it, looks lit, and is caught by no "
                "screenshot guard. Found: %s"
                % (SHADER.name, sampler, len(coords),
                   ", ".join(sorted(coords)) or "none"))

    src = strip_comments(SOURCE.read_text(encoding="utf-8", errors="replace"))
    body = composite_body(src)
    if body is None:
        failures.append(
            "could not find the body of %s() in %s; the scan is broken, not "
            "the tree." % (COMPOSITE_FN, SOURCE.name))
    else:
        for stage in (1, 2):
            if not re.search(r"bgfx_set_texture\s*\(\s*%d\s*," % stage, body):
                failures.append(
                    "%s() does not bind sampler stage %d. The shader declares "
                    "one there; an unbound sampler reads undefined data and "
                    "reports nothing."
                    % (COMPOSITE_FN, stage))

    # Rule 3: no alpha in the weight.  Any use of the sampled RT's .a / .w in
    # the composite is the bias described above.
    for m in re.finditer(r"texture2D\s*\(\s*s_color\s*,[^)]*\)\s*\.\s*([awrgbxyz]+)",
                         sh):
        swz = m.group(1)
        if "a" in swz or "w" in swz:
            failures.append(
                "%s reads the bounce RT's ALPHA (.%s). That channel is hit "
                "COVERAGE, not confidence: bounce.rgb is already normalised by "
                "rays CAST, so weighting by rays that HIT biases the estimate "
                "upward -- measured, it raised the bounce's high-frequency "
                "energy by 37.6%% in a filter that was called a denoiser."
                % (SHADER.name, swz))

    if failures:
        print("check_ssgi_denoise_guided: FAIL - %d problem(s):"
              % len(failures), file=sys.stderr)
        for f in failures:
            print("  " + f, file=sys.stderr)
        return 1

    print("check_ssgi_denoise_guided: OK (%s samples s_depth + s_normal and "
          "uses no coverage weight; %s() binds stages 1 and 2)"
          % (SHADER.name, COMPOSITE_FN))
    return 0


if __name__ == "__main__":
    sys.exit(main())
