#!/usr/bin/env python3
"""Check the bgfx C view-name length contract in first-party source.

Pinned bgfx/c99/bgfx.h declares three arguments and INT32_MAX for a
NUL-terminated name. A negative literal is not that sentinel.

This is a finite lexical guard, not C/C++ evaluation or GPU validation.
Comments, ordinary/character/raw strings are masked; (), [], {} are balanced.
Only an int32_t C cast is unwrapped; other casts remain unknown.
Macro expansion, preprocessor conditions, integer dataflow and C++ template
commas are not evaluated. Nonliteral lengths and ambiguous template argument
lists are reported UNRESOLVED, never certified safe or rejected as literals.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

try:
    from .lint_git_files import working_tree
except ImportError:
    from lint_git_files import working_tree

REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOTS = ("engine/src/", "editor/src/", "tools/")
SOURCE_SUFFIXES = (".c", ".h", ".cpp", ".hpp", ".cc", ".cxx")
CALL = re.compile(r"(?<![A-Za-z0-9_])bgfx_set_view_name\s*\(")
# Preserve source offsets/argument slots; quoted commas are not separators.
TOKENS = re.compile(
    r'(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
    r'|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"'
    r"|(?:u|U|L)?'(?:\\.|[^'\\])*'"
    r'|//(?:\\\r?\n|[^\r\n])*|/\*.*?(?:\*/|\Z)', re.S)
INTEGER = re.compile(r"^([+-]?)\s*(0[xX][0-9a-fA-F]+|0[bB][01]+|0[0-7]*|[1-9][0-9]*)"
                     r"(?:[uU](?:[lL]{1,2})?|[lL]{1,2}[uU]?)?$")
CAST = re.compile(r"^\(\s*int32_t\s*\)\s*")
INT32_MAX_VALUE = 2147483647


def mask_prose(source: str) -> str:
    def replace(match):
        token = match.group(0)
        blank = re.sub(r"[^\r\n]", " ", token)
        return blank if token.startswith(("//", "/*")) else "L" + blank[1:]
    return TOKENS.sub(replace, source)


def arguments(code: str, opening: int):
    """Return lexical argument expressions, or None for unmatched delimiters."""
    closing = {"(": ")", "[": "]", "{": "}"}
    stack, result, start = [")"], [], opening + 1
    for pos in range(start, len(code)):
        char = code[pos]
        if char in closing:
            stack.append(closing[char])
        elif char in ")]}":
            if char != stack[-1]:
                return None
            stack.pop()
            if not stack:
                result.append(code[start:pos].strip())
                return result
        elif char == "," and len(stack) == 1:
            result.append(code[start:pos].strip())
            start = pos + 1
    return None


def unwrap(expression: str) -> str:
    expression = expression.strip()
    while True:
        if expression.startswith("("):
            args = arguments(expression, 0)
            # Only remove a pair enclosing the entire expression.
            depth = 0
            end = None
            for pos, char in enumerate(expression):
                if char == "(": depth += 1
                elif char == ")":
                    depth -= 1
                    if depth == 0:
                        end = pos
                        break
            if args is not None and end == len(expression) - 1:
                expression = expression[1:-1].strip()
                continue
        cast = CAST.match(expression)
        if cast:
            expression = expression[cast.end():].strip()
            continue
        return expression


def literal_length(expression: str):
    """Known integer literal / INT32_MAX, otherwise None (not evaluated)."""
    expression = unwrap(expression)
    if expression == "INT32_MAX":
        return INT32_MAX_VALUE
    match = INTEGER.fullmatch(expression)
    if not match:
        return None
    sign, spelling = match.groups()
    base = 16 if spelling.lower().startswith("0x") else 2 if spelling.lower().startswith("0b") else 8 if spelling.startswith("0") else 10
    value = int(spelling, base)
    return -value if sign == "-" else value


def scan_source(source: str) -> dict:
    code = mask_prose(source)
    result = {"calls": 0, "literal_checked": 0, "findings": [], "unresolved": []}
    for match in CALL.finditer(code):
        line = code.count("\n", 0, match.start()) + 1
        result["calls"] += 1
        args = arguments(code, match.end() - 1)
        if args is None:
            result["findings"].append((line, "unbalanced view-name call"))
            continue
        if len(args) != 3:
            # Angle brackets require C++ syntax/evaluation we do not own.
            if len(args) > 3 and any("<" in arg or ">" in arg for arg in args):
                result["unresolved"].append((line, "argument count has template/comparison ambiguity"))
            else:
                result["findings"].append((line, f"expected 3 arguments, found {len(args)}"))
            continue
        if not args[2]:
            result["findings"].append((line, "missing length argument"))
            continue
        value = literal_length(args[2])
        if value is None:
            result["unresolved"].append((line, "nonliteral length expression"))
        elif value < 0 or value > INT32_MAX_VALUE:
            result["findings"].append((line, "length literal outside [0, INT32_MAX]; NUL sentinel is INT32_MAX"))
        else:
            result["literal_checked"] += 1
    return result


def first_party_sources(paths):
    return sorted(rel for rel in paths
                  if rel.startswith(SOURCE_ROOTS) and rel.endswith(SOURCE_SUFFIXES)
                  and not any(part in {"third_party", "external", "vendor"}
                              for part in Path(rel).parts))


def main() -> int:
    sources = first_party_sources(working_tree())
    if not sources:
        print("bgfx_view_name_length: FAIL source inventory unavailable/empty", file=sys.stderr)
        return 1
    calls = checked = unresolved = 0
    findings = []
    try:
        for rel in sources:
            result = scan_source((REPO_ROOT / rel).read_text(encoding="utf-8"))
            calls += result["calls"]
            checked += result["literal_checked"]
            unresolved += len(result["unresolved"])
            findings.extend((rel, line, why) for line, why in result["findings"])
            for line, why in result["unresolved"]:
                print(f"bgfx_view_name_length: UNRESOLVED {rel}:{line}: {why}")
    except (OSError, UnicodeError) as error:
        print(f"bgfx_view_name_length: FAIL source read: {error}", file=sys.stderr)
        return 1
    for rel, line, why in findings:
        print(f"bgfx_view_name_length: FAIL {rel}:{line}: {why}", file=sys.stderr)
    if not calls:
        print("bgfx_view_name_length: FAIL no API occurrences; inventory cannot prove coverage", file=sys.stderr)
        return 1
    print(f"bgfx_view_name_length: {'FAIL' if findings else 'OK'} ({len(sources)} sources; "
          f"{calls} occurrences; {checked} literal/sentinel checked; {unresolved} UNRESOLVED; "
          "static arity/literal scope only)")
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
