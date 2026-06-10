#!/usr/bin/env python3
"""shader_lint.py - cross-backend portability lint for bgfx .sc/.sh shaders.

bgfx compiles ONE shader source to HLSL, GLSL, ESSL, SPIR-V and Metal. A
small set of constructs COMPILES on every profile but means something
different per profile, producing renders that are correct on one backend
and silently wrong on another. This lint bans exactly those constructs.

Rules (high-signal only; every rule has a documented real-world failure):

  E001  multi-argument raw matrix constructor: mat2/mat3/mat4(a, b, ...)
        HLSL float3x3(a,b,c) packs the args as ROWS; GLSL mat3(a,b,c)
        packs them as COLUMNS. The same source builds TRANSPOSED matrices
        per backend. Use mtxFromCols()/mtxFromRows() from bgfx_shader.sh.
        (Real bug: fs_pbr.sc TBN built with mat3(T,B,N) applied the
        inverse basis on OpenGL/WASM — every light pool was cut in half
        at the light's own axis because N.L flipped sign. 2026-06-10.)
        Single-argument constructors (mat3(someMat4) truncation,
        mat3(1.0) diagonal) are portable and allowed.

  E002  matrix combined with the '*' operator.
        GLSL '*' on matrices is the linear-algebra product; HLSL '*' is
        COMPONENT-WISE (mul() is the product). Both compile; results
        diverge. Always use mul(a, b).

Suppression (sparingly, with a justification comment nearby):
        ... // shader-lint: allow

Usage:
    python tools/shader_lint.py <dir-or-file> [more...] [--quiet]
Exit code 0 = clean, 1 = findings, 2 = usage error.
"""

import os
import re
import sys

SHADER_EXTS = (".sc", ".sh")
SUPPRESS_TAG = "shader-lint: allow"

# mat ctor with at least one TOP-LEVEL comma inside the parens.
MAT_CTOR_RE = re.compile(r"\bmat[234]\s*\(")
# matrix variable declarations: 'mat3 name' / 'uniform mat4 name[4]'.
MAT_DECL_RE = re.compile(r"\bmat[234]\s+([A-Za-z_]\w*)")


def strip_comments(text):
    """Replace comment bodies with spaces, preserving line structure."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            if j < 0:
                j = n
            # Keep the comment text itself for suppression detection by
            # the caller; here we only strip for ANALYSIS, so blank it.
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            seg = text[i:j]
            out.append("".join("\n" if ch == "\n" else " " for ch in seg))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def top_level_commas(text, open_paren_idx):
    """Count commas at depth 1 inside the paren starting at open_paren_idx."""
    depth = 0
    commas = 0
    for i in range(open_paren_idx, len(text)):
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return commas
        elif c == "," and depth == 1:
            commas += 1
    return commas  # unbalanced; treat as-is


def lint_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        raw = f.read()

    code = strip_comments(raw)
    raw_lines = raw.splitlines()
    findings = []

    def suppressed(line_no):
        line = raw_lines[line_no - 1] if line_no - 1 < len(raw_lines) else ""
        return SUPPRESS_TAG in line

    def line_of(offset):
        return code.count("\n", 0, offset) + 1

    # E001: multi-arg raw matrix constructors.
    for m in MAT_CTOR_RE.finditer(code):
        # Skip declarations ('mat3 TBN = ...' does not match: regex needs
        # '(' right after the type) and mtxFromCols/Rows calls (different
        # identifier). A match here IS a constructor call.
        if top_level_commas(code, m.end() - 1) >= 1:
            ln = line_of(m.start())
            if not suppressed(ln):
                findings.append((ln, "E001",
                    "raw multi-arg %s constructor: HLSL packs args as ROWS, "
                    "GLSL as COLUMNS (transposed result per backend) - use "
                    "mtxFromCols()/mtxFromRows() + mul()"
                    % m.group(0).rstrip("(").strip()))

    # E002: matrix identifiers combined with '*'.
    mat_vars = set(MAT_DECL_RE.findall(code))
    # Uniform matrices declared as arrays also count: 'uniform mat4 u_x[4]'.
    if mat_vars:
        star_re = re.compile(
            r"(?:\b(%s)\s*\*)|(?:\*\s*(%s)\b)"
            % ("|".join(map(re.escape, mat_vars)),
               "|".join(map(re.escape, mat_vars))))
        for m in star_re.finditer(code):
            ln = line_of(m.start())
            if not suppressed(ln):
                name = m.group(1) or m.group(2)
                findings.append((ln, "E002",
                    "matrix '%s' used with '*': GLSL multiplies, HLSL is "
                    "COMPONENT-WISE - use mul()" % name))

    return findings


def collect_files(args):
    files = []
    for a in args:
        if os.path.isdir(a):
            for root, _dirs, names in os.walk(a):
                for n in names:
                    if n.endswith(SHADER_EXTS):
                        files.append(os.path.join(root, n))
        elif os.path.isfile(a):
            files.append(a)
        else:
            print("shader_lint: no such path: %s" % a, file=sys.stderr)
            sys.exit(2)
    return sorted(set(files))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    quiet = "--quiet" in sys.argv
    if not args:
        print(__doc__)
        sys.exit(2)

    files = collect_files(args)
    total = 0
    for path in files:
        for ln, code_id, msg in lint_file(path):
            total += 1
            print("%s:%d: error %s: %s" % (path, ln, code_id, msg))

    if not quiet:
        print("shader_lint: %d file(s), %d finding(s)" % (len(files), total))
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
