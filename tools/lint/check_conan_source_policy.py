#!/usr/bin/env python3
"""Reject source-writing Conan callbacks in the public build pipeline."""
import ast
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[2]
ALLOWED = {"pre_generate", "post_package_id", "post_package_info"}

def _is_pure_shlex_import(node):
    return (isinstance(node, ast.Import) and len(node.names) == 1
            and node.names[0].name == "shlex" and node.names[0].asname is None)


def _is_string_input(node):
    return (isinstance(node, ast.Constant) and isinstance(node.value, str)
            or isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Constant) and isinstance(node.func.value.value, str)
            and node.func.attr == "join" and len(node.args) == 1 and not node.keywords)


def _is_pure_split_use(node, parents):
    attribute = parents.get(node)
    call = parents.get(attribute)
    return (isinstance(node.ctx, ast.Load) and isinstance(attribute, ast.Attribute)
            and attribute.attr == "split" and isinstance(call, ast.Call) and call.func is attribute
            and 1 <= len(call.args) <= 3 and _is_string_input(call.args[0])
            and all(isinstance(arg, ast.Constant) and isinstance(arg.value, bool) for arg in call.args[1:])
            and all(keyword.arg in {"comments", "posix"} and isinstance(keyword.value, ast.Constant)
                    and isinstance(keyword.value.value, bool) for keyword in call.keywords))



def _is_pure_data_import(node):
    return (isinstance(node, ast.Import) and len(node.names) == 1
            and node.names[0].name in {"json", "hashlib"} and node.names[0].asname is None)


def _is_canonical_json_use(node, parents):
    attribute = parents.get(node)
    call = parents.get(attribute)
    if not (isinstance(node.ctx, ast.Load) and isinstance(attribute, ast.Attribute)
            and attribute.attr == "dumps" and isinstance(call, ast.Call) and call.func is attribute
            and len(call.args) == 1 and len(call.keywords) == 4):
        return False
    options = {keyword.arg: keyword.value for keyword in call.keywords}
    if set(options) != {"sort_keys", "ensure_ascii", "allow_nan", "separators"}:
        return False
    for key, expected in (("sort_keys", True), ("ensure_ascii", True), ("allow_nan", False)):
        if not isinstance(options[key], ast.Constant) or options[key].value is not expected:
            return False
    separators = options["separators"]
    return (isinstance(separators, ast.Tuple) and len(separators.elts) == 2
            and all(isinstance(item, ast.Constant) and item.value == expected
                    for item, expected in zip(separators.elts, (",", ":"))))


def _is_pure_sha256_use(node, parents):
    attribute = parents.get(node)
    call = parents.get(attribute)
    if not (isinstance(node.ctx, ast.Load) and isinstance(attribute, ast.Attribute)
            and attribute.attr == "sha256" and isinstance(call, ast.Call) and call.func is attribute
            and len(call.args) == 1 and not call.keywords):
        return False
    value = call.args[0]
    if isinstance(value, ast.Constant) and isinstance(value.value, bytes):
        return True
    return (isinstance(value, ast.Call) and isinstance(value.func, ast.Attribute)
            and isinstance(value.func.value, ast.Name) and value.func.attr == "encode"
            and len(value.args) == 1 and not value.keywords
            and isinstance(value.args[0], ast.Constant) and value.args[0].value == "ascii")


def violations(source):
    tree = ast.parse(source)
    errors = []
    parents = {child: parent for parent in ast.walk(tree) for child in ast.iter_child_nodes(parent)}
    for node in ast.walk(tree):
        if isinstance(node, (ast.Import, ast.ImportFrom)):
            if not _is_pure_shlex_import(node):
                if not _is_pure_data_import(node):
                    errors.append("configuration hooks cannot import filesystem/code loaders")
        if isinstance(node, ast.Name) and node.id == "shlex" and not _is_pure_split_use(node, parents):
            errors.append("shlex is restricted to direct pure string split calls")
        if isinstance(node, ast.Name) and node.id == "json" and not _is_canonical_json_use(node, parents):
            errors.append("json is restricted to canonical dumps calls")
        if isinstance(node, ast.Name) and node.id == "hashlib" and not _is_pure_sha256_use(node, parents):
            errors.append("hashlib is restricted to direct pure sha256 calls")
        if isinstance(node, ast.arg) and node.arg in {"shlex", "json", "hashlib"}:
            errors.append("pure configuration imports cannot be shadowed")
        if isinstance(node, ast.Attribute) and node.attr in {"source_folder", "package_folder", "build_folder", "generators_folder"}:
            errors.append("hook reaches dependency files")
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id in {"open", "exec", "eval", "__import__", "getattr", "setattr", "globals", "locals", "vars"}:
            errors.append("hook can write files or bypass source guard")
        if isinstance(node, ast.Attribute) and node.attr.startswith("__"):
            errors.append("hook can bypass source guard through introspection")
        if isinstance(node, ast.FunctionDef) and not node.name.startswith("_") and node.name not in ALLOWED:
            errors.append("unapproved callback: " + node.name)
    return errors

def main():
    if "--self-check" in sys.argv:
        negative = ('def post_source(c): pass', 'def pre_generate(c): open("x", "w")',
                    'import shutil', 'from pathlib import Path', 'import os',
                    'def pre_generate(c): c.source_folder', 'def pre_generate(c): getattr(c,"source_folder")',
                    'from shlex import split', 'import shlex as parser', 'import shlex, os',
                    'import shlex\nshlex.os.unlink("x")', 'import shlex\nshlex.sourcehook("x")',
                    'import shlex\nparser = shlex.split', 'import shlex\nparser = shlex',
                    'import shlex\nshlex = parser', 'import shlex\nshlex.split(None)',
                    'import shlex\nshlex.split()', 'import shlex\nshlex.split(c.stream)',
                    'import shlex\nshlex.split("x", **options)',
                    'import shlex\nglobals()["shlex"].os.unlink("x")',
                    'import shlex\n(lambda: None).__globals__["shlex"].os.unlink("x")',
                    'import shlex\ndef pre_generate(shlex): shlex.split("x")')
        negative += (
            'import json\njson.dump(data, stream)',
            'import json\njson.load(stream)',
            'import json\njson.loads(text)',
            'import json as encoder',
            'from json import dumps',
            'import json\nencoder = json',
            'import json\njson.dumps(data)',
            'import json\njson.dumps(data, sort_keys=True, ensure_ascii=True, allow_nan=False, separators=(",", ":"), default=loader)',
            'import json\ndef pre_generate(json): pass',
            'import hashlib\nhashlib.file_digest(stream, "sha256")',
            'import hashlib\nhashlib.new("sha256")',
            'import hashlib\nhashlib.sha256(stream.read())',
            'import hashlib\nhashlib.sha256(text.encode("utf-8"))',
            'from hashlib import sha256',
            'import hashlib as digest',
            'import hashlib\nloader = hashlib',
            'import hashlib\ndef pre_generate(hashlib): pass',
            'import json\njson.__dict__["load"](stream)',
        )
        for source in negative:
            assert violations(source), source
        assert not violations('def pre_generate(c): c.conf.define("x", 1)')
        assert not violations('import shlex\ndef pre_generate(c): shlex.split("x", posix=False)')
        assert not violations('import shlex\ndef pre_generate(c): shlex.split(" ".join(flags), posix=False)')
        assert not violations('import json\njson.dumps(data, sort_keys=True, ensure_ascii=True, allow_nan=False, separators=(",", ":"))')
        assert not violations('import hashlib\nhashlib.sha256(canonical.encode("ascii")).hexdigest()')
        print(f"Conan source policy self-check: PASS ({len(negative)} negative controls)")
    errors = []
    for file in (ROOT / "conan/hooks").glob("hook_*.py"):
        errors.extend(str(file.relative_to(ROOT)) + ": " + e for e in violations(file.read_text(encoding="utf-8")))
    for e in errors: print("FAIL: " + e)
    print("Conan source policy: " + ("FAIL" if errors else "PASS"))
    return bool(errors)
if __name__ == "__main__": raise SystemExit(main())
