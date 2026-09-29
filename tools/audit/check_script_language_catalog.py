#!/usr/bin/env python3
"""
check_script_language_catalog.py — the offline script-extension table must
cover every extension a backend claims at runtime.

WHAT PROBLEM THIS SOLVES
------------------------
There are, unavoidably, TWO answers to "what language is this file?":

  * the RUNTIME registry — jce_script_vm_register_extension(ext, language),
    called by each backend from its own register().  It is deliberately not a
    central list, so a sixth backend claims ".rb" without any engine file
    learning about it.  Its answer depends on which backends were LINKED.

  * the OFFLINE catalog — k_script_ext_table in
    engine/src/resource/jce_asset_ext.c.  The cooker, the bundle manifest,
    the editor's asset database and the editor's publication policy read it,
    and it must NOT depend on build options: a cooker with no Python linked
    still has to pack turret.py into a bundle a Python-enabled runtime loads.

They are different questions and both are needed.  What must never happen is
the runtime claiming an extension the offline table has never heard of,
because the whole failure is invisible until a PACKAGED build:

  * the bundle manifest labels the file "binary" / "binary.raw", so no report
    or tool can tell it is code;
  * the archive cooker compresses it as an opaque blob instead of putting it
    in the shared text dictionary;
  * the editor's asset database does not classify it as a script, so it never
    appears in the Script component's picker and cannot be attached;
  * the editor's publication policy may drop it from the shipped build
    entirely — and the editor keeps running it from loose files, so nothing
    is red until someone installs the packaged game.

WHAT COUNTS AS BREAKAGE
-----------------------
  UNKNOWN CLAIM      a jce_script_vm_register_extension() call whose
                     extension has no row in the offline catalog
  LANGUAGE MISMATCH  a claim whose language differs from the catalog's, so
                     the editor would name a different VM than the one that
                     will actually run the file
  ORPHAN LANGUAGE    a catalog language no backend implements (a typo in the
                     table promises a language nothing can ever run)

Each exits 1 and names the extension.

NOT breakage: a catalog row no backend claims YET.  That is the normal state
for a language whose backend has not wired its claim, and for a build with
the backend switched off — the catalog is the offline answer precisely
because it must outlive both.  Those are printed as notes.

Usage:
  python tools/audit/check_script_language_catalog.py
  # exit 0 = clean, 1 = violations, 2 = the catalog could not be parsed.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

CATALOG = REPO_ROOT / "engine/src/resource/jce_asset_ext.c"
VM_HEADER = REPO_ROOT / "engine/include/jce/middleware/script/jce_script_vm.h"

# Where a backend's register() and VM table can live.  scripting/ is the
# language-binding layer; the built-in Lua VM registers from the engine's own
# script middleware.  Both are READ ONLY here.
CLAIM_ROOTS = [
    REPO_ROOT / "scripting",
    REPO_ROOT / "engine/src/middleware/script",
]

SOURCE_SUFFIXES = {".c", ".cpp", ".cc", ".h", ".hpp"}

# { "ext": ("language", "representation", "FORM") }
CATALOG_ROW = re.compile(
    r'\{\s*"(?P<ext>[^"]+)"\s*,\s*'
    r'"(?P<language>[^"]+)"\s*,\s*'
    r'"(?P<representation>[^"]+)"\s*,\s*'
    r'(?P<form>JCEASSET_SCRIPT_FORM_[A-Z]+)\s*\}')

CLAIM_CALL = re.compile(
    r'jce_script_vm_register_extension\s*\(\s*'
    r'(?P<ext>"[^"]*"|[A-Z_][A-Z0-9_]*)\s*,\s*'
    r'(?P<language>"[^"]*"|[A-Z_][A-Z0-9_]*)\s*\)')

# A backend's own language name, from its JceScriptVM table: the member is
# the SECOND initialiser after struct_size (jce_script_vm.h pins the order).
VM_TABLE = re.compile(
    r'JceScriptVM\s+\w+\s*=\s*\{\s*[^,]+,\s*'
    r'(?P<language>"[^"]*"|[A-Z_][A-Z0-9_]*)')

DEFINE = re.compile(r'^\s*#\s*define\s+(?P<name>[A-Z_][A-Z0-9_]*)\s+"(?P<value>[^"]*)"',
                    re.MULTILINE)


# A C comment, and a C++ one.  See blank_comments().
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)

# A roster is plain `set(NAME "value")` by contract — JCEScriptEnable.cmake
# include()s it, so a roster that grew logic would break there first and loudly.
ROSTER_SET = re.compile(
    r'^\s*set\s*\(\s*(JCE_BACKEND_[A-Z_]+)\s+"?([^")]*)"?\s*\)', re.M)

# Directories under scripting/ that are not language backends.
NON_BACKEND_DIRS = {"c_abi", "cmake"}


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def blank_comments(text: str) -> str:
    """The text as the COMPILER reads it: comments replaced by blanks.

    WHY THIS EXISTS.  Every pattern below reads C source with a regex, and a
    regex that has not been told about comments reads a DIFFERENT program than
    the compiler does.  Measured, on the day scripting/c was added:

        static JceScriptVM g_c_vm = {
            0,                          /* struct_size: derived at register */
            JCE_SCRIPT_VM_C_LANGUAGE,

    VM_TABLE's whitespace run cannot cross that comment, so the backend's language was
    invisible, so `.jcec` was reported as an ORPHAN LANGUAGE -- "nothing can
    ever run it" -- on a tree where the language registered, claimed its
    extension and ran a script.  That is the SECOND time this checker has been
    structurally unable to see a real backend (the first is recorded in the
    two-pass note below), and both times the red landed on a correct change.

    NEWLINES ARE PRESERVED and every other character becomes a space, so byte
    offsets and line numbers are unchanged: DEFINE is anchored with MULTILINE
    and would silently stop matching if a multi-line comment collapsed the
    lines around it.

    A string literal containing "/*" would be mangled by this, which is why the
    result is used ONLY for matching and never for reporting -- every message
    quotes the ORIGINAL text.
    """
    def blank(m: "re.Match[str]") -> str:
        return "".join(c if c == chr(10) else " " for c in m.group(0))

    return COMMENT.sub(blank, text)


def catalog_table(text: str) -> dict[str, tuple[str, str, str]]:
    """The rows of k_script_ext_table, keyed by extension."""
    start = text.find("k_script_ext_table")
    if start < 0:
        return {}
    end = text.find("};", start)
    if end < 0:
        return {}
    rows: dict[str, tuple[str, str, str]] = {}
    for m in CATALOG_ROW.finditer(text[start:end]):
        rows[m.group("ext").lower()] = (m.group("language"),
                                        m.group("representation"),
                                        m.group("form"))
    return rows


def source_files() -> list[Path]:
    out: list[Path] = []
    for root in CLAIM_ROOTS:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                out.append(path)
    return out


def resolve(token: str, defines: dict[str, str]) -> str | None:
    """A string literal or a #define'd string macro; None when neither."""
    if token.startswith('"'):
        return token[1:-1]
    return defines.get(token)


def read_roster(path: Path) -> dict:
    """The set() values out of one jce_backend.cmake."""
    return {m.group(1): m.group(2).strip()
            for m in ROSTER_SET.finditer(read(path))}


def check_rosters(implemented: dict, claims: dict) -> list:
    """Cross-check every scripting/<lang>/jce_backend.cmake against the sources.

    `implemented` is language -> file, from the real JceScriptVM tables.
    `claims` is ext -> (language, file), from the real register_extension calls.
    Both are already computed above from the sources; the roster is the same
    two facts restated for the build layer, and nothing compared them.
    """
    problems: list[str] = []
    root = REPO_ROOT / "scripting"
    if not root.is_dir():
        return problems

    # ext claims, grouped by language, from the SOURCES
    claimed_by_lang: dict[str, set] = {}
    for ext, (language, _where) in claims.items():
        claimed_by_lang.setdefault(language, set()).add(ext)

    seen_languages = set()
    for d in sorted(p for p in root.iterdir() if p.is_dir()):
        if d.name in NON_BACKEND_DIRS or d.name.startswith("."):
            continue
        if not (d / "CMakeLists.txt").is_file():
            continue
        rel = "scripting/" + d.name
        roster_path = d / "jce_backend.cmake"
        if not roster_path.is_file():
            problems.append(
                f"NO ROSTER   {rel} is a backend directory with no "
                f"jce_backend.cmake. scripting/CMakeLists.txt globs and BUILDS "
                f"it, and jce_script_enable() globs rosters and will not find "
                f"it — so the target is built and NEVER LINKED, in the editor "
                f"and in every shipped game, with nothing erroring.")
            continue

        r = read_roster(roster_path)
        lang = r.get("JCE_BACKEND_LANGUAGE", "")
        for field in ("JCE_BACKEND_LANGUAGE", "JCE_BACKEND_VM_TARGET",
                      "JCE_BACKEND_REGISTER_FRAGMENT"):
            if not r.get(field):
                problems.append(
                    f"ROSTER FIELD   {rel}/jce_backend.cmake sets no {field}. "
                    f"jce_script_enable() refuses such a roster at a "
                    f"CONSUMER's configure; this says so at authoring time.")
        frag = r.get("JCE_BACKEND_REGISTER_FRAGMENT", "")
        if frag and not (d / frag).is_file():
            problems.append(
                f"NO FRAGMENT   {rel}/jce_backend.cmake names {frag}, which "
                f"does not exist. A backend with no register fragment is "
                f"LINKED AND NEVER REGISTERED: its extension resolves to "
                f"nothing and every entity carrying such a script is refused "
                f"at run time while the scene works perfectly.")
        if r.get("JCE_BACKEND_SDK_IMPORT_SIMPLE", "").upper() in ("ON", "TRUE", "1") \
           and not r.get("JCE_BACKEND_SDK_HEADER_DIR"):
            problems.append(
                f"SIMPLE WITHOUT HEADERS   {rel}/jce_backend.cmake is "
                f"SDK_IMPORT_SIMPLE and names no JCE_BACKEND_SDK_HEADER_DIR. "
                f"Its register include is emitted verbatim into a consumer's "
                f"shim, so the header not shipping is a compile error on the "
                f"CONSUMER's machine, not here.")
        if not lang:
            continue
        seen_languages.add(lang)
        if lang not in implemented:
            problems.append(
                f"ROSTER LANGUAGE   {rel}/jce_backend.cmake declares language "
                f"'{lang}', which no JceScriptVM table implements. The build "
                f"would link a VM under a name nothing registers.")
        declared = {e.strip().lstrip(".").lower()
                    for e in r.get("JCE_BACKEND_EXTENSIONS", "").split(";")
                    if e.strip()}
        actual = claimed_by_lang.get(lang, set())
        if declared and actual and declared != actual:
            problems.append(
                f"EXTENSION DRIFT   {rel}/jce_backend.cmake declares "
                f"{sorted('.' + e for e in declared)} but {lang} actually "
                f"claims {sorted('.' + e for e in actual)} at run time. The "
                f"roster is what an SDK ships and what tooling reads; a stale "
                f"one describes a language the binary does not have.")

    for language, where in sorted(implemented.items()):
        # ONLY languages implemented UNDER scripting/.  lua is built into the
        # engine and registered by it, lazily, on first use -- it has no
        # backend directory and must never have a roster.  ("!pin" is the
        # k_public_signature_pin sentinel in jce_script_vm.c, not a language at
        # all; the same predicate excludes it.)  Demanding a roster for those
        # made this rule permanently red the first time it ran, which is what
        # running it before wiring it is for.
        if not where.startswith("scripting/"):
            continue
        if language not in seen_languages:
            problems.append(
                f"UNROSTERED LANGUAGE   '{language}' has a JceScriptVM table "
                f"({where}) and no scripting/*/jce_backend.cmake declares it. "
                f"Nothing in the build layer can discover it, so no consumer "
                f"will ever link or register it.")
    return problems


def main() -> int:
    if not CATALOG.is_file():
        print(f"check_script_language_catalog: FAILED — {CATALOG} is missing",
              file=sys.stderr)
        return 2

    catalog = catalog_table(blank_comments(read(CATALOG)))
    if not catalog:
        print("check_script_language_catalog: FAILED — could not parse "
              f"k_script_ext_table in {CATALOG.relative_to(REPO_ROOT)}. The "
              "table shape changed; update this checker rather than deleting "
              "it, or the offline/runtime split goes unguarded.",
              file=sys.stderr)
        return 2

    defines: dict[str, str] = {}
    if VM_HEADER.is_file():
        for m in DEFINE.finditer(blank_comments(read(VM_HEADER))):
            defines[m.group("name")] = m.group("value")

    claims: dict[str, tuple[str, str]] = {}     # ext -> (language, where)
    implemented: dict[str, str] = {}            # language -> where
    unresolved: list[str] = []

    # TWO PASSES, because a backend's own header is not the file its VM table
    # lives in.  scripting/cpp spells its language once, as
    # JCE_SCRIPT_VM_CPP_LANGUAGE in jce_script_vm_cpp.h, and uses it in
    # jce_script_vm_cpp.c — so a per-file define table could not resolve it and
    # this checker silently did not know the cpp backend EXISTED.  It reported
    # three implemented languages while four were built, and the ORPHAN
    # LANGUAGE check for "cpp" was structurally incapable of passing: adding
    # the catalog row for it turned the checker red on a correct change.
    #
    # A macro named in one scanned file and defined in another therefore
    # resolves through this pool.  Local definitions still WIN, so a file that
    # redefines a name for itself is read the way the compiler reads it.
    texts: dict[Path, str] = {p: blank_comments(read(p)) for p in source_files()}
    pool: dict[str, str] = dict(defines)
    for text in texts.values():
        for m in DEFINE.finditer(text):
            pool.setdefault(m.group("name"), m.group("value"))

    for path, text in texts.items():
        rel = str(path.relative_to(REPO_ROOT)).replace("\\", "/")
        local = dict(pool)
        for m in DEFINE.finditer(text):
            local[m.group("name")] = m.group("value")

        for m in VM_TABLE.finditer(text):
            language = resolve(m.group("language"), local)
            if language:
                implemented.setdefault(language, rel)

        for m in CLAIM_CALL.finditer(text):
            ext = resolve(m.group("ext"), local)
            language = resolve(m.group("language"), local)
            if ext is None or language is None:
                # A claim assembled from a variable cannot be checked
                # statically.  Report it rather than passing silently: an
                # unreadable claim is exactly how one would slip past.
                unresolved.append(f"{rel}: {m.group(0)}")
                continue
            ext = ext.lstrip(".").lower()
            if not ext:
                continue
            claims.setdefault(ext, (language, rel))

    failures: list[str] = []

    for ext, (language, where) in sorted(claims.items()):
        row = catalog.get(ext)
        if row is None:
            failures.append(
                f"UNKNOWN CLAIM   .{ext} -> '{language}' claimed by {where}, "
                f"but there is no row for it in "
                f"{CATALOG.relative_to(REPO_ROOT)}. Until there is, the "
                f"cooker labels the file 'binary', the editor will not offer "
                f"it in the Script picker, and a packaged build may ship "
                f"without it.")
        elif row[0] != language:
            failures.append(
                f"LANGUAGE MISMATCH   .{ext} is '{row[0]}' in the catalog but "
                f"claimed for '{language}' by {where}. The editor would name "
                f"one VM and the runtime would use another.")

    for ext, (language, _rep, _form) in sorted(catalog.items()):
        if language not in implemented:
            failures.append(
                f"ORPHAN LANGUAGE   .{ext} names language '{language}', which "
                f"no JceScriptVM table in "
                f"{', '.join(str(r.relative_to(REPO_ROOT)) for r in CLAIM_ROOTS)}"
                f" implements. Nothing can ever run it.")

    failures.extend(check_rosters(implemented, claims))

    unclaimed = sorted(e for e in catalog if e not in claims)
    if unclaimed:
        print("check_script_language_catalog: note — catalog rows no backend "
              "claims at runtime yet (expected while a backend has not wired "
              "its jce_script_vm_register_extension call, and always for a "
              "build with that backend off): "
              + ", ".join("." + e for e in unclaimed))
    if unresolved:
        print("check_script_language_catalog: note — claims that are not "
              "string literals and could not be checked statically:")
        for line in unresolved:
            print("    " + line)

    if failures:
        print("\ncheck_script_language_catalog: FAILED — "
              f"{len(failures)} problem(s)", file=sys.stderr)
        for line in failures:
            print("    " + line, file=sys.stderr)
        return 1

    rosters = len(list((REPO_ROOT / "scripting").glob("*/jce_backend.cmake")))
    print(f"check_script_language_catalog: OK — {len(catalog)} catalog "
          f"row(s), {len(claims)} runtime claim(s), "
          f"{len(implemented)} backend language(s) implemented, "
          f"{rosters} roster(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
