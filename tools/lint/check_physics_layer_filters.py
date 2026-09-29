#!/usr/bin/env python3
"""
check_physics_layer_filters.py — a broadphase filter in the physics backend
must come from the layer matrix, never from Bullet's own constants.

WHY THIS EXISTS.  JCE ships a 32-slot Physics Layer Collision Matrix
(jce_physics_layers.h): a body on layer L is added to the world with
group = 1<<L and mask = the matrix row for L.  Bullet ALSO ships a small set
of built-in filter constants, and their values are the low layer bits:

    btBroadphaseProxy::DefaultFilter   =  1   -> JCE layer 0
    btBroadphaseProxy::StaticFilter    =  2   -> JCE layer 1
    btBroadphaseProxy::KinematicFilter =  4   -> JCE layer 2
    btBroadphaseProxy::DebrisFilter    =  8   -> JCE layer 3
    btBroadphaseProxy::SensorTrigger   = 16   -> JCE layer 4
    btBroadphaseProxy::CharacterFilter = 32   -> JCE layer 5

So the two vocabularies ALIAS.  Writing a Bullet constant does not opt out of
the matrix; it silently opts INTO a specific layer, and there is no diagnostic
because the value is a perfectly ordinary bitmask either way.

MEASURED CONSEQUENCE (2026-08-31, the defect this gate was written for).  The
character capsule was added as

    world->addRigidBody(body, btBroadphaseProxy::CharacterFilter,
                              btBroadphaseProxy::StaticFilter |
                              btBroadphaseProxy::DefaultFilter);

which reads to the matrix as "this capsule is on layer 5, and it collides with
layers 0 and 1 only".  Consequences, all silent:

  * The player passed through every body a designer put on layers 2..31, no
    matter what the collision matrix said.  Put the ground on a "Terrain"
    layer and the player falls out of the world.
  * The matrix row for the character's own layer was never consulted, so
    turning a pair OFF in the editor did nothing to the player.
  * The ground / step / head-clearance probes are separate rayTests carrying
    their own copy of the same constants, so even where the capsule did
    collide, it probed the world through a different filter than it collided
    through.

WHAT THIS CHECKS.  Two rules over engine/src/middleware/physics/**:

  RULE 1  No btBroadphaseProxy::*Filter / ::SensorTrigger / ::AllFilter
          anywhere.  There is no correct use: every one of them means a layer
          index, and if that is what the author wants they should say so
          through jce_physics_layers.h.  The tree had zero when this gate
          landed, so the rule is ABSOLUTE -- no baseline, no exemption file.

  RULE 2  Every addRigidBody() call ON THE MAIN WORLD passes a group and a
          mask, and neither is an integer literal.  The two-argument form
          silently takes Bullet's DefaultFilter/AllFilter, and a literal is the
          same aliasing bug written in decimal.  All six main-world call sites
          pass named variables that trace back to a layer; this rule keeps a
          seventh from not doing so.

          "Main world" is `bw->world`, the btDiscreteDynamicsWorld the layer
          matrix governs.  jce_cloth.cpp owns a SECOND world -- a
          btSoftRigidDynamicsWorld holding cloth plus the static proxies its
          own API adds (jce_softbody_add_static_box) -- which no layer indexes,
          so an unfiltered add there is correct.  That is a structural
          exclusion, not a waiver: the OK line prints how many calls it covered
          so the number cannot grow unnoticed.

WHAT THIS DOES NOT CHECK.  That the variable handed to addRigidBody actually
came from the matrix -- that is a dataflow question a text scan cannot answer.
The gate narrows the opening to "a named value the reviewer can follow"; it
does not close it.

Usage:  python tools/lint/check_physics_layer_filters.py
Exit 0 clean, 1 on any finding.
"""

import re
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]
PHYS_DIR = REPO_ROOT / "engine" / "src" / "middleware" / "physics"

BULLET_FILTER = re.compile(
    r"btBroadphaseProxy::(?:DefaultFilter|StaticFilter|KinematicFilter"
    r"|DebrisFilter|SensorTrigger|CharacterFilter|AllFilter)")

ADD_BODY = re.compile(r"\baddRigidBody\s*\(")
# Receiver of the call: `bw->world->addRigidBody(...)` is the layered world.
MAIN_WORLD = re.compile(r"\bbw\s*->\s*world\s*->\s*$")
# A group/mask argument that is a bare integer literal (decimal or hex), with
# or without a cast around it.  `static_cast<int>(col_group)` is fine.
LITERAL_ARG = re.compile(r"^\s*(?:\(\s*int\s*\)\s*)?(?:0[xX][0-9a-fA-F]+|\d+)[uU]?\s*$")


def strip_comments(text: str) -> str:
    """Blank out // and /* */ so a constant NAMED in prose is not a finding.

    This gate's own rationale quotes the constants it bans; so does the header
    comment on JceCharacterDesc::layer.  A gate that fires on the sentence
    explaining it teaches people to delete the explanation.
    """
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
        elif text[i] == '"':
            j, esc = i + 1, False
            while j < n and (esc or text[j] != '"'):
                esc = (text[j] == "\\") and not esc
                j += 1
            j = min(j + 1, n)
            out.append(text[i:j])
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def split_args(call: str) -> list:
    """Top-level comma split of an argument list (nesting- and string-aware)."""
    args, depth, cur, i, n = [], 0, [], 0, len(call)
    while i < n:
        ch = call[i]
        if ch in "([{<":
            # '<' only nests when it opens a template argument list; a bare
            # less-than would otherwise swallow the rest of the call.
            if ch != "<" or re.search(r"\b(?:static_cast|reinterpret_cast|"
                                      r"const_cast|dynamic_cast)\s*$",
                                      "".join(cur)):
                depth += 1
        elif ch in ")]}>":
            if depth > 0:
                depth -= 1
        elif ch == "," and depth == 0:
            args.append("".join(cur))
            cur = []
            i += 1
            continue
        cur.append(ch)
        i += 1
    if cur:
        args.append("".join(cur))
    return args


def call_body(text: str, open_paren: int) -> str:
    """Text between the parens of a call whose '(' is at `open_paren`."""
    depth, i, n = 0, open_paren, len(text)
    while i < n:
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1:i]
        i += 1
    return ""


def main() -> int:
    if not PHYS_DIR.is_dir():
        print("check_physics_layer_filters: FAIL - %s not found" % PHYS_DIR,
              file=sys.stderr)
        return 1

    sources = sorted(list(PHYS_DIR.glob("*.c")) + list(PHYS_DIR.glob("*.cpp"))
                     + list(PHYS_DIR.glob("*.h")))
    if not sources:
        print("check_physics_layer_filters: FAIL - no physics sources found",
              file=sys.stderr)
        return 1

    findings = []
    calls_checked = 0
    other_worlds = 0

    for src in sources:
        raw = src.read_text(encoding="utf-8", errors="replace")
        code = strip_comments(raw)
        rel = src.relative_to(REPO_ROOT).as_posix()

        for m in BULLET_FILTER.finditer(code):
            line = code.count("\n", 0, m.start()) + 1
            findings.append((rel, line, "rule 1",
                             "%s is a layer index in disguise (see the header "
                             "of this checker); take the filter pair from "
                             "jce_physics_layers.h instead" % m.group(0)))

        for m in ADD_BODY.finditer(code):
            line = code.count("\n", 0, m.start()) + 1
            if not MAIN_WORLD.search(code[max(0, m.start() - 64):m.start()]):
                other_worlds += 1
                continue
            body = call_body(code, m.end() - 1)
            args = split_args(body)
            calls_checked += 1
            if len(args) < 3:
                findings.append((rel, line, "rule 2",
                                 "addRigidBody() with %d argument(s): Bullet "
                                 "then applies DefaultFilter/AllFilter, i.e. "
                                 "layer 0 colliding with everything, "
                                 "regardless of the matrix" % len(args)))
                continue
            for which, a in (("group", args[1]), ("mask", args[2])):
                if LITERAL_ARG.match(a):
                    findings.append((rel, line, "rule 2",
                                     "addRigidBody() %s is the literal %s -- "
                                     "the same aliasing bug written in "
                                     "decimal; pass a value resolved from the "
                                     "layer matrix" % (which, a.strip())))

    if findings:
        for rel, line, rule, why in findings:
            print("  %s:%d  [%s] %s" % (rel, line, rule, why), file=sys.stderr)
        print("check_physics_layer_filters: FAIL - %d broadphase filter(s) in "
              "engine/src/middleware/physics do not come from the layer "
              "matrix.  Bullet's constants ARE layer bits 0-5, so using them "
              "silently pins an object to a layer nobody authored."
              % len(findings), file=sys.stderr)
        return 1

    print("check_physics_layer_filters: OK (%d source(s); %d addRigidBody() "
          "call(s) on the layered world checked, %d on the soft-body world "
          "excluded by construction; no Bullet built-in filter constant and no "
          "literal filter argument)"
          % (len(sources), calls_checked, other_worlds))
    return 0


if __name__ == "__main__":
    sys.exit(main())
