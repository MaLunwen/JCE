#!/usr/bin/env python3
"""Verify JCE build-root ownership and canonical temporary test workspaces.

The source check is independent of a populated build/ tree. --check-existing
also checks existing top-level build/ entries (e.g. after a local lint run).
It never deletes, moves, or creates build output.
"""
from __future__ import annotations

import argparse
import ast
import json
import os
from pathlib import Path
import re
import stat
import sys

ROOT = Path(__file__).resolve().parents[2]
POLICY = "contracts/build-output-roots.json"
_LEGACY_TEST_LITERAL = re.compile(r"build/[a-z0-9][a-z0-9-]*-tests/?")


def _is_reparse(path: Path) -> bool:
    details = path.lstat()
    return stat.S_ISLNK(details.st_mode) or bool(
        getattr(details, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    )


def source_errors(repo: Path) -> list[str]:
    contract_path = repo / POLICY
    if not contract_path.is_file():
        return ["missing build-output authority: " + POLICY]
    try:
        policy = json.loads(contract_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return ["invalid build-output authority: " + str(error)]
    errors: list[str] = []
    if policy.get("schema") != 1 or policy.get("root") != "build":
        errors.append("build-output authority schema/root mismatch")
    roots = policy.get("roots")
    producers = policy.get("test_producers")
    if not isinstance(roots, dict) or not isinstance(producers, dict):
        return errors + ["build-output roots/producers must be objects"]
    if "tests" not in roots or "desktop" not in roots or "host" not in roots:
        errors.append("build-output contract is missing required roots")
    if not producers:
        errors.append("build-output contract has no test producers")
    for name, owner in roots.items():
        if (not re.fullmatch(r"\.?[a-z][a-z0-9-]*", name) or name in (".", "..")
                or not isinstance(owner, dict) or not owner.get("purpose")
                or owner.get("retention") not in ("ephemeral", "cache", "evidence", "runtime", "locks")):
            errors.append("invalid build-output root: " + repr(name))
    declared: set[str] = set()
    for path, destinations in producers.items():
        if (not isinstance(path, str) or not path.startswith("tools/tests/")
                or not path.endswith(".py") or not isinstance(destinations, dict)
                or not destinations):
            errors.append("invalid test producer declaration: " + repr(path))
            continue
        owner_file = repo / path
        if not owner_file.is_file():
            errors.append("registered test producer missing: " + path)
            continue
        try:
            source = owner_file.read_text(encoding="utf-8")
        except OSError as error:
            errors.append("cannot read test producer " + path + ": " + str(error))
            continue
        for directory, expected_count in destinations.items():
            if (not isinstance(directory, str) or not re.fullmatch(r"[a-z0-9][a-z0-9-]*-tests", directory)
                    or type(expected_count) is not int or expected_count < 1):
                errors.append("invalid test destination: " + path + " " + repr(directory))
                continue
            declared.add(directory)
            old = "build/" + directory
            new = "build/tests/" + directory
            if source.count(new) != expected_count or source.count(old) != 0:
                errors.append(f"test workspace not canonical: {path}: {directory}; "
                              f"expected {expected_count} references under build/tests/")
    if len(declared) != sum(len(v) for v in producers.values() if isinstance(v, dict)):
        errors.append("duplicate test workspace ownership")
    # Discover *future* unregistered output roots, not only these 14. Inspect
    # Python AST string literals, not comments, docs or generated test output.
    for source_root in (repo / "tools/tests", repo / "tests"):
        if not source_root.is_dir():
            continue
        # Do not scan ignored build/ or dist/ copies living under a test
        # consumer: those are generated output, not first-party test sources.
        for directory, subdirs, filenames in os.walk(source_root, followlinks=False):
            subdirs[:] = [name for name in subdirs
                          if name not in {"build", "dist", "out", ".git", "__pycache__",
                                          "third_party", "node_modules", ".venv", "_cooked"}
                          and not _is_reparse(Path(directory) / name)]
            for name in filenames:
                if not name.endswith(".py") or name == "test_build_output_roots_v9.py":
                    continue  # exclude the checker's negative-control literals
                candidate = Path(directory) / name
                try:
                    relative = candidate.relative_to(repo).as_posix()
                    tree = ast.parse(candidate.read_text(encoding="utf-8"), filename=relative)
                except (SyntaxError, UnicodeError, OSError) as error:
                    errors.append("cannot inspect test source " + str(candidate) + ": " + str(error))
                    continue
                for node in ast.walk(tree):
                    if (isinstance(node, ast.Constant) and isinstance(node.value, str)
                            and _LEGACY_TEST_LITERAL.fullmatch(node.value)):
                        errors.append(f"unregistered root-level test output: {relative}:{node.lineno} "
                                      f"{node.value}; use build/tests/<name>")
    return errors


def existing_errors(repo: Path) -> list[str]:
    root = repo / "build"
    if not root.exists() and not root.is_symlink():
        return []  # A fresh clone has no generated directory.
    if _is_reparse(root) or not root.is_dir():
        return ["build root is a link/reparse-point or not a directory"]
    policy = json.loads((repo / POLICY).read_text(encoding="utf-8"))
    allowed = set(policy["roots"])
    errors = []
    for child in root.iterdir():
        if child.name not in allowed:
            errors.append("unregistered build/ top-level output: " + child.name)
        elif _is_reparse(child):
            errors.append("build/ root entry is a link/reparse-point: " + child.name)
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument("--check-existing", action="store_true",
                        help="require actual build/ top-level entries to match the contract")
    args = parser.parse_args(argv)
    repo = args.repo.resolve()
    errors = source_errors(repo)
    if args.check_existing and not errors:
        errors += existing_errors(repo)
    for error in errors:
        print("FAIL: " + error)
    if errors:
        print(f"check_build_output_roots: FAIL ({len(errors)} issue(s))")
        return 1
    print("check_build_output_roots: PASS (registered test roots canonical"
          + ("; existing build root admitted)" if args.check_existing else "; source-only)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
