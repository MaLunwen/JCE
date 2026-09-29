#!/usr/bin/env python3
"""
check_skills.py - keep the JCE agent skills honest.

A skill is documentation an agent will act on without checking.  That makes a
stale path or an invented flag worse than no skill at all: the agent runs the
command, gets an error it cannot attribute, and concludes the CODE is broken.
This session produced three real instances before the skills were even written
-- `python tools/shader_lint.py` with no arguments (exit 2, usage error), a
ctest baseline read off a stale build directory, and a claim that
`JCE_WINDOW_HIDDEN=1` bypasses the single-instance lock on a binary built
before that code existed.

So every factual reference a skill makes is checked against the tree:

  * frontmatter        name/description present, well formed, within budget
  * repo paths         `engine/src/...`, `tools/lint/...` must exist
  * absence claims     a path said to be GONE must really be gone
  * script commands    `python scripts/x.py` / `python tools/x.py` must exist
  * env vars           every `JCE_*` must appear in first-party source
  * skill references   every `jce-*` name must be a sibling skill

The absence check exists because a skill shipped saying "tools/audit/ does not
exist" when it does, with 15 files -- one wrong out of five such claims.  The
two path checks are mutually exclusive by construction: a path named inside a
"this is gone" sentence is the opposite claim, and letting both fire meant the
positive check reported the very absence the sentence was asserting.

WHAT THIS CANNOT DO.  It checks that the nouns exist, not that the sentences
are true.  A skill can pass every check here and still give bad advice; that is
what the RED/GREEN subagent scenarios are for.  Stating the limit matters: a
green mechanical run is not a verdict on the content.

Usage:
    python tools/lint/check_skills.py
    python tools/lint/check_skills.py --skills-dir <path>
    python tools/lint/check_skills.py --list
Exit 0 clean / 1 findings / 2 preconditions missing (SKIPPED, never silent).
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

DEFAULT_SKILLS_DIR = Path(os.environ.get("JCE_SKILLS_DIR") or (REPO_ROOT / "skills"))
# Matches both shapes this repo has used: the split form (jce-architecture-map,
# ...) and the consolidated one (a single `jce` directory with references/).
SKILL_PREFIX = "jce"

NAME_RE = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
FRONTMATTER_RE = re.compile(r"\A---\r?\n(.*?)\r?\n---\r?\n", re.S)

# `path/like/this.ext` or `path/like/this/` inside backticks.
PATH_RE = re.compile(r"`([A-Za-z0-9_.][A-Za-z0-9_./-]*/[A-Za-z0-9_./*-]+)`")

# A path claim is only checked when it is REPO-ROOTED.  Without this the
# checker flags prose that merely contains a slash -- `os/platform`,
# `malloc/free/fopen`, `JCE_DBG_VISTA_TX/TY/TZ` -- and a checker that cries
# wolf on prose gets its real findings ignored along with the noise.  The rule
# it enforces is worth stating on its own: a path claim in a skill must be
# rooted, because a reader cannot resolve `os/platform` without guessing.
REPO_ROOTS = (
    "engine/", "editor/", "scripts/", "tools/", "tests/", "cmake/", "conan/",
    "docs/", ".docs/", ".github/", ".claude/", ".jce/", "dist/", "build/",
    "reports/", "contracts/", "space/", "science_lab/", "examples/", "caged_kingdom/",
    "elemental_serenity/", "street_demo/", "meadow_valley/", "diag/",
)

# Same idiom as tools/shader_lint.py: an explicit, visible opt-out for the case
# the rule cannot know about -- here, a path named precisely BECAUSE it is
# missing (a documented broken link).  Suppression must be on the same line, so
# it is impossible to silence a finding without seeing it.
ALLOW_MARK = "skills-lint: allow"

# A skill can be wrong in the other direction too: it can claim something is
# ABSENT when it is not.  The path checks above cannot see that -- they only
# verify that named things exist -- and one such claim did ship ("tools/audit/
# does not exist"; it does, with 15 files).  1 of 5 negative claims was wrong.
#
# Precision matters more than recall here: a checker that flags prose gets its
# real findings ignored along with the noise.  So a negation is only read as a
# claim about a path when the marker follows the path CLOSELY on the same line.
NEGATION_MARKS = ("不存在", "已删", "已被删", "does not exist", "no longer exists")
NEGATION_WINDOW = 48        # chars between the closing backtick and the marker
# python scripts/foo.py ... / python tools/bar.py ...
PY_CMD_RE = re.compile(r"python3?\s+((?:scripts|tools)/[A-Za-z0-9_./-]+\.py)")
ENV_RE = re.compile(r"\b(JCE_[A-Z0-9_]{2,})\b")
SKILL_REF_RE = re.compile(r"`(jce-[a-z0-9-]+)`")

# Env names that are ours but live only in tooling/CI, plus the ones this
# checker itself documents.  Each needs a reason, same rule as the exemption
# file in check_editor_consumption.py.
ENV_ALLOW = {
    "JCE_SKILLS_DIR": "read by this checker",
    "JCE_SDK_DIR": "consumed by tools/build/jce.py, not by engine source",
    "JCE_BUILD_VARIANT": "CMake cache variable, not a runtime getenv",
    "JCE_BUILD_TESTS": "CMake option, not a runtime getenv",
    "JCE_ENABLE_PROFILING": "CMake option",
    "JCE_ENABLE_PATENTED_CODECS": "CMake option",
    "JCE_PROFILING_ALLOW_REMOTE": "CMake option",
    "JCE_TRACY_ENABLED": "compile definition",
    "JCE_PAK_EXECUTABLE": "CMake cache variable injected by tools/build/jce.py",
    "JCE_SHADERC_EXECUTABLE": "CMake cache variable injected by tools/build/jce.py",
    "JCE_ENVSHOT_NO_BUILD": "read by tools/envshot.py",
}

# Directories whose absence means "this worktree does not carry it", not "the
# skill is wrong".  A path under these is reported as a NOTE, not a finding.
# `.jce/` joins them 2026-09-17: it is where visual_diff and perf_bench
# write their runs, so it exists only after one of those tools has been
# run and is absent on a clean checkout -- the same shape as build/ and
# reports/, and it had been FAILing this gate rather than noting it.
SOFT_DIRS = ("dist/", "build/", "reports/", ".docs/", "docs/", ".jce/")

# What counts as "first-party source" when deciding whether a JCE_* name the
# skill mentions actually exists.
#
# The build files belong here too, and did not until 2026-08-27: three real
# variables (JCE_GAME_PROJECT_DIR / JCE_GAME_TARGET / JCE_GAME_ICON) live only
# in CMakeLists.txt and in scripts/*.bat + scripts/macos/*.sh, so this gate
# reported all three as INVENTED.  A checker that cannot see where a name is
# declared does not get to call it imaginary.
SOURCE_GLOBS = ("engine/**/*.c", "engine/**/*.h", "engine/**/*.cpp",
                "engine/**/*.inc.h", "editor/**/*.cpp", "editor/**/*.h",
                "tools/**/*.py", "scripts/**/*.py", "tools/**/*.c",
                "CMakeLists.txt", "*/CMakeLists.txt", "examples/*/CMakeLists.txt", "cmake/**/*.cmake",
                "engine/cmake/**/*.cmake", "scripting/**/*.cmake",
                "scripts/**/*.bat", "scripts/**/*.sh", "scripts/**/*.cmd")

# Trees that are build output or vendored code: they can contain a copy of a
# name without that copy being a declaration.
#
# Matched as PATH SEGMENTS, not as substrings.  A substring test excluded
# scripts/build-web.bat and scripts/build-android.bat -- their FILENAMES
# contain "build" -- so the widening above silently did nothing for exactly the
# files it was added for.
SOURCE_SKIP_SEGMENTS = ("/third_party/", "/build/", "/dist/", "/_retired")


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def parse_frontmatter(text: str):
    m = FRONTMATTER_RE.match(text)
    if not m:
        return None, 0
    block = m.group(1)
    data = {}
    key = None
    for line in block.splitlines():
        if re.match(r"^\s", line) and key:
            data[key] += " " + line.strip()
            continue
        if ":" in line:
            key, _, val = line.partition(":")
            key = key.strip()
            data[key] = val.strip()
    return data, len(m.group(0))


_source_blob = None


def source_blob() -> str:
    """Concatenate first-party source once; env lookups are set membership."""
    global _source_blob
    if _source_blob is None:
        parts = []
        for pattern in SOURCE_GLOBS:
            for path in REPO_ROOT.glob(pattern):
                posix = "/" + path.as_posix().lstrip("/")
                if path.is_file() and not any(
                        seg in posix for seg in SOURCE_SKIP_SEGMENTS):
                    parts.append(read(path))
        _source_blob = "\n".join(parts)
    return _source_blob


def check_skill(skill_dir: Path, known_skills: set) -> tuple[list, list]:
    findings = []
    notes = []
    md = skill_dir / "SKILL.md"

    def fail(line, msg):
        findings.append("%s:%d: %s"
                        % (md.relative_to(md.parents[2]).as_posix()
                           if len(md.parents) > 2 else md.name, line, msg))

    if not md.exists():
        findings.append("%s: no SKILL.md" % skill_dir.name)
        return findings, notes

    text = read(md)
    fm, fm_len = parse_frontmatter(text)
    if fm is None:
        fail(1, "no YAML frontmatter (must start with a --- block)")
        return findings, notes

    if fm_len > 1024:
        fail(1, "frontmatter is %d chars, budget is 1024" % fm_len)
    name = fm.get("name", "")
    desc = fm.get("description", "")
    if not name:
        fail(1, "frontmatter has no `name`")
    else:
        if not NAME_RE.match(name):
            fail(1, "name %r must be lowercase letters/digits/hyphens" % name)
        if name != skill_dir.name:
            fail(1, "name %r does not match directory %r"
                 % (name, skill_dir.name))
    if not desc:
        fail(1, "frontmatter has no `description`")
    else:
        if not desc.lower().startswith("use when"):
            fail(1, "description should start with \"Use when...\" "
                    "(triggering conditions), got %r" % desc[:48])
        for pronoun in (" I ", "I'll", "I can", " we ", " you should "):
            if pronoun in " " + desc:
                fail(1, "description must be third person; found %r" % pronoun)

    # A skill may be one SKILL.md or SKILL.md plus a references/ tree.  The
    # body checks below must cover BOTH: a stale path is no less wrong for
    # sitting in a reference file, and that is where most of the prose is.
    docs = [md] + sorted(q for q in skill_dir.rglob("*.md") if q != md)
    ref_names = {q.relative_to(skill_dir).as_posix()
                 for q in skill_dir.rglob("*.md")}
    for doc in docs:
        _check_body(doc, skill_dir, read(doc), known_skills, ref_names,
                    findings, notes)

    # Every reference must be REACHABLE from the entry point.
    #
    # A reference file has no frontmatter of its own, so nothing loads it on
    # its own description -- SKILL.md's routing table is the only way an agent
    # ever reaches it.  A reference that sits on disk unrouted is therefore not
    # a reference, it is a file.  Nothing else here catches that: the
    # cross-reference check verifies that every `references/x.md` MENTIONED
    # resolves, which says nothing about one that is mentioned nowhere.
    # (Measured 2026-08-27: repository-state.md shipped in an archive while
    # absent from both indexes, and this checker was green.)
    entry = read(md)
    for name in sorted(ref_names):
        if not name.startswith("references/"):
            continue
        if name not in entry:
            # findings holds preformatted "<path>:<line>: <msg>" STRINGS, not
            # tuples -- appending a tuple printed the raw repr.
            fail(1, "%s is not routed to from SKILL.md. Reference files have "
                    "no frontmatter, so the routing table is the only way an "
                    "agent reaches one; an unrouted reference is unreachable, "
                    "not optional." % name)
    return findings, notes


def _check_body(doc: Path, skill_dir: Path, text: str, known_skills: set,
                ref_names: set, findings: list, notes: list):
    """Path / command / env / cross-reference checks for one markdown file."""
    label = doc.relative_to(skill_dir.parent).as_posix()

    def fail(line, msg):
        findings.append("%s:%d: %s" % (label, line, msg))

    lines = text.splitlines()

    # Control characters are ALWAYS a mistake here, and always the same one: a
    # shell heredoc collapsed a literal escape while this file was being
    # written.  A backslash-b inside a Windows path becomes BACKSPACE, so
    # "scripts\build-web.bat" renders as "scriptsuild-web.bat" -- invisible in
    # a terminal, wrong in the file, and no other check can see it, because the
    # path it names then looks merely ABSENT rather than corrupt.  Measured five
    # separate times on 2026-08-27, in the CR, LF and BACKSPACE variants -- and
    # once in the source of this very check.
    for idx, line in enumerate(lines, start=1):
        bad = sorted({c for c in line if ord(c) < 0x20 and ord(c) != 0x09})
        if bad:
            fail(idx, "control character(s) %s -- a literal escape was "
                      "collapsed when this file was written.  Build the "
                      "backslash with chr(92) rather than relying on the "
                      "shell's escaping."
                      % ", ".join("0x%02X" % ord(c) for c in bad))

    for idx, line in enumerate(lines, start=1):
        if ALLOW_MARK in line:
            continue
        # `references/foo.md` must resolve inside this skill.
        for ref in re.findall(r"`(references/[A-Za-z0-9_.-]+\.md)`", line):
            if ref not in ref_names:
                fail(idx, "points at `%s`, which is not a file in this skill"
                     % ref)
        for pm in PATH_RE.finditer(line):
            rel = pm.group(1).rstrip("/")
            if rel.startswith(("http", "~", "<")) or "*" in rel:
                continue
            if not rel.startswith(REPO_ROOTS):
                continue          # prose with a slash, not a path claim
            # A path named INSIDE a "this is gone" sentence is the opposite
            # claim, and the two checks must not both fire on it: the positive
            # check would report the absence it is asserting.  Decide which
            # claim this is once, then run only that one.
            tail = line[pm.end():pm.end() + NEGATION_WINDOW]
            if any(mark in tail for mark in NEGATION_MARKS):
                if (REPO_ROOT / rel).exists():
                    fail(idx, "claims `%s` does not exist, but it does — "
                              "reword, or add `%s` on this line if the claim "
                              "is about something narrower"
                              % (rel, ALLOW_MARK))
                continue
            if (REPO_ROOT / rel).exists():
                continue
            if rel.startswith(SOFT_DIRS):
                # `label`, not `md.name`: there is no `md` in this scope.
                # This branch had never once executed -- it needs a path that
                # is under a SOFT_DIR *and* absent right now, and no skill text
                # had named one until 2026-08-27.  A NameError in the reporting
                # path of a gate is invisible until the day the gate has
                # something to report.
                notes.append("%s:%d: %s not present in this worktree "
                             "(generated or gitignored)" % (label, idx, rel))
                continue
            fail(idx, "path `%s` does not exist" % rel)

        for script in PY_CMD_RE.findall(line):
            if not (REPO_ROOT / script).exists():
                fail(idx, "command references `%s`, which does not exist"
                     % script)

        for ref in SKILL_REF_RE.findall(line):
            if ref not in known_skills:
                fail(idx, "references skill `%s`, which does not exist" % ref)

    blob = source_blob()
    for idx, line in enumerate(lines, start=1):
        if ALLOW_MARK in line:
            continue
        for env in set(ENV_RE.findall(line)):
            if env in ENV_ALLOW:
                continue
            if env in blob:
                continue
            fail(idx, "env var `%s` appears nowhere in first-party source "
                      "(invented, renamed, or needs an ENV_ALLOW reason)" % env)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--skills-dir", default=str(DEFAULT_SKILLS_DIR))
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    root = Path(args.skills_dir)
    if not root.exists():
        print("SKIPPED check_skills: skills dir %s does not exist. The JCE "
              "skills are user-level (owner decision 2026-08-26) and do not "
              "travel with a clone; this is not a pass." % root)
        return 2

    # os.listdir, not iterdir+is_dir: the sources live in ~/.agents/skills and
    # ~/.claude/skills holds junctions to them.  A junction whose target moved
    # is still a NAME on disk but is not a directory, so an is_dir() filter
    # drops it silently and the run reports "one fewer skill" -- which reads as
    # "somebody deleted a skill", not "a link broke".  Name them, then say why.
    entries = sorted(n for n in os.listdir(root)
                     if n == SKILL_PREFIX or n.startswith(SKILL_PREFIX + "-"))
    dangling = [n for n in entries if not (root / n).is_dir()]
    skills = [root / n for n in entries if (root / n).is_dir()]
    if not skills:
        print("SKIPPED check_skills: no %s* skills under %s. Not a pass."
              % (SKILL_PREFIX, root))
        return 2

    known = {d.name for d in skills}

    if dangling:
        print("check_skills: FAIL - %d dangling link(s) under %s:"
              % (len(dangling), root))
        for n in dangling:
            print("  %s: the name is on disk but is not a readable directory "
                  "(junction target moved or deleted?)" % n)
        print("  This is a BROKEN LINK, not a missing skill. The sources live "
              "in ~/.agents/skills; ~/.claude/skills holds junctions to them.")
        return 1

    if args.list:
        for d in skills:
            fm, _ = parse_frontmatter(read(d / "SKILL.md"))
            desc = (fm or {}).get("description", "(no description)")
            print("  %-30s %s" % (d.name, desc[:96]))
        return 0

    all_findings = []
    all_notes = []
    for d in skills:
        f, n = check_skill(d, known)
        all_findings += ["%s: %s" % (d.name, x) for x in f]
        all_notes += ["%s: %s" % (d.name, x) for x in n]

    linked = [d for d in skills if d.is_symlink() or d.resolve() != d]
    print("check_skills: %d skills under %s" % (len(skills), root))
    if linked:
        print("  %d of them resolve elsewhere (sources), e.g. %s -> %s"
              % (len(linked), linked[0].name, linked[0].resolve().parent))
    for note in all_notes:
        print("  NOTE %s" % note)
    if all_findings:
        print("")
        print("check_skills: FAIL - %d finding(s):" % len(all_findings))
        for f in all_findings:
            print("  %s" % f)
        return 1

    print("check_skills: OK - every path, command, env var and cross-reference "
          "resolves.")
    print("  NOTE: this checks that the nouns exist, not that the sentences "
          "are true. Content correctness is the RED/GREEN scenarios' job.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
