#!/usr/bin/env python3
"""Reject source-writing Conan callbacks in the public build pipeline."""
import ast
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[2]
ALLOWED = {"pre_generate", "post_package_id", "post_package_info"}

def violations(source):
    tree = ast.parse(source)
    errors = []
    for node in ast.walk(tree):
        if isinstance(node, (ast.Import, ast.ImportFrom)):
            errors.append("configuration hooks cannot import filesystem/code loaders")
        if isinstance(node, ast.Attribute) and node.attr in {"source_folder", "package_folder", "build_folder", "generators_folder"}:
            errors.append("hook reaches dependency files")
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id in {"open", "exec", "eval", "__import__", "getattr", "setattr"}:
            errors.append("hook can write files or bypass source guard")
        if isinstance(node, ast.FunctionDef) and not node.name.startswith("_") and node.name not in ALLOWED:
            errors.append("unapproved callback: " + node.name)
    return errors

def main():
    if "--self-check" in sys.argv:
        for source in ('def post_source(c): pass', 'def pre_generate(c): open("x", "w")', 'import shutil', 'def pre_generate(c): c.source_folder', 'def pre_generate(c): getattr(c,"source_folder")'):
            assert violations(source), source
        assert not violations('def pre_generate(c): c.conf.define("x", 1)')
        print("Conan source policy self-check: PASS (5 negative controls)")
    errors = []
    for file in (ROOT / "conan/hooks").glob("hook_*.py"):
        errors.extend(str(file.relative_to(ROOT)) + ": " + e for e in violations(file.read_text(encoding="utf-8")))
    for e in errors: print("FAIL: " + e)
    print("Conan source policy: " + ("FAIL" if errors else "PASS"))
    return bool(errors)
if __name__ == "__main__": raise SystemExit(main())
