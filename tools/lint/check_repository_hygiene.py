#!/usr/bin/env python3
"""Check Git text/ignore policy; optionally synchronize owned workspace text.

Default lint checks tracked and visible new files, including LF index storage.
--all-owned also scans ignored local text outside generated/protected roots.
--fix changes newline bytes only; never stages files or edits upstream sources.
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
import os
import stat
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GENERATED = {"build", "dist", "out", "build_standalone", "node_modules",
             "__pycache__", "bin", "obj", "artifacts", "_cooked",
             ".vs", ".scannerwork", ".binsize_history", ".playwright-mcp", ".jce"}
PROTECTED = {"third_party", ".git", "ZKM-VMP"}
ATTR_PROBES = {
    "engine/src/jce-policy-probe.c": ("auto", "lf"),
    "scripts/jce-policy-probe.py": ("auto", "lf"),
    "scripts/jce-policy-probe.sh": ("auto", "lf"),
    ".github/workflows/jce-policy-probe.yml": ("auto", "lf"),
    "scripts/windows/jce-policy-probe.bat": ("set", "crlf"),
    "scripts/windows/jce-policy-probe.cmd": ("set", "crlf"),
    "scripts/windows/jce-policy-probe.BAT": ("set", "crlf"),
    "scripts/windows/jce-policy-probe.CMD": ("set", "crlf"),
    "resources/jce-policy-probe.png": ("unset", "lf"),
    "resources/jce-policy-probe.flac": ("unset", "lf"),
    "resources/jce-policy-probe.woff2": ("unset", "lf"),
}
VISIBLE = [
    ".gitignore", ".gitattributes", ".editorconfig", "AGENTS.md", "CLAUDE.md",
    ".github/workflows/jce-policy-probe.yml",
    "engine/src/build/jce-policy-probe.c", "editor/src/jce-policy-probe.cpp",
    "tools/lint/jce-policy-probe.py", "tools/audit/jce-policy-probe.py",
    "tests/jce-policy-probe.c", "scripting/python/jce_script/__init__.py",
    "tools/jce-policy-package/__init__.py",
    "examples/elemental_serenity/CMakeLists.txt",
    "examples/elemental_serenity/src/main.c",
    "examples/elemental_serenity/resources/assets/scenes/elemental_serenity.scene.json",
    "examples/elemental_serenity/resources/assets/scripts/es_director.lua",
    "examples/caged_kingdom/CMakeLists.txt",
    "examples/caged_kingdom/src/game/_policy_probe.c",
    "examples/caged_kingdom/resources/assets/scenes/black_hole_lab.scene.json",
]
IGNORED = [
    "docs/jce-policy-probe.md", ".docs/way/jce-policy-probe.md",
    "scripts/android/local.properties", "tools/build/android/local.properties",
    "engine/src/renderer/internal/stb_image.h",
    ".env", ".claude/settings.local.json", "build/jce-policy-probe.txt",
    "dist/jce-policy-probe.txt", "out/jce-policy-probe.txt",
    "examples/minesweeper/build/jce-policy-probe.txt",
    "examples/snake_seven/csharp/bin/jce-policy-probe.dll",
    "scripting/csharp/obj/jce-policy-probe.json",
    "tools/lint/__pycache__/jce-policy-probe.pyc",
    "engine/src/middleware/video/third_party/jce-policy-probe.h",
    "private/jce-policy-probe.py", "reports/jce-policy-probe.json",
    "wasm_console.txt", "wasm_dist-probe.png", "t.obj",
    "examples/elemental_serenity/build/jce-policy-probe.c",
    "examples/elemental_serenity/dist/jce-policy-probe.exe",
    "examples/elemental_serenity/native/bin/jce-policy-probe.dll",
    "examples/elemental_serenity/archive/legacy_ai_dispatch/jce-policy-probe.c",
    "examples/elemental_serenity/resources/assets/models/imported-policy-probe.glb",
    "examples/caged_kingdom/build/jce-policy-probe.c",
    "examples/caged_kingdom/dist/jce-policy-probe.exe",
    "examples/caged_kingdom/_retired_ck_app/jce-policy-probe.c",
    "examples/caged_kingdom/resources/assets/models/private-policy-probe.glb",
]


def git(root, *args, data=None):
    result = subprocess.run(["git", *args], cwd=root, input=data,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode not in (0, 1) or (result.returncode and args[0] != "check-ignore"):
        raise RuntimeError(result.stderr.decode("utf-8", errors="replace"))
    return result.stdout


def paths(root, *args):
    return [p for p in git(root, *args).decode("utf-8").split("\0") if p]


def attributes(root, names):
    if not names:
        return {}
    raw = git(root, "check-attr", "-z", "--stdin", "text", "eol",
              data=("\0".join(names) + "\0").encode("utf-8"))
    fields = raw.decode("utf-8").split("\0")[:-1]
    result = {}
    for i in range(0, len(fields), 3):
        result.setdefault(fields[i], {})[fields[i + 1]] = fields[i + 2]
    return result


def policy_errors(root):
    errors = []
    attrs = attributes(root, list(ATTR_PROBES))
    for name, (text, eol) in ATTR_PROBES.items():
        if attrs[name] != {"text": text, "eol": eol}:
            errors.append(f"attribute mismatch: {name}: {attrs[name]}")
    raw = git(root, "check-ignore", "--no-index", "-v", "-n", "-z", "--stdin",
              data=("\0".join(VISIBLE + IGNORED) + "\0").encode("utf-8"))
    fields = raw.decode("utf-8").split("\0")[:-1]
    decisions = {}
    for i in range(0, len(fields), 4):
        pattern, name = fields[i + 2:i + 4]
        decisions[name] = bool(pattern and not pattern.startswith("!"))
    for name in VISIBLE:
        if decisions.get(name, True):
            errors.append(f"source is ignored: {name}")
    for name in IGNORED:
        if not decisions.get(name, False):
            errors.append(f"generated/private/upstream path is visible: {name}")
    hidden = paths(root, "ls-files", "-c", "-i", "--exclude-standard", "-z")
    errors.extend(f"tracked source matches ignore rules: {name}" for name in hidden)
    return errors


def is_link(path):
    return path.is_symlink() or bool(
        getattr(path.lstat(), "st_file_attributes", 0)
        & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))


def owned_paths(root, skipped):
    errors = []
    def onerror(error):
        errors.append(str(error))
    for base, dirs, files in os.walk(root, followlinks=False, onerror=onerror):
        keep = []
        for name in dirs:
            p = Path(base) / name
            linked = is_link(p)
            if name in PROTECTED or linked or (p / ".git").exists():
                skipped["protected_roots"] += 1
            elif name in GENERATED or (Path(base) == root and name == "reports"):
                skipped["generated_roots"] += 1
            else:
                keep.append(name)
        dirs[:] = keep
        for name in files:
            p = Path(base) / name
            if is_link(p):
                skipped["protected_links"] += 1
            else:
                yield p.relative_to(root).as_posix()
    if errors:
        raise RuntimeError("\n".join(errors))


def is_text(data, attr):
    if attr.get("text") == "unset" or b"\0" in data:
        return False
    # Same binary safeguard on fixtures as on a real workspace. Keep encodings.
    return not any(c < 9 or 13 < c < 32 for c in data[:8192])


def normalized(data, eol):
    lf = data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    return lf.replace(b"\n", b"\r\n") if eol == "crlf" else lf


def inspect(root, all_owned=False, fix=False):
    skipped = Counter()
    names = sorted(set(owned_paths(root, skipped) if all_owned else
                       paths(root, "ls-files", "-c", "-o", "--exclude-standard", "-z")))
    attrs = attributes(root, names)
    changed, violations = [], []
    text_count = 0
    for name in names:
        p = root / name
        if not p.is_file() or is_link(p):
            continue
        if "third_party" in p.parts:
            raise RuntimeError(f"upstream source became trackable: {name}")
        with p.open("rb") as stream:
            sample = stream.read(8192)
        if not is_text(sample, attrs[name]):
            skipped["binary_files"] += 1
            continue
        data = p.read_bytes()
        if not is_text(data, attrs[name]):
            skipped["binary_files"] += 1
            continue
        text_count += 1
        eol = "crlf" if p.suffix.lower() in (".bat", ".cmd") else "lf"
        if attrs[name].get("eol") != eol:
            violations.append(f"wrong effective eol for text: {name}")
            continue
        target = normalized(data, eol)
        if data != target:
            if fix:
                p.write_bytes(target)
                changed.append(name)
            else:
                violations.append(f"working text must use {eol}: {name}")
    # Git stores text as LF even for .bat/.cmd, which check out as CRLF.
    for entry in paths(root, "ls-files", "--eol", "-z"):
        meta, name = entry.split("\t", 1)
        index_kind = meta.split()[0]
        if index_kind in ("i/crlf", "i/mixed"):
            violations.append(f"index text needs git add --renormalize: {name}")
    return {"text_files": text_count, "changed": changed,
            "violations": violations, "skipped": dict(skipped)}


def self_check():
    # Actual Git attributes/index/ignore decisions, not a hand-written globber.
    with tempfile.TemporaryDirectory(prefix="jce-text-policy-") as tmp:
        root = Path(tmp)
        git(root, "init", "-q")
        git(root, "config", "core.autocrlf", "false")
        git(root, "config", "core.excludesFile", str(root / "no-global-ignore"))
        for name in (".gitattributes", ".gitignore",
                     "examples/minesweeper/.gitignore",
                     "examples/snake_seven/.gitignore",
                     "scripting/csharp/.gitignore"):
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_bytes((ROOT / name).read_bytes())
        (root / ".editorconfig").write_bytes((ROOT / ".editorconfig").read_bytes())
        def put(name, data):
            p = root / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        put("src/sample.c", b"a\r\nb\nc\r")
        put("scripts/sample.bat", b"a\r\nb\n")
        put("src/sample.bin", b"\0\r\n\xff")
        put("engine/third_party/original.h", b"upstream\r\n")
        put("project/reports/notes.md", b"owned\r\n")
        put("build/generated.c", b"generated\r\n")
        put("reports/generated.log", b"generated\r\n")
        git(root, "add", "--", ".gitignore", ".gitattributes", ".editorconfig")
        assert not policy_errors(root)
        assert len(inspect(root)["violations"]) == 3
        result = inspect(root, all_owned=True, fix=True)
        assert set(result["changed"]) == {"src/sample.c", "scripts/sample.bat",
                                          "project/reports/notes.md"}
        assert (root / "project/reports/notes.md").read_bytes() == b"owned\n"
        assert (root / "src/sample.c").read_bytes() == b"a\nb\nc\n"
        assert (root / "scripts/sample.bat").read_bytes() == b"a\r\nb\r\n"
        for name, data in [
            ("src/sample.bin", b"\0\r\n\xff"),
            ("engine/third_party/original.h", b"upstream\r\n"),
            ("build/generated.c", b"generated\r\n"),
            ("reports/generated.log", b"generated\r\n")]:
            assert (root / name).read_bytes() == data
        assert not inspect(root, all_owned=True, fix=True)["changed"]
        git(root, "add", "--", "src/sample.c", "scripts/sample.bat")
        assert git(root, "show", ":scripts/sample.bat") == b"a\nb\n"
        original = (root / ".gitattributes").read_bytes()
        (root / ".gitattributes").write_bytes(original + b"\n*.c -text\n")
        assert policy_errors(root)
        (root / ".gitattributes").write_bytes(original)
        original = (root / ".gitignore").read_bytes()
        for bad_rule in (b"\n*.c\n", b"\n!/private/\n!/private/**\n"):
            (root / ".gitignore").write_bytes(original + bad_rule)
            assert policy_errors(root)
        (root / ".gitignore").write_bytes(original)
    print("repository hygiene self-check: PASS (negative controls and idempotence)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix", action="store_true")
    parser.add_argument("--all-owned", action="store_true")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    if args.self_check:
        self_check()
        return 0
    result = inspect(ROOT, all_owned=args.all_owned, fix=args.fix)
    result["policy_errors"] = policy_errors(ROOT)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_bytes(
            (json.dumps(result, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    for error in result["violations"] + result["policy_errors"]:
        print("FAIL:", error)
    print(f"repository hygiene: {result['text_files']} text files, "
          f"{len(result['changed'])} synchronized, exclusions={result['skipped']}")
    return int(bool(result["violations"] or result["policy_errors"]))


if __name__ == "__main__":
    raise SystemExit(main())
