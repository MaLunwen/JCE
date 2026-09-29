#!/usr/bin/env python3
"""Enforce repository ownership without relying on local ignored files."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]


def git(root, *args):
    p = subprocess.run(["git", *args], cwd=root, capture_output=True)
    if p.returncode:
        raise RuntimeError(p.stderr.decode("utf-8", errors="replace"))
    return p.stdout


def violations(root):
    contract = json.loads((root / "contracts/source-layout.json").read_text(encoding="utf-8"))
    paths = [p for p in git(root, "ls-files", "-z").decode("utf-8").split("\0") if p]
    if not paths:
        raise RuntimeError("refusing an empty inventory")
    forbidden = contract["scratch_roots"] + contract["private_implementation_roots"] + contract["protected_original_copies"]
    failures = []
    for p in paths:
        if any(p.startswith(s) if s.endswith("/") else p == s for s in forbidden):
            failures.append("local/private/upstream content tracked: " + p)
        if p.startswith("contracts/") and re.search(r"(?:plan|verification|audit|migration|limits)-20\d\d-", Path(p).name):
            failures.append("dated delivery record belongs under local docs/: " + p)
        if p.startswith("scripts/") and p.endswith(".py"):
            text = (root / p).read_text(encoding="utf-8")
            if len(text.splitlines()) > 15 or 'exec(compile(' not in text or '"tools/' not in text:
                failures.append("manual Python entrypoint contains backend logic: " + p)
    return failures


def self_check():
    with tempfile.TemporaryDirectory(prefix="jce-layout-") as tmp:
        root = Path(tmp)
        git(root, "init", "-q")
        git(root, "config", "core.excludesFile", str(root / "no-global-ignore"))
        (root / "contracts").mkdir()
        (root / "contracts/source-layout.json").write_bytes((ROOT / "contracts/source-layout.json").read_bytes())
        git(root, "add", "contracts/source-layout.json")
        assert not violations(root)
        for path in ["docs/local.md", "private/algorithm.py", "engine/src/renderer/internal/stb_image.h", "contracts/media-verification-2026-09-29.md", "scripts/bad.py"]:
            q = root / path
            q.parent.mkdir(parents=True, exist_ok=True)
            q.write_text("sample\n", encoding="utf-8")
            git(root, "add", path)
            assert violations(root), path
            git(root, "rm", "--cached", "--", path)
        assert not violations(root)
    print("source-layout self-check: PASS (5 negative controls)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    if args.self_check:
        self_check()
        return 0
    errors = violations(ROOT)
    for error in errors:
        print("FAIL:", error)
    print("source-layout:", "FAIL" if errors else "PASS")
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
