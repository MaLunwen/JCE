#!/usr/bin/env python3
"""
report_readme_facts.py -- the numbers in README.md are generated, not typed.

WHY THIS EXISTS.  README.md listed FIFTEEN public umbrella headers.  The tree
has twenty-three.  Nobody edited the README wrongly; the tree moved and the
README did not, for a month, on the front page of a repository that is now
public.  That is the same defect this codebase keeps finding in its own gates
and ledgers -- A NUMBER THAT IS PRINTED BUT NOT DERIVED STOPS BEING TRUE AND
SAYS NOTHING WHEN IT DOES -- and the front page is the worst place for it,
because it is the one file a stranger reads before they can check anything.

So the volatile half of the README is a GENERATED BLOCK between two markers.
This script prints it; `--write` splices it in; tools/lint/check_readme_facts.py
regenerates and fails when the file has drifted.  The prose around the block
stays hand-written, because prose is not data.

EVERY COUNT COMES FROM THE INDEX -- `git ls-files` for the file list AND
`git cat-file` for the contents.  NEVER FROM THE DISK.  That is not a detail:
this working tree has untracked files that a clean clone does not, and a
README that counted what is on THIS machine would tell a reader about files
they will never receive.  The numbers describe the REPOSITORY, so they
reproduce for whoever clones it.

THIS SENTENCE WAS FALSE FOR THE CONTENTS UNTIL 2026-09-21, and it is worth
saying so rather than quietly correcting it.  The file LIST came from
`git ls-files`; the file CONTENTS came from `read_text()` on the working
tree.  So the numbers described the repository's file list with this
machine's file contents -- and they did not reproduce for a cloner, which is
the one property the paragraph claimed.  It went unnoticed for as long as it
did BECAUSE it was written this forcefully: a stated guarantee stops people
re-deriving it, so the more confident the sentence, the longer its error
survives.  Measured counterexample, same file list, two content sources:
working tree 3811 `JCE_API`, index 3809.  Two concurrent sessions each held
one uncommitted symbol in a different tracked header, and either one
regenerating would have committed a README asserting 3811 for a commit that
produced 3810.

If you change where these numbers come from, change this paragraph in the
same commit.

Usage:
    python tools/report_readme_facts.py            # print the block
    python tools/report_readme_facts.py --write    # splice it into README.md
"""
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
README = REPO_ROOT / "README.md"

BEGIN = "<!-- BEGIN GENERATED: readme-facts -->"
END = "<!-- END GENERATED: readme-facts -->"


def tracked(pattern):
    """Files matching a pathspec, as git sees them.  See the module docstring
    for why this is not a filesystem walk."""
    out = subprocess.run(["git", "ls-files", "-z", "--", pattern],
                         cwd=REPO_ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        # NOT an empty list.  A git that failed to run and a pathspec that
        # matches nothing are different facts, and returning [] for both makes
        # every count silently zero -- which `--write` would then splice into
        # a tracked README as if it were a measurement.
        raise SystemExit("report_readme_facts: `git ls-files -- %s` failed "
                         "(exit %d): %s" % (pattern, out.returncode,
                                            out.stderr.strip()))
    return [p for p in out.stdout.split("\0") if p]


def staged(rels):
    """Content of each path AS STAGED, keyed by path.

    THE INDEX, NOT THE DISK, AND NOT HEAD.  The docstring above promises that
    these numbers describe the repository a cloner receives, and only the
    index delivers that:

      disk  counts whatever every concurrent session happens to be holding
            mid-edit.  Measured 2026-09-21 on this very file's numbers: two
            sessions each had one uncommitted `JCE_API` in a different tracked
            header, disk said 3811, and EITHER session regenerating would have
            committed a README claiming 3811 when its own commit produced
            3810.  Disk is wrong by exactly the set of files somebody else is
            editing, which is not a set this script can see.

      HEAD  is wrong in the opposite direction and worse, because it is wrong
            every time rather than occasionally: `--write` runs BEFORE the
            commit, so HEAD numbers describe the tree without the very change
            they are shipping with, and the gate goes red the instant that
            commit lands.  A generator that reads HEAD can never be correct
            for the commit it is part of.

      index IS the commit.  After `git add`, unstaged paths read at their HEAD
            content and staged paths read as staged, so the count is exactly
            what the cloner gets.  This also retires the "git add first,
            regenerate second" ordering rule -- it stops being something a
            human has to remember and becomes true by construction.

    One `git cat-file --batch` for the whole set: `git show :path` per file is
    the same answer but ~300 subprocesses for the header sweep alone.
    """
    rels = list(rels)
    if not rels:
        return {}
    proc = subprocess.run(["git", "cat-file", "--batch"],
                          cwd=REPO_ROOT, capture_output=True,
                          input="".join(":%s\n" % r for r in rels).encode())
    if proc.returncode != 0:
        raise SystemExit("report_readme_facts: `git cat-file --batch` failed "
                         "(exit %d): %s" % (proc.returncode,
                                            proc.stderr.decode(errors="replace")))
    out, pos, result = proc.stdout, 0, {}
    for rel in rels:
        nl = out.index(b"\n", pos)
        header = out[pos:nl].decode(errors="replace")
        if header.endswith(("missing", "ambiguous")):
            # Staged-but-unreadable is a real state (a path removed from the
            # index); skip it rather than counting it as empty.
            pos = nl + 1
            continue
        size = int(header.rsplit(" ", 1)[1])
        body = out[nl + 1:nl + 1 + size]
        result[rel] = body.decode("utf-8", errors="replace")
        pos = nl + 1 + size + 1      # trailing newline after the blob
    return result


def staged_one(rel):
    """The single-path form of staged(); raises if the path is not in the
    index, because every caller of this reads a file that must exist."""
    got = staged([rel])
    if rel not in got:
        raise SystemExit("report_readme_facts: %s is not in the index" % rel)
    return got[rel]


def version():
    m = re.search(r"VERSION\s+(\d+\.\d+\.\d+)", staged_one("CMakeLists.txt"))
    return m.group(1) if m else "unknown"


def api_symbol_count():
    rels = tracked("engine/include/jce/**/*.h")
    if not rels:
        # The sweep matching nothing is a broken script, not an API-free
        # engine.  Without this the README would record 0 and the gate would
        # agree with it, because both sides load this same module.
        raise SystemExit("report_readme_facts: the public-header sweep "
                         "matched no files -- refusing to report 0 symbols")
    return sum(text.count("JCE_API") for text in staged(rels).values())


def umbrellas():
    """Each umbrella and the one line it uses to describe itself.

    Taken from the header's OWN second line (`api_render.h  Layer 4 -- Render
    abstraction.`) rather than from a table beside it, so the description
    cannot disagree with the file -- which is the whole failure this script
    exists to stop, one level down.
    """
    rows = []
    rels = sorted(tracked("engine/include/jce/api*.h"))
    blobs = staged(rels)
    for rel in rels:
        name = Path(rel).name
        if name == "api.h":
            continue
        text = blobs.get(rel, "")
        lines = text.splitlines()
        desc = ""
        for ln in lines[:4]:
            m = re.match(r"\s*\*?\s*" + re.escape(name) + r"\s+(.*)", ln)
            if m:
                desc = m.group(1).strip().rstrip(".")
                break
        rows.append((name, desc))
    return rows


def script_languages():
    """The binding trees under scripting/, which is where a language IS.

    c_abi is the shared flat ABI every binding sits on, not a language, so it
    is named separately rather than counted as one.  Lua is built into the
    engine and has no tree here.
    """
    dirs = sorted({p.split("/")[1] for p in tracked("scripting/*/**")
                   if p.count("/") >= 2})
    dirs = [d for d in dirs if d not in ("cmake",)]
    return dirs


# THERE IS NO CHECKER COUNT IN THE BLOCK, AND THAT IS THE FIX RATHER THAN A
# GAP.  Three attempts, three different ways of being wrong:
#
#   1. `git ls-files -- "tools/lint/check_*.py"` answered 67 while the block
#      was generated, because the two checkers added in that same commit were
#      not tracked yet.  A count taken from the tree BEFORE the commit that
#      changes the tree is stale the instant it lands.
#
#   2. `git ls-files -- "tools/audit/*.py"` is not "the .py files in that
#      directory": a pathspec's `*` crosses `/`, so the moment
#      tools/audit/tests/ became tracked the count went 16 -> 22 and began
#      counting the gates' own unit tests as gates.
#
#   3. Reading the runner's own LINTS list -- which sounds exact -- reads the
#      WORKING TREE, and the working tree is not what a clone receives.  With
#      one uncommitted lint entry in run_all.py it answered 72 here and 71 on
#      a clean clone, so the gate failed on a README that was correct for the
#      machine that wrote it.
#
# The project's own rule already said this: "do not put a count into a
# criterion -- adding one checker turns a correct criterion false."  The
# criterion is the exit code of two commands, so the block names the commands.
# This note stays because the third attempt is the one that looks right.


def parity():
    # Also read from the index, and not incidentally: this contract is one of
    # the files a concurrent session is most likely to be holding mid-edit,
    # since every parity change touches it.
    rel = "contracts/engine-parity.json"
    blobs = staged([rel])
    if rel not in blobs:
        return None
    d = json.loads(blobs[rel])
    rows = d.get("features", [])
    counts = {}
    for r in rows:
        counts[r.get("status", "?")] = counts.get(r.get("status", "?"), 0) + 1
    scored = len(rows) - counts.get("na", 0)
    good = counts.get("parity", 0) + counts.get("ahead", 0)
    return rows, counts, scored, good


def block():
    out = []
    a = out.append

    a(BEGIN)
    a("<!-- Regenerate: python tools/report_readme_facts.py --write")
    a("     Gated by:   tools/lint/check_readme_facts.py")
    a("     Counts come from the INDEX (list and contents), so they describe")
    a("     the REPOSITORY as this commit delivers it")
    a("     and reproduce on a fresh clone -- not this machine's disk. -->")
    a("")
    a("### At a glance")
    a("")
    a("| | |")
    a("| --- | --- |")
    a("| Version | `%s` (authoritative: `CMakeLists.txt`) |" % version())
    a("| Engine (C99) | %d tracked `.c` under `engine/src/` |"
      % len(tracked("engine/src/**/*.c")))
    a("| Editor (C++20) | %d tracked `.cpp` under `editor/src/`, %d panels |"
      % (len(tracked("editor/src/**/*.cpp")),
         len(tracked("editor/src/panels/*.cpp"))))
    a("| Public API | %d headers under `engine/include/jce/`, %d umbrellas, "
      "%d `JCE_API` symbols |"
      % (len(tracked("engine/include/jce/**/*.h")), len(umbrellas()),
         api_symbol_count()))
    a("| Shaders | %d `.sc` under `engine/shaders/` |"
      % len(tracked("engine/shaders/**/*.sc")))
    a("| Scripting | %d binding trees under `scripting/` (%s) over one flat C "
      "ABI, plus Lua built in |"
      % (len([d for d in script_languages() if d != "c_abi"]),
         ", ".join(d for d in script_languages() if d != "c_abi")))
    a("| Editor languages | %d locales under "
      "`editor/resources/assets/i18n/` |"
      % len(tracked("editor/resources/assets/i18n/*.json")))
    a("| Build matrix | %d CMake presets, %d Conan profiles |"
      % (len(json.loads(staged_one("CMakePresets.json"))["configurePresets"]),
         len([p for p in tracked("conan/**") if "profile" in p])))
    a("| Gates | `tools/lint/run_all.py` + "
      "`tools/audit/run_architecture_audit.py` -- both must exit 0 with no "
      "`FAIL` line |")

    pr = parity()
    if pr:
        rows, counts, scored, good = pr
        a("| Parity ledger | %d measured rows vs Unity / Unreal / Godot -- "
          "%d ahead, %d parity, %d behind, %d n/a (%.1f%% at or above) |"
          % (len(rows), counts.get("ahead", 0), counts.get("parity", 0),
             counts.get("behind", 0), counts.get("na", 0),
             100.0 * good / scored if scored else 0.0))

    a("")
    a("### Public API surface")
    a("")
    a("`#include <jce/api.h>` reaches all of it.  Each umbrella below "
      "describes itself on its own second line; this table is generated from "
      "those lines, so it cannot disagree with the headers.")
    a("")
    a("| Header | Covers |")
    a("| --- | --- |")
    for name, desc in umbrellas():
        a("| `<jce/%s>` | %s |" % (name, desc or "&mdash;"))
    a("")
    a(END)
    return "\n".join(out)


def main():
    text = block()
    if "--write" not in sys.argv:
        print(text)
        return 0

    src = README.read_text(encoding="utf-8")
    if BEGIN not in src or END not in src:
        print("report_readme_facts: README.md has no generated block; add the "
              "two markers first:\n  %s\n  %s" % (BEGIN, END), file=sys.stderr)
        return 2
    head = src.split(BEGIN)[0]
    tail = src.split(END, 1)[1]
    README.write_text(head + text + tail, encoding="utf-8", newline="\n")
    print("report_readme_facts: README.md block updated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
