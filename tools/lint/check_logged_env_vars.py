#!/usr/bin/env python3
"""
check_logged_env_vars.py — a log line that names an environment variable
nothing reads.

THE BUG THIS EXISTS FOR.  Both editor viewports logged

    "JCE_DISABLE_OCCLUSION set — scene-view occlusion culling OFF"

on every default run.  JCE_DISABLE_OCCLUSION is read by no getenv anywhere in
this tree and never has been: the switch is JCE_ENABLE_OCCLUSION, an opt-IN,
and the feature is off by design.  So the message fired when nothing was set,
named a variable that does not exist, and told the reader a diagnostic had been
left on.  The owner asked about it, and answering took reading four files.

A message like that is worse than no message.  It does not merely fail to
help -- it actively asserts something false about the machine it is running
on, and the reader has no way to tell from the log that it is wrong.

WHAT IS CHECKED.  Every LOG_* call in engine/, editor/ and tools/ is scanned
for JCE_* names inside its string literals.  A name is reported when BOTH:

  * no getenv("<name>") exists anywhere in the tree, and
  * the name never appears as a bare C token outside a string literal.

The second test is what makes this usable.  Logs legitimately mention macros,
enum members and build defines -- JCE_NO_AUDIO, JCE_ASYNC_EXECUTION_COOPERATIVE,
JCE_VERSION_STR, JCE_BGFX_OPENGL_VERSION.  Those all appear as real tokens
somewhere, so they are identifiers, not environment variables.  An env var name
appears ONLY inside strings, which is exactly the signature this looks for.
Earlier drafts that tried to enumerate the declaration forms instead reported
43, then 4, then 2 false positives; this one reports none.

Comments are deliberately NOT scanned.  A comment may name a retired variable
on purpose -- the one in jce_editor_viewport_common.h explains this very bug --
and a gate that flagged that would be switched off within a day.

ONLY C/C++ SOURCES CONTRIBUTE IDENTIFIERS, and that line is load-bearing.
Committing this file disarmed the gate.  The scan covers scripts/ too, so once
this file was tracked its own prose -- the paragraph above, which names
JCE_DISABLE_OCCLUSION while explaining it -- was collected as a bare token and
permanently whitelisted the one name the gate exists to catch.  The original
negative control passed only because the file was still UNTRACKED when it ran:
git ls-files did not list it, so its prose was not scanned.  Re-running that
control after the commit returns exit 0.

A Python docstring cannot define a C identifier, and neither can a CMake
variable.  Identifiers are collected from C/C++ translation units only; every
tracked file is still read for getenv() readers, because a build script may
legitimately be the thing that sets a variable.

Usage:
    python tools/lint/check_logged_env_vars.py
    python tools/lint/check_logged_env_vars.py --list
Exit 0 clean, 1 when a logged env var has no reader.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# Enumerate the WORKING TREE, not tracked-only: `git ls-files` hides a
# brand-new file until it is committed, so a defect introduced in a new file
# passes this checker on the commit that introduces it.  Measured: an internal
# function marked JCE_API sailed through a full run and failed on the next one,
# one commit late.  See tools/lint/lint_git_files.py.
import importlib.util as _ilu
_spec = _ilu.spec_from_file_location(
    "lint_git_files", str(Path(__file__).resolve().parent / "lint_git_files.py"))
_lgf = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(_lgf)


REPO_ROOT = Path(__file__).resolve().parents[2]

SCAN_PREFIXES = ("engine/", "editor/", "tools/", "scripting/", "cmake/",
                 "conan/", "scripts/")
SOURCE_SUFFIXES = (".c", ".cpp", ".h", ".hpp", ".inc")

STRING = re.compile(r'"(?:[^"\\]|\\.)*"')
GETENV = re.compile(r'getenv\s*\(\s*"(JCE_[A-Z0-9_]+)"')
TOKEN = re.compile(r'\b(JCE_[A-Z0-9_]{3,})\b')
LOG_OPEN = re.compile(r'LOG_[A-Z]+[ \t]*\(')
NAME_IN_STRING = re.compile(r'"[^"]*?\b(JCE_[A-Z0-9_]{3,})')

EXEMPT = REPO_ROOT / "tools" / "lint" / "logged_env_var_exempt.txt"


def strip_comments(text: str) -> str:
    """Blank /* */ and // regions, preserving length.

    Not optional.  The first version of this gate stripped string literals but
    not comments when looking for bare identifiers, so any name mentioned in a
    comment counted as "a real identifier" and was whitelisted forever -- and
    the comment in jce_editor_viewport_common.h that explains this very bug
    mentions JCE_DISABLE_OCCLUSION.  The gate therefore could not fail on the
    one case it was written for.  Its own negative control caught that.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join(c if c == chr(10) else " " for c in text[i:end]))
            i = end
        elif text.startswith("//", i):
            end = text.find(chr(10), i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def log_call_text(text: str, open_paren: int) -> str:
    """The full text of a LOG_*(...) call, by balancing parentheses.

    A regex cannot do this.  The first version used
    a regex ending in a literal `);` that also excluded semicolons, and it
    matched nothing on the very call it was written to catch: that message
    contains a SEMICOLON inside its string literal -- "off by default; enable
    with ..." -- so the scan stopped there.  Its own negative control caught
    that; without one it would have shipped green and blind.

    String literals are skipped so a parenthesis or quote inside a message
    cannot end the scan early.
    """
    n = len(text)
    depth = 0
    i = open_paren
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n:
                if text[i] == chr(92):
                    i += 2
                    continue
                if text[i] == '"':
                    break
                i += 1
        elif c == chr(40):
            depth += 1
        elif c == chr(41):
            depth -= 1
            if depth == 0:
                return text[open_paren:i + 1]
        i += 1
    return text[open_paren:min(n, open_paren + 2000)]


def tracked() -> list:
    out = _lgf.working_tree()
    return [f for f in out if f.startswith(SCAN_PREFIXES)]


def read_exemptions() -> dict:
    out = {}
    if not EXEMPT.is_file():
        return out
    for raw in EXEMPT.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        name, _, reason = line.partition(" ")
        out[name.strip()] = reason.strip()
    return out


def main() -> int:
    files = tracked()
    if len(files) < 200:
        print("check_logged_env_vars: FAIL - only %d tracked file(s) matched; "
              "the tree shape changed and this gate no longer sees the source."
              % len(files), file=sys.stderr)
        return 1

    readers: set = set()
    tokens: set = set()
    sources: dict = {}

    for rel in files:
        p = REPO_ROOT / rel
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        readers.update(m.group(1) for m in GETENV.finditer(text))
        if rel.endswith(SOURCE_SUFFIXES):
            # Bare tokens = the file with every string literal blanked out.
            # Strings AND comments removed: what is left is real code.
            # C/C++ ONLY -- see the header comment; scanning this file's own
            # Python prose is what disarmed the gate.
            tokens.update(TOKEN.findall(STRING.sub('""', strip_comments(text))))
            sources[rel] = text

    exempt = read_exemptions()
    found: dict = {}
    for rel, text in sources.items():
        for open_m in LOG_OPEN.finditer(text):
            call = log_call_text(text, open_m.end() - 1)
            for name in set(NAME_IN_STRING.findall(call)):
                if name in readers or name in tokens:
                    continue
                line = text[:open_m.start()].count(chr(10)) + 1
                found.setdefault(name, []).append((rel, line))

    if "--list" in sys.argv:
        print("getenv-read JCE_* variables : %d" % len(readers))
        print("JCE_* identifiers in code   : %d" % len(tokens))
        for name in sorted(found):
            for rel, line in found[name]:
                state = "EXEMPT" if name in exempt else "NO READER"
                print("%-36s %-10s %s:%d" % (name, state, rel, line))

    ok = True
    for name in sorted(found):
        if name in exempt:
            if not exempt[name]:
                ok = False
                print("  %s: exempted with no reason.  An exemption without a "
                      "stated reason cannot be told from an oversight."
                      % name, file=sys.stderr)
            continue
        ok = False
        where = ", ".join("%s:%d" % (r, l) for r, l in found[name][:3])
        print("  %s is named in a log message at %s, but no getenv reads it "
              "and it is not an identifier anywhere in this tree -- so the "
              "line fires on a machine where nothing of the sort is set and "
              "tells the reader something false about their own run.  Name "
              "the variable the code actually reads, or drop the mention."
              % (name, where), file=sys.stderr)

    for name in sorted(exempt):
        if name not in found:
            ok = False
            print("  %s: exempted, but no log names it any more.  Drop the "
                  "exemption." % name, file=sys.stderr)

    if not ok:
        print("check_logged_env_vars: FAILED", file=sys.stderr)
        return 1

    print("check_logged_env_vars: OK (%d env var(s) read via getenv, %d JCE_* "
          "identifier(s) in C/C++ sources; every JCE_* name in a log message "
          "is one of them)" % (len(readers), len(tokens)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
