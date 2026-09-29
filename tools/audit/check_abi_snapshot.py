#!/usr/bin/env python3
"""
check_abi_snapshot.py — freeze the public C ABI and fail on silent breakage
(plan §34: "ABI dump/diff").

WHAT PROBLEM THIS SOLVES
------------------------
JCE ships a flat C ABI that SDK consumers, Lua bindings and the JNI bridge
all compile against.  Nothing today notices when a public declaration
changes: rename a parameter type, widen an enum, drop a function, reorder a
struct's fields, and every existing check still passes — the breakage
surfaces as a link error or, worse, silent corruption in somebody else's
build weeks later.  check_public_api_purity.py and check_public_abi.py both
answer "is this header CLEAN?"; neither answers "is it the SAME?".

So: extract a normalised snapshot of the public surface, keep it in the repo,
and diff on every run.

WHAT COUNTS AS BREAKAGE
-----------------------
  REMOVED  — a declaration that existed is gone            -> FAIL
  CHANGED  — same name, incompatible signature / layout    -> FAIL
  APPENDED — a record gained members ONLY at the end       -> reported, OK
  MOVED    — identical declaration, different header       -> reported, OK
  ADDED    — a new declaration                             -> reported, OK

Additions are safe for consumers; removals and changes are not.  When a break
is intentional, re-run with --update and commit the regenerated snapshot in
the SAME commit as the ABI change, so review sees both together.

APPENDED vs CHANGED is the ORDERED-PREFIX RULE, and it is the difference
between a safe edit and a silent P0.  Appending an enumerator or a struct
member is source- and slot-compatible; INSERTING one in the middle shifts
every later enumerator value and every later struct offset, so code already
compiled against the old header reads the wrong field or calls through the
wrong function-pointer slot.  For any record present in both snapshots the
old member list must be a PREFIX of the new one; when it is not, the report
names the member and its index.  Until this rule existed the gate compared
the whole normalised record text and said CHANGED either way, so "APPEND
ONLY" comments in public headers (jce_script.h's JceScriptHost is the loud
one) were enforced by nothing but a human reading a diff.

WHICH TREE IS BEING CHECKED
---------------------------
Default: the WORKING TREE.  Answers "am I about to break the ABI?".

--committed: the COMMITTED tree.  Reads both the headers and the baseline out
of the object database at HEAD and requires them to agree EXACTLY — no
pending additions, no pending appends.  This is the gate that makes a red
snapshot somebody's job:

  * It is immune to uncommitted work, so in a worktree several sessions share
    it can never be red for a colleague's half-finished header — the failure
    mode that trains people to ignore a checker.
  * Its answer is the answer CI gets, because CI checks out exactly HEAD.
    The working-tree run is NOT: `--update` records whatever is in the tree,
    so a baseline regenerated over somebody else's dirty headers is green
    locally and red in CI.  That is not hypothetical — it is how commit
    bdce6823 re-baselined this file over two untracked headers, and the
    working-tree gate reported OK while HEAD carried 44 removals.
  * Red means one thing only: a commit changed engine/include/jce without
    updating contracts/abi-snapshot.txt in the same commit.  The
    report runs `git log` and names the commits, so "not mine" is checkable
    instead of assumable.

WHAT IS AND IS NOT CHECKED
--------------------------
This is a TEXTUAL snapshot taken by parsing headers — it needs no compiler
and no build, which is what lets it run in the lint job.  The tradeoff is
real and worth stating plainly:

  * It sees declarations, not the compiler's view.  It cannot catch a change
    in a macro that alters a type, an #if that hides a field on one platform,
    or a struct whose SIZE changed because a member type changed elsewhere.
  * It normalises whitespace and comments, so pure formatting churn does not
    produce diffs.
  * Parameter NAMES are dropped, parameter TYPES are kept: renaming a
    parameter is not an ABI change, retyping one is.

A true ABI dump (abi-dumper / abidiff over DWARF) would catch the rest, but
requires a full build with debug info on every platform.  This is the cheap
gate that catches the common case; the gap is documented rather than hidden.

Usage:
  python tools/audit/check_abi_snapshot.py             # working tree, exit 1 on break
  python tools/audit/check_abi_snapshot.py --committed # HEAD vs HEAD, exit 1 on ANY drift
  python tools/audit/check_abi_snapshot.py --update    # regenerate from the working tree
  python tools/audit/check_abi_snapshot.py --update --committed
                                                       # regenerate from HEAD (repair)
  python tools/audit/check_abi_snapshot.py --json
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import subprocess
import sys
import tarfile
from pathlib import Path

# The parser lives in tools/scriptgen/cdecl.py -- ONE C-declaration parser for
# the repo, beside gen_script_bindings.py, its other consumer, so this gate
# and the generator read the same headers the same way.  sys.path is set
# explicitly rather than relying on argv[0]'s directory, because
# tools/audit/tests/ loads this file through importlib, which does NOT put
# this file's directory on the path.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scriptgen"))
from cdecl import (                                        # noqa: E402
    _DECOR,
    _matching_brace,
    iter_typedef_struct_bodies,
    norm_ws,
    split_params,
    split_struct_fields,
    strip_comments_and_strings,
    TYPEDEF_STRUCT_OPEN_RE,
    TYPEDEF_STRUCT_TAIL_RE,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
# Kept as repo-relative posix strings as well: the --committed mode addresses
# these paths through `git archive` / `git show`, which speak repo-relative
# posix and nothing else.
PUBLIC_REL = "engine/include/jce"
SNAPSHOT_REL = "contracts/abi-snapshot.txt"
PUBLIC_ROOT = REPO_ROOT / PUBLIC_REL
SNAPSHOT = REPO_ROOT / SNAPSHOT_REL
HEADER_SUFFIXES = (".h", ".hpp")

# ---------------------------------------------------------------------------
# Header text -> normalised declarations
# ---------------------------------------------------------------------------

FUNC_RE = re.compile(
    r"JCE_API\s+(?P<sig>[^;{}]+?)\s*\((?P<params>[^;{}]*?)\)\s*;",
    re.S)

TYPEDEF_STRUCT_RE = re.compile(r"typedef\s+struct\s+(\w+)\s+(\w+)\s*;")
TYPEDEF_FN_RE = re.compile(
    r"typedef\s+(?P<ret>[\w \*]+?)\s*\(\s*\*\s*(?P<name>\w+)\s*\)\s*"
    r"\((?P<params>[^;]*?)\)\s*;", re.S)
ENUM_RE = re.compile(r"typedef\s+enum\s*(?:\w+\s*)?\{(?P<body>[^}]*)\}\s*(?P<name>\w+)\s*;",
                     re.S)


def parse_header_text(raw: str, rel: str) -> list[str]:
    """Declarations of one header, given its TEXT and its path under PUBLIC_ROOT.

    Split out from parse_header() so the --committed mode can feed it a blob
    read straight from the object database.  Two parsers would be two ABI
    definitions, and the one nobody runs is the one that rots."""
    text = strip_comments_and_strings(raw)
    decls: list[str] = []

    for m in FUNC_RE.finditer(text):
        sig = norm_ws(_DECOR.sub("", m.group("sig")))
        mm = re.match(r"^(?P<ret>.*?[\s\*])(?P<name>\w+)$", sig)
        if not mm:
            continue
        ret = norm_ws(mm.group("ret"))
        name = mm.group("name")
        params = ", ".join(split_params(m.group("params"))) or "void"
        decls.append(f"fn {name} :: ({params}) -> {ret}    [{rel}]")

    for m in TYPEDEF_FN_RE.finditer(text):
        params = ", ".join(split_params(m.group("params"))) or "void"
        ret = norm_ws(_DECOR.sub("", m.group("ret")))
        decls.append(f"fnptr {m.group('name')} :: ({params}) -> {ret}    [{rel}]")

    for m in TYPEDEF_STRUCT_RE.finditer(text):
        decls.append(f"opaque {m.group(2)} = struct {m.group(1)}    [{rel}]")

    for name, body in iter_typedef_struct_bodies(text):
        fields = split_struct_fields(body)
        decls.append(f"struct {name} {{ {'; '.join(fields)} }}    [{rel}]")

    for m in ENUM_RE.finditer(text):
        # Enumerator names AND explicit values both matter: a reordered enum
        # silently renumbers every consumer that stored an int.
        vals = [norm_ws(v) for v in m.group("body").split(",") if norm_ws(v)]
        decls.append(f"enum {m.group('name')} {{ {', '.join(vals)} }}    [{rel}]")

    return decls


def parse_header(path: Path) -> list[str]:
    return parse_header_text(
        path.read_text(encoding="utf-8", errors="replace"),
        path.relative_to(PUBLIC_ROOT).as_posix())


def build_snapshot() -> tuple[list[str], int]:
    """(declarations, headers read) for the WORKING TREE."""
    decls: list[str] = []
    nfiles = 0
    for r, _dirs, files in os.walk(PUBLIC_ROOT):
        for fn in files:
            if fn.endswith(HEADER_SUFFIXES):
                decls.extend(parse_header(Path(r) / fn))
                nfiles += 1
    return sorted(set(decls)), nfiles


# ---------------------------------------------------------------------------
# The COMMITTED tree — read out of the object database, never off disk
# ---------------------------------------------------------------------------


class GitError(RuntimeError):
    pass


def _git(args: list[str], binary: bool = False):
    """Run git at REPO_ROOT.  Returns (rc, out); raises only if git is absent."""
    try:
        p = subprocess.run(["git", *args], cwd=str(REPO_ROOT),
                           capture_output=True,
                           **({} if binary else
                              {"text": True, "encoding": "utf-8",
                               "errors": "replace"}))
    except OSError as e:
        raise GitError(f"git is not runnable: {e}") from e
    return p.returncode, (p.stdout if p.returncode == 0 else
                          (p.stderr or (b"" if binary else "")))


def git_work_tree() -> bool:
    try:
        rc, _ = _git(["rev-parse", "--git-dir"])
    except GitError:
        return False
    return rc == 0


def build_snapshot_from_commit(rev: str = "HEAD") -> tuple[list[str], int]:
    """(declarations, headers read) for PUBLIC_REL as COMMITTED at `rev`.

    One `git archive` rather than one `git show` per header: ~400 subprocess
    spawns is a second and a half on Windows, and a gate people wait on is a
    gate people skip."""
    rc, out = _git(["archive", "--format=tar", rev, PUBLIC_REL], binary=True)
    if rc != 0:
        raise GitError(out.decode("utf-8", "replace").strip() or
                       f"git archive {rev} {PUBLIC_REL} failed")
    prefix = PUBLIC_REL + "/"
    decls: list[str] = []
    nfiles = 0
    with tarfile.open(fileobj=io.BytesIO(out)) as tf:
        for member in tf.getmembers():
            if not member.isfile() or not member.name.endswith(HEADER_SUFFIXES):
                continue
            fh = tf.extractfile(member)
            if fh is None:
                continue
            raw = fh.read().decode("utf-8", "replace")
            decls.extend(parse_header_text(raw, member.name[len(prefix):]))
            nfiles += 1
    return sorted(set(decls)), nfiles


def read_committed_snapshot(rev: str = "HEAD") -> list[str] | None:
    try:
        rc, out = _git(["show", f"{rev}:{SNAPSHOT_REL}"])
    except GitError:
        return None
    if rc != 0:
        return None
    return [ln for ln in out.splitlines()
            if ln.strip() and not ln.startswith("#")]


def dirty_public_headers() -> list[str] | None:
    """`git status --porcelain` rows for PUBLIC_REL, or None if git is absent."""
    try:
        rc, out = _git(["status", "--porcelain", "--", PUBLIC_REL])
    except GitError:
        return None
    if rc != 0:
        return None
    return [ln for ln in out.splitlines() if ln.strip()]


# ---------------------------------------------------------------------------
# Diff
# ---------------------------------------------------------------------------

KEY_RE = re.compile(r"^(?P<kind>\w+) (?P<name>\w+)")

# Greedy body, anchored tail: a nested anonymous struct/union puts `}` inside
# the body, and only the LAST one is followed by the `    [path]` locator.
RECORD_LINE_RE = re.compile(
    r"^(?P<kind>struct|enum) (?P<name>\w+) \{ (?P<body>.*) \}"
    r"    \[(?P<path>[^\]]*)\]$")


def keyed(lines: list[str]) -> dict[str, str]:
    out = {}
    for ln in lines:
        m = KEY_RE.match(ln)
        if m:
            out[f"{m.group('kind')} {m.group('name')}"] = ln
    return out


def record_members(line: str) -> tuple[str, list[str]] | None:
    """(header path, ordered member list) for a `struct`/`enum` snapshot line.

    Returns None for `fn` / `fnptr` / `opaque`, which have no member ORDER to
    reason about — a changed signature there is simply incompatible.

    The split mirrors the emitter exactly: struct fields were joined with
    "; " after a depth-aware split, so they are split back depth-aware
    (a nested `union { float f; uint8_t raw[16]; }` stays ONE member);
    enumerators were joined with ", " after a plain split, so a plain split
    is the exact inverse."""
    m = RECORD_LINE_RE.match(line)
    if not m:
        return None
    body = m.group("body")
    if m.group("kind") == "enum":
        members = [norm_ws(v) for v in body.split(",") if norm_ws(v)]
    else:
        members = split_struct_fields(body)
    return m.group("path"), members


def classify_change(before: str, after: str) -> tuple[str, str]:
    """ORDERED-PREFIX RULE.  -> ("appended"|"moved"|"changed", human detail).

    "appended" and "moved" are compatible with an already-compiled consumer;
    "changed" is not.  A pure append leaves every existing member at the same
    index, which is the only property that keeps an old binary correct."""
    b_rec, a_rec = record_members(before), record_members(after)
    if b_rec is None or a_rec is None:
        return "changed", "signature changed"

    b_path, b_mem = b_rec
    a_path, a_mem = a_rec

    if b_mem == a_mem:
        # Identical member list: the only thing that moved is the header it
        # is declared in.  Layout- and slot-compatible, but a consumer that
        # #includes the old path directly stops compiling, so it is reported.
        return "moved", f"declared in {b_path} -> {a_path}"

    if len(a_mem) > len(b_mem) and a_mem[:len(b_mem)] == b_mem:
        gained = a_mem[len(b_mem):]
        shown = ", ".join(f"[{len(b_mem) + i}] {g}" for i, g in enumerate(gained))
        return "appended", f"{len(gained)} member(s) appended at the end: {shown}"

    # Not a prefix: something at an existing index moved, changed or vanished.
    n = min(len(b_mem), len(a_mem))
    i = next((k for k in range(n) if b_mem[k] != a_mem[k]), n)
    old = b_mem[i] if i < len(b_mem) else None
    new = a_mem[i] if i < len(a_mem) else None

    if new is None:
        why = (f"member [{i}] {old!r} was REMOVED — the record lost "
               f"{len(b_mem) - len(a_mem)} trailing member(s)")
    elif old is None:                                  # unreachable: prefix case
        why = f"member [{i}] {new!r} appeared past the end of the old record"
    elif old in a_mem[i + 1:]:
        why = (f"{new!r} was INSERTED at index {i}: {old!r} was index {i} and "
               f"is now index {a_mem.index(old, i + 1)}, and so is everything "
               f"after it")
    elif new in b_mem[i + 1:]:
        why = (f"member [{i}] {old!r} was REMOVED: {new!r} was index "
               f"{b_mem.index(new, i + 1)} and moved up to {i}")
    else:
        why = f"member [{i}] changed: {old!r} -> {new!r}"

    return "changed", why


def diff_snapshot(baseline: list[str], current: list[str]) -> dict:
    """Every difference, sorted into fatal (removed/changed) and drift."""
    b, c = keyed(baseline), keyed(current)
    out = {
        "keyed_before": b, "keyed_after": c,
        "removed": sorted(set(b) - set(c)),
        "added": sorted(set(c) - set(b)),
        "changed": [], "appended": [], "moved": [],
    }
    for k in sorted(set(b) & set(c)):
        if b[k] == c[k]:
            continue
        cat, why = classify_change(b[k], c[k])
        out[cat].append((k, why))
    out["fatal"] = len(out["removed"]) + len(out["changed"])
    out["drift"] = (out["fatal"] + len(out["added"]) + len(out["appended"])
                    + len(out["moved"]))
    return out


def _listing(items, render, cap: int = 20) -> None:
    for it in items[:cap]:
        print(render(it))
    if len(items) > cap:
        print(f"    ... and {len(items) - cap} more")


HOW_TO_FIX = (
    "  Regenerate the baseline and commit it WITH the header change:\n"
    "\n"
    "      python tools/audit/check_abi_snapshot.py --update\n"
    "      git add engine/include/... contracts/abi-snapshot.txt\n"
    "\n"
    "  READ THE DIFF BEFORE YOU COMMIT IT.  `--update` does not verify\n"
    "  anything: it records WHATEVER IS IN YOUR WORKING TREE, including\n"
    "  headers you have not committed and headers another session left dirty\n"
    "  in a shared worktree.  Run\n"
    "\n"
    "      git diff contracts/abi-snapshot.txt\n"
    "\n"
    "  and confirm every line you are about to record is a change you meant\n"
    "  to make.  If the diff contains somebody else's work, you have just\n"
    "  made this file green on your machine and red in CI — which is exactly\n"
    "  how it spent six days red with a real REMOVED hidden inside it.\n"
    "  `--update --committed` regenerates from HEAD instead and is the repair\n"
    "  for a baseline that already got polluted that way.\n")

WHY_FATAL = (
    "  Why this is fatal and not a warning: the snapshot is the ONLY record\n"
    "  of what the public C ABI was.  While it disagrees with the headers,\n"
    "  every later ABI break lands in a report that is already red, and the\n"
    "  next reader — correctly, for their own change — concludes the pending\n"
    "  entries are not theirs and moves on.  A warning cannot un-ship an ABI\n"
    "  break that reached an SDK consumer, a Lua binding or the JNI bridge.\n")


def print_update_provenance() -> None:
    """Say, out loud, which tree `--update` just recorded.

    An `--update` that silently swallows a colleague's uncommitted headers
    looks identical to a clean one, and the difference only surfaces in CI."""
    dirty = dirty_public_headers()
    if dirty is None:
        print("  provenance: git unavailable — cannot tell whether the headers "
              "just recorded are committed.")
        return
    if not dirty:
        print(f"  provenance: {PUBLIC_REL} is clean — the snapshot records "
              f"committed headers only.")
        return
    print(f"  WARNING: {len(dirty)} uncommitted change(s) under {PUBLIC_REL} "
          f"were recorded into the baseline:")
    _listing(dirty, lambda ln: f"      {ln}", cap=15)
    print("  If any of those are not yours, do NOT commit this snapshot: it")
    print("  will be green here and red in CI.  Repair with:")
    print("      python tools/audit/check_abi_snapshot.py --update --committed")


def report_blame() -> None:
    """Turn 'somebody changed a header' into a list of commits with names."""
    try:
        rc, out = _git(["log", "-1", "--format=%h %as %an  %s",
                        "--", SNAPSHOT_REL])
    except GitError:
        return
    if rc != 0 or not out.strip():
        return
    last = out.strip().splitlines()[0]
    print(f"  The baseline was last updated by:  {last}")
    sha = last.split()[0]
    rc, out = _git(["log", "--format=%h %as %an  %s", f"{sha}..HEAD",
                    "--", PUBLIC_REL])
    if rc != 0:
        return
    rows = [ln for ln in out.splitlines() if ln.strip()]
    if not rows:
        print(f"  No commit has touched {PUBLIC_REL} since — the drift is in "
              f"that baseline commit itself.")
        return
    print(f"  {len(rows)} commit(s) have changed {PUBLIC_REL} since, without "
          f"updating it:")
    _listing(rows, lambda ln: f"      {ln}", cap=15)


def run_committed(as_json: bool) -> int:
    """HEAD's headers vs HEAD's baseline.  Any drift at all is a failure."""
    if not git_work_tree():
        print("abi-snapshot[committed]: SKIPPED — not a git work tree.")
        print("  This gate reads the committed headers and the committed")
        print("  baseline out of the object database, so it needs git history;")
        print("  a source export or release tarball legitimately has none.")
        print("  NOTHING WAS CHECKED.  The working-tree gate still applies:")
        print("      python tools/audit/check_abi_snapshot.py")
        return 0

    try:
        current, nfiles = build_snapshot_from_commit("HEAD")
    except GitError as e:
        print(f"abi-snapshot[committed]: FAILED — cannot read {PUBLIC_REL} at "
              f"HEAD: {e}", file=sys.stderr)
        return 2

    # "Found nothing" and "did not look" print the same thing unless one of
    # them is made to fail.  A path typo, a rename of engine/include, or an
    # archive that matched no pathspec all land here.
    if nfiles == 0 or not current:
        print(f"abi-snapshot[committed]: FAILED — read {nfiles} header(s) and "
              f"{len(current)} declaration(s) from {PUBLIC_REL} at HEAD.")
        print("  Zero is not 'clean', it is 'did not look'.  Either the public")
        print("  header root moved or this gate is addressing the wrong path.")
        return 2

    baseline = read_committed_snapshot("HEAD")
    if baseline is None:
        print("abi-snapshot[committed]: FAILED — no committed baseline at "
              f"{SNAPSHOT_REL}.")
        print(f"  {nfiles} public header(s) at HEAD produce {len(current)} "
              f"declaration(s) that nothing is recording.")
        print(HOW_TO_FIX, end="")
        return 1

    d = diff_snapshot(baseline, current)

    if as_json:
        b, c = d["keyed_before"], d["keyed_after"]
        print(json.dumps({
            "mode": "committed", "headers": nfiles,
            "declarations": len(current),
            "removed": [b[k] for k in d["removed"]],
            "changed": [{"key": k, "why": w, "before": b[k], "after": c[k]}
                        for k, w in d["changed"]],
            "appended": [{"key": k, "why": w} for k, w in d["appended"]],
            "moved": [{"key": k, "why": w} for k, w in d["moved"]],
            "added": [c[k] for k in d["added"]],
        }, indent=1))
        return 1 if d["drift"] else 0

    if not d["drift"]:
        print(f"abi-snapshot[committed]: OK — {nfiles} public header(s) at HEAD "
              f"produce {len(current)} declaration(s), exactly the set recorded "
              f"in {SNAPSHOT_REL} at HEAD.")
        return 0

    b, c = d["keyed_before"], d["keyed_after"]
    print("abi-snapshot[committed]: FAILED — a commit changed public headers "
          "without updating the ABI snapshot")
    print()
    print(f"  {SNAPSHOT_REL} as committed at HEAD does not describe")
    print(f"  {PUBLIC_REL} as committed at HEAD.  {nfiles} header(s) read, "
          f"{len(current)} declaration(s) found, {d['drift']} disagreement(s).")
    print()
    for k in d["removed"]:
        print(f"  REMOVED   {b[k]}")
    for k, why in d["changed"]:
        print(f"  CHANGED   {k} — {why}")
        print(f"      was:  {b[k]}")
        print(f"      now:  {c[k]}")
    for k, why in d["appended"]:
        print(f"  APPENDED  {k} — {why}")
    for k, why in d["moved"]:
        print(f"  MOVED     {k} — {why}")
    _listing(d["added"], lambda k: f"  ADDED     {c[k]}")
    print()
    print(f"  {len(d['removed'])} removal(s), {len(d['changed'])} incompatible "
          f"change(s), {len(d['appended'])} append(s), {len(d['moved'])} "
          f"move(s), {len(d['added'])} addition(s).")
    print()
    report_blame()
    print()
    print(HOW_TO_FIX, end="")
    print()
    print(WHY_FATAL, end="")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true",
                    help="regenerate the baseline snapshot")
    ap.add_argument("--committed", action="store_true",
                    help="check (or, with --update, regenerate from) HEAD "
                         "instead of the working tree")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    if not PUBLIC_ROOT.is_dir():
        print(f"error: public ABI root missing: {PUBLIC_ROOT}", file=sys.stderr)
        return 2

    if args.committed and not args.update:
        return run_committed(args.json)

    if args.update and args.committed:
        # Repair path: regenerate from the object database so a baseline that
        # captured somebody's dirty tree can be put back to the committed
        # truth without touching (or needing to stash) their work.
        if not git_work_tree():
            print("error: --update --committed needs a git work tree",
                  file=sys.stderr)
            return 2
        try:
            current, nfiles = build_snapshot_from_commit("HEAD")
        except GitError as e:
            print(f"error: {e}", file=sys.stderr)
            return 2
        if nfiles == 0 or not current:
            print(f"error: read {nfiles} header(s) at HEAD — refusing to write "
                  f"an empty baseline", file=sys.stderr)
            return 2
    else:
        current, nfiles = build_snapshot()

    if args.update:
        SNAPSHOT.parent.mkdir(parents=True, exist_ok=True)
        header = (
            "# JCE public C ABI snapshot — generated by\n"
            "#   python tools/audit/check_abi_snapshot.py --update\n"
            "#\n"
            "# DO NOT hand-edit.  Regenerate in the SAME commit as the ABI\n"
            "# change so a reviewer sees the declaration and the snapshot diff\n"
            "# together.  Additions are safe for consumers; removals and\n"
            "# signature changes are not, and the gate fails on those.\n"
            f"# {len(current)} declarations.\n\n")
        # newline="\n": write_text() applies platform line-ending translation
        # on Windows, so an unmarked write_text() here rewrites this LF-policy
        # file (.gitattributes: * text=auto eol=lf) as CRLF on every --update.
        SNAPSHOT.write_text(header + "\n".join(current) + "\n", encoding="utf-8",
                            newline="\n")
        src = "HEAD (committed)" if args.committed else "the working tree"
        print(f"snapshot updated: {SNAPSHOT.relative_to(REPO_ROOT)} "
              f"({len(current)} declarations from {nfiles} header(s) in {src})")
        if args.committed:
            print("  provenance: read from the object database — uncommitted "
                  "work in the tree was ignored.")
        else:
            print_update_provenance()
        return 0

    if not SNAPSHOT.is_file():
        # This used to SKIP unconditionally, on the reasoning that docs/ is
        # gitignored so a fresh clone legitimately has no baseline.  That
        # reasoning expired: .gitignore re-includes contracts/ and the
        # baseline IS tracked (`git ls-files contracts/abi-snapshot.txt`
        # prints it).  So a missing baseline in a git work tree is a DELETION,
        # and skipping on it turns "somebody deleted the gate's memory" into a
        # green run.  Only a checkout with no git history — an export or a
        # release tarball — still earns the skip, and it says so loudly.
        if read_committed_snapshot("HEAD") is not None:
            print(f"abi-snapshot: FAILED — the tracked baseline "
                  f"{SNAPSHOT_REL} is missing from the working tree.")
            print("  It exists at HEAD, so this is a deletion, not a fresh")
            print("  clone.  Restore it with:")
            print(f"      git checkout -- {SNAPSHOT_REL}")
            return 1
        print(f"abi-snapshot: SKIPPED — no baseline at {SNAPSHOT_REL} and no "
              f"committed copy to compare against.")
        print("  NOTHING WAS CHECKED.  Create the baseline with:")
        print("      python tools/audit/check_abi_snapshot.py --update")
        return 0

    baseline = [ln for ln in SNAPSHOT.read_text(encoding="utf-8").splitlines()
                if ln.strip() and not ln.startswith("#")]

    d = diff_snapshot(baseline, current)
    b, c = d["keyed_before"], d["keyed_after"]

    if args.json:
        print(json.dumps({
            "mode": "worktree", "headers": nfiles,
            "declarations": len(current),
            "removed": [b[k] for k in d["removed"]],
            "changed": [{"key": k, "why": w, "before": b[k], "after": c[k]}
                        for k, w in d["changed"]],
            "appended": [{"key": k, "why": w} for k, w in d["appended"]],
            "moved": [{"key": k, "why": w} for k, w in d["moved"]],
            "added": [c[k] for k in d["added"]],
        }, indent=1))
        return 1 if d["fatal"] else 0

    if not d["fatal"]:
        pending = len(d["added"]) + len(d["appended"]) + len(d["moved"])
        extra = f", {pending} unrecorded compatible change(s)" if pending else ""
        print(f"abi-snapshot: OK — {len(current)} declarations from {nfiles} "
              f"header(s), no removals or incompatible changes{extra}")
        _listing(d["added"], lambda k: f"    + {c[k]}")
        for k, why in d["appended"]:
            print(f"    ~ APPENDED {k} — {why}")
        for k, why in d["moved"]:
            print(f"    ~ MOVED    {k} — {why}")
        if pending:
            print("  (compatible, but unrecorded; run --update to record them)")
        return 0

    print("abi-snapshot: FAILED — the public C ABI changed incompatibly")
    print()
    for k in d["removed"]:
        print(f"  REMOVED   {b[k]}")
    for k, why in d["changed"]:
        print(f"  CHANGED   {k} — {why}")
        print(f"      was:  {b[k]}")
        print(f"      now:  {c[k]}")
    # Compatible drift is never the reason for the failure, but it must still
    # be SHOWN here.  This branch used to skip additions entirely, so a diff
    # that changed one declaration and added ten reported only the change —
    # and the person reading it had no idea the other ten existed.  A report
    # that goes quiet the moment something else goes wrong is how a real
    # addition gets waved through with the unrelated break it rode in on.
    for k, why in d["appended"]:
        print(f"  APPENDED  {k} — {why}")
    for k, why in d["moved"]:
        print(f"  MOVED     {k} — {why}")
    _listing(d["added"], lambda k: f"  ADDED     {c[k]}")
    print()

    print(f"{len(d['removed'])} removal(s), {len(d['changed'])} incompatible "
          f"change(s), {len(d['appended'])} append(s), {len(d['moved'])} "
          f"move(s), {len(d['added'])} addition(s).")
    print()
    print(HOW_TO_FIX, end="")
    return 1


if __name__ == "__main__":
    sys.exit(main())
