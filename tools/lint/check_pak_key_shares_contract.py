#!/usr/bin/env python3
"""check_pak_key_shares_contract.py — the embedded asset-key TU has THREE
producers/consumers, and they must agree byte-for-byte on two symbols.

An encrypted `dist` executable links a generated translation unit that carries
two XOR shares of the project asset key:

    const unsigned char jce_embedded_pak_key_shares[64];  /* mask || key^mask */
    const int           jce_embedded_pak_key_present;     /* 1 = key shipped  */

Three places write or depend on that exact shape:

  1. `engine/src/application/jce_pak_key_default.c` — the zeroed default,
     linked as a standalone archive so ordinary symbol resolution lets a
     generated TU win.  `present = 0` there means "no key shipped".
  2. `editor/src/core/jce_pak_key.cpp` — `jce_pak_key_write_shares_c`, the
     editor's emitter.
  3. `tools/build/jce.py` — `_write_pak_key_shares_c`, the CLI emitter added
     2026-09-14 so a machine without the editor can ship `--variant dist`.

**Why this gate exists.** Rename either symbol, or change the 64, in any ONE
of the three and nothing fails loudly: the link still succeeds because the
zeroed default resolves the externs, and the shipped executable then simply
cannot decrypt its own archive.  The failure surfaces as a runtime "assets
missing" at customer sites, not as a build error here.

══ WHY IT DOES NOT JUST GREP FOR THE NAMES ════════════════════════════════

It used to, and a mutation control caught it being blind.  `tools/build/jce.py`
names both symbols TWICE: once in `_write_pak_key_shares_c`'s docstring, which
spells out the contract for a reader, and once in the C source the function
actually emits.  `"name" in text` cannot tell those apart, so renaming the
EMITTED one — the only one that reaches a compiler — left the gate green
because the docstring still matched.  MEASURED: three mutations, two caught,
that one silently accepted.

`"<identifier>" in source` being satisfied by prose about the identifier is a
failure this repository has now paid for more than once.  So each producer is
checked in the form it actually produces:

  * the engine default DEFINES the symbols   → match C code with comments removed
  * the editor emitter WRITES them as text   → match inside its string literals
  * the CLI emitter WRITES them as text      → RUN it and read what came out

Running the CLI emitter costs one import and one temp file, and it buys a
check the textual version could not make at all: that the two 32-byte shares
really XOR back to the key.  That is only the CLI emitter's maths; the
editor's own round-trip belongs to the editor's tests.
"""
import importlib.util
import pathlib
import re
import secrets
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]

SYM_SHARES = "jce_embedded_pak_key_shares"
SYM_PRESENT = "jce_embedded_pak_key_present"

# The declaration as a C compiler sees it.  The 64 is in here on purpose: it
# is the part that silently breaks decryption when it drifts (two 32-byte
# shares), and it is the part a careless edit is most likely to touch.
DECL_SHARES = re.compile(
    r"const\s+unsigned\s+char\s+" + SYM_SHARES + r"\s*\[\s*64\s*\]")
DECL_PRESENT = re.compile(r"const\s+int\s+" + SYM_PRESENT + r"\b")

ENGINE_DEFAULT = "engine/src/application/jce_pak_key_default.c"
EDITOR_EMITTER = "editor/src/core/jce_pak_key.cpp"
CLI_EMITTER = "tools/build/jce.py"


def strip_c_comments(src: str) -> str:
    """Remove // and /* */ comments, leaving string and char literals intact.

    Written out rather than regexed because the whole point of this gate is
    telling code from prose about code, and a regex that ate a `//` inside a
    string literal would get that backwards.
    """
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                out.append(src[i])
                if src[i] == "\\":
                    if i + 1 < n:
                        out.append(src[i + 1])
                        i += 2
                        continue
                elif src[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


STRING_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def emitted_text(src: str) -> str:
    """Concatenate every double-quoted literal in already-decommented C++.

    Adjacent literals concatenate in C++ exactly as they do here, so a
    declaration split across two string chunks still reads as one line —
    which is how both emitters actually write it.
    """
    return "".join(m.group(1) for m in STRING_LITERAL.finditer(src))


def check_file(failures, label, rel, transform):
    path = ROOT / rel
    if not path.is_file():
        failures.append("%s: missing %s" % (label, rel))
        return
    text = transform(path.read_text(encoding="utf-8", errors="replace"))
    if not DECL_SHARES.search(text):
        failures.append(
            "%s (%s): no `const unsigned char %s[64]` — the symbol was "
            "renamed, or its length is no longer 64"
            % (label, rel, SYM_SHARES))
    if not DECL_PRESENT.search(text):
        failures.append(
            "%s (%s): no `const int %s`" % (label, rel, SYM_PRESENT))


def check_cli_emitter(failures):
    """Run tools/build/jce.py's emitter and read the C it produced."""
    label, rel = "CLI emitter", CLI_EMITTER
    path = ROOT / rel
    if not path.is_file():
        failures.append("%s: missing %s" % (label, rel))
        return
    try:
        spec = importlib.util.spec_from_file_location("_jce_cli_probe", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except Exception as exc:                      # noqa: BLE001 — reported
        failures.append("%s (%s): will not import: %r" % (label, rel, exc))
        return
    fn = getattr(mod, "_write_pak_key_shares_c", None)
    if fn is None:
        failures.append(
            "%s (%s): no _write_pak_key_shares_c. The CLI can no longer ship "
            "an encrypted dist without the editor, which is the whole reason "
            "it grew an emitter." % (label, rel))
        return
    if getattr(mod, "DRY_RUN", False):
        failures.append(
            "%s (%s): module-level DRY_RUN is true on import, so the emitter "
            "returns without writing and this check cannot see its output."
            % (label, rel))
        return

    key = secrets.token_bytes(32)
    with tempfile.TemporaryDirectory() as td:
        out = pathlib.Path(td) / "pak_key_shares.c"
        try:
            fn(out, key)
        except Exception as exc:                  # noqa: BLE001 — reported
            failures.append("%s (%s): emitter raised: %r" % (label, rel, exc))
            return
        if not out.is_file():
            failures.append(
                "%s (%s): emitter wrote no file" % (label, rel))
            return
        src = out.read_text(encoding="utf-8", errors="replace")

    if not DECL_SHARES.search(strip_c_comments(src)):
        failures.append(
            "%s (%s): the C it EMITS has no `const unsigned char %s[64]`. "
            "Its docstring may still say so; the compiler reads the output."
            % (label, rel, SYM_SHARES))
    if not DECL_PRESENT.search(strip_c_comments(src)):
        failures.append(
            "%s (%s): the C it EMITS has no `const int %s`"
            % (label, rel, SYM_PRESENT))

    # The shares must XOR back to the key.  Free to check now that the
    # emitter has actually been run, and it is the one way the TU can be
    # shaped correctly and still decrypt nothing.
    body = src.split("{", 1)[-1].split("}", 1)[0]
    bytes_out = [int(b, 16) for b in re.findall(r"0[xX]([0-9a-fA-F]{2})", body)]
    if len(bytes_out) != 64:
        failures.append(
            "%s (%s): emitted %d share bytes, not 64"
            % (label, rel, len(bytes_out)))
        return
    recovered = bytes(a ^ b for a, b in
                      zip(bytes_out[:32], bytes_out[32:]))
    if recovered != key:
        failures.append(
            "%s (%s): the two emitted shares do not XOR back to the key, so "
            "an executable linking this TU decrypts nothing" % (label, rel))


def main() -> int:
    failures = []
    check_file(failures, "engine default", ENGINE_DEFAULT, strip_c_comments)
    check_file(failures, "editor emitter", EDITOR_EMITTER,
               lambda s: emitted_text(strip_c_comments(s)))
    check_cli_emitter(failures)

    if failures:
        print("check_pak_key_shares_contract: FAIL - "
              "%d finding(s):" % len(failures))
        for f in failures:
            print("  " + f)
        return 1
    print("check_pak_key_shares_contract: OK - engine default defines both "
          "symbols, the editor emitter writes both, and the CLI emitter was "
          "run: its output declares both and its shares XOR back to the key.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
