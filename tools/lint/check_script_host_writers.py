#!/usr/bin/env python3
"""
check_script_host_writers.py — every JceScriptHost slot must have a writer.

WHY THIS EXISTS.  JceScriptHost is a table of function pointers.  The engine
copies it whole and, for every member, jce_script.c checks the pointer before
calling and falls back to a plausible default when it is NULL: false, 0, nil,
or nothing at all.  That fallback is correct and necessary -- it is what lets an
older, shorter host stay safe -- and it is also what makes an UNFILLED slot
invisible.  A binding whose slot nobody assigns is registered, callable, and
permanently returns the default, in every language, forever.

The runtime is the ONLY writer.  There is no application hook: `script_host`
is assigned in exactly one place (engine/src/application/jce_rt_script.c) and
handed to every language VM by rt_script_vm_get.  So for a shipped game, "not
assigned in that file" means "dead for every script".

MEASURED WHEN THIS GATE LANDED (2026-09-01): 90 function-pointer members, 89
assigned, 1 not.  The one is is_key_down -- the scripting layer's only raw
keyboard primitive.  It has been declared, registered as Lua `jce.is_key_down`,
and read by jce_script.c since it was written, and nothing has ever assigned
it, so it has returned false in every shipped build.  A sample's own Lua source
carries a comment recording the defect, and a previous audit filed it with a
zero-consumer proof.  Folklore in an audit JSON is not a gate; this is.

WHAT THIS CHECKS.  Every `ret (*name)(...)` member of JceScriptHost is assigned
a REAL writer in jce_rt_script.c -- `host.<name> = <something that is not a
null literal>`.  Exemptions live in script_host_writer_exempt.txt, one name per
line with a reason on the same line, and the reason must say what would be
needed -- an exemption that only says "not implemented" records nothing a
reader could act on.

THE NULL RULE, AND WHY IT IS HERE (2026-09-05).  This gate shipped matching
only the LEFT of the `=`.  So `host.audio_play = NULL;` satisfied it, and it
printed "95 assigned" over precisely the dead slot its own failure message
describes as "The slot stays NULL".  Found by accident: a feature commit ran
that line as a negative control on ITS OWN test, the test went red as intended,
and this gate stayed green beside it.  A gate that a one-word edit defeats is
not a weaker gate, it is a gate for a different defect than the one it names.
Measured both ways on the same tree: old gate exit 0 ("95 assigned"), new gate
exit 1.  self_test() below keeps the rule honest, and runs on every invocation
rather than only under --self-test, because a control that has to be asked for
is a control that gets asked for once.

WHAT THIS DOES NOT CHECK.  That the writer is CORRECT, or that it reads the
live value rather than an authored seed.  A cast, or a `cond ? f : NULL`, is a
real writer here and its correctness is somebody else's problem.  It checks
that a writer exists, which is the difference between a feature that might be
wrong and a feature that cannot be right.

SIBLING VTABLES: SWEPT, NONE FOUND (2026-09-01).  This gate covers
JceScriptHost only, and the obvious question is whether the other
function-pointer tables in engine/include have the same hole.  They do not.
All eight were enumerated and each non-JceScriptHost one read by hand:

  JcePlatformBackend (18), JceInputBackend (6)   positional initialisers --
      k_local_backend in jce_platform_services.c and s_sdl_backend in
      jce_input_sdl.c fill every slot in declaration order.
  JceAppDesc.on_resize, jce_subsystem_desc_t.shutdown/.quiesce
      deliberately optional, NULL-checked at every call site
      (jce_engine.c:1300, jce_subsystem.c:124/128/156) and set to an
      explicit `NULL, /* on_resize */` in jce_game_module.c:66.
  jce_allocator_t (3), JceSceneRendererCallbacks   fully assigned.

Worth recording because the first pass at this used a `.member =` regex and
reported "JceInputBackend 0/6", which is what a POSITIONAL initialiser looks
like to that pattern.  Do not re-run that scan and believe it; the six were
assigned all along.

Usage:  python tools/lint/check_script_host_writers.py
Exit 0 clean, 1 on an unassigned member with no exemption.
"""

import re
import sys
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]

HEADER = (REPO_ROOT / "engine" / "include" / "jce" / "middleware" / "script"
          / "jce_script.h")
WRITER = (REPO_ROOT / "engine" / "src" / "application" / "jce_rt_script.c")
EXEMPT = LINT_DIR / "script_host_writer_exempt.txt"

MEMBER = re.compile(r"\(\s*\*\s*(\w+)\s*\)\s*\(")
# The RHS is captured, not just the name: `host.x = NULL;` is an assignment to
# this pattern and leaves exactly the dead slot the gate exists to prevent.
# `[^;]*` cannot cross a statement boundary, so it bounds itself and still
# spans a wrapped line.
ASSIGN = re.compile(r"\bhost\s*\.\s*(\w+)\s*=(?!=)([^;]*);")

# A bare null literal, in the shapes C actually writes one.  Anything else --
# an identifier, a cast of one, a `cond ? f : NULL` -- is a real writer whose
# CORRECTNESS is out of scope, as the docstring says.
NULL_RHS = re.compile(r"^\(?\s*(?:\(\s*void\s*\*\s*\))?\s*"
                      r"(?:NULL|nullptr|0)\s*\)?$")


def load_exempt():
    """{name: reason}.  A line with no reason is not an exemption."""
    out, bad = {}, []
    if not EXEMPT.is_file():
        return out, bad
    for n, line in enumerate(EXEMPT.read_text(encoding="utf-8").splitlines(), 1):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        name, _, reason = s.partition(" ")
        if not reason.strip():
            bad.append((n, name))
            continue
        out[name] = reason.strip()
    return out, bad


def main() -> int:
    for f in (HEADER, WRITER):
        if not f.is_file():
            print("check_script_host_writers: FAIL - %s not found"
                  % f.relative_to(REPO_ROOT).as_posix(), file=sys.stderr)
            return 1

    h = HEADER.read_text(encoding="utf-8", errors="replace")
    try:
        end = h.index("} JceScriptHost;")
        start = h.rindex("typedef struct", 0, end)
    except ValueError:
        print("check_script_host_writers: FAIL - JceScriptHost not found in %s"
              % HEADER.relative_to(REPO_ROOT).as_posix(), file=sys.stderr)
        return 1

    members = MEMBER.findall(h[start:end])
    if not members:
        print("check_script_host_writers: FAIL - parsed zero members; the "
              "struct's shape changed and this gate is now blind",
              file=sys.stderr)
        return 1

    assigned, nulled = set(), {}
    for name, rhs in ASSIGN.findall(
            WRITER.read_text(encoding="utf-8", errors="replace")):
        if NULL_RHS.match(" ".join(rhs.split())):
            nulled[name] = " ".join(rhs.split())
        else:
            assigned.add(name)
    exempt, malformed = load_exempt()

    problems = []
    for n, name in malformed:
        problems.append("%s:%d  exemption for '%s' carries no reason -- an "
                        "exemption without one records nothing"
                        % (EXEMPT.name, n, name))

    missing = [m for m in members if m not in assigned]
    for m in missing:
        if m in exempt:
            continue
        if m in nulled:
            problems.append(
                "JceScriptHost.%s is assigned `%s` in %s.  A null literal is "
                "not a writer: the slot is exactly as dead as an absent line, "
                "and the binding stays registered, callable and permanently "
                "inert in all seven languages.  This gate used to match only "
                "the left of the `=`, so `host.%s = NULL;` passed it."
                % (m, nulled[m], WRITER.relative_to(REPO_ROOT).as_posix(), m))
            continue
        problems.append(
            "JceScriptHost.%s has no `host.%s =` in %s.  The slot stays NULL, "
            "jce_script.c falls back to its default, and the binding is "
            "registered, callable and permanently inert in all seven "
            "languages.  Assign it, or exempt it in %s WITH what would be "
            "needed."
            % (m, m, WRITER.relative_to(REPO_ROOT).as_posix(), EXEMPT.name))

    # An exemption for a member that IS assigned, or that no longer exists, is
    # stale: it would silently keep covering a future regression.  A member
    # assigned a null literal is NOT assigned for this purpose either -- an
    # exemption is still the honest record of it.
    for name in sorted(exempt):
        if name not in members:
            problems.append("%s exempts '%s', which is not a JceScriptHost "
                            "member any more" % (EXEMPT.name, name))
        elif name in assigned:
            problems.append("%s exempts '%s', but it IS assigned now -- drop "
                            "the line so the gate covers it"
                            % (EXEMPT.name, name))

    if problems:
        for p in problems:
            print("  " + p, file=sys.stderr)
        print("check_script_host_writers: FAIL - %d problem(s)." % len(problems),
              file=sys.stderr)
        return 1

    print("check_script_host_writers: OK (%d host member(s); %d assigned to a "
          "real writer, %d exempted with a reason)"
          % (len(members), len(members) - len(missing), len(missing)))
    return 0


def self_test() -> int:
    """Prove the RHS rule can say no, and does not say no to a real writer.

    This gate shipped able to be defeated by one word: `host.audio_play = NULL;`
    passed it, while its own failure message says "The slot stays NULL".  The
    reason it went unnoticed is that only the GREEN case was ever run -- so the
    fixtures below are the negative controls, and they are checked here rather
    than by hand, because a hand-run control is a control that runs once.
    """
    real = [
        "host.set_position = rt_script_set_position;",
        "host.get_x        = (JceScriptGetFn)rt_script_get_x;",
        "host.maybe        = have_it ? rt_script_maybe : NULL;",
        "host.wrapped      =\n\t\trt_script_wrapped;",
    ]
    dead = [
        "host.audio_play = NULL;",
        "host.audio_stop=nullptr;",
        "host.is_key_down = 0;",
        "host.cast_null   = (void *)0;",
    ]
    bad = []
    for src in real:
        for name, rhs in ASSIGN.findall(src):
            if NULL_RHS.match(" ".join(rhs.split())):
                bad.append("false positive: %r read as a null literal" % src)
    for src in dead:
        hits = ASSIGN.findall(src)
        if not hits:
            bad.append("not matched at all: %r" % src)
            continue
        for name, rhs in hits:
            if not NULL_RHS.match(" ".join(rhs.split())):
                bad.append("false negative: %r read as a real writer" % src)
    for b in bad:
        print("  self-test: " + b, file=sys.stderr)
    if bad:
        print("check_script_host_writers: SELF-TEST FAIL - %d" % len(bad),
              file=sys.stderr)
        return 1
    print("check_script_host_writers: self-test OK (%d real writers accepted, "
          "%d null literals rejected)" % (len(real), len(dead)))
    return 0


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    sys.exit(self_test() or main())
