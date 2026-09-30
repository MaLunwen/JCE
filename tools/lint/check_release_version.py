#!/usr/bin/env python3
"""Keep release commit subjects, CMake, Java ABI and README in agreement.

The commit-msg hook reads the Git index, because a correct working file that
was not staged must not make a mismatched commit appear valid. A normal lint
run checks the working tree and checks HEAD when its CMake version still
matches the working tree (so preparing the next release can pass before commit).
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
VERSION = re.compile(r"project\(JCE VERSION (\d+)\.(\d+)\.(\d+) LANGUAGES C\)")
JAVA = re.compile(r"EXPECTED_API_VERSION\s*=\s*0x([0-9A-Fa-f]{8})")
README = re.compile(r"\| Version \| `(\d+\.\d+\.\d+)` \(authoritative: `CMakeLists.txt`\) \|")
SUBJECT = re.compile(r"^v-(\d+\.\d+\.\d+)(?:$|[, :])")


def git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=ROOT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result.stdout


def version(cmake: str) -> tuple[int, int, int]:
    match = VERSION.search(cmake)
    if not match:
        raise ValueError("cannot read project(JCE VERSION ...) from CMakeLists.txt")
    return tuple(map(int, match.groups()))


def check_files(cmake: str, java: str, readme: str) -> list[str]:
    value = version(cmake)
    label = ".".join(map(str, value))
    packed = (value[0] << 24) | (value[1] << 16) | (value[2] << 8)
    mirror = JAVA.search(java)
    errors = []
    if not mirror or int(mirror.group(1), 16) != packed:
        errors.append(f"Java EXPECTED_API_VERSION must be 0x{packed:08X} for {label}")
    fact = README.search(readme)
    if not fact or fact.group(1) != label:
        errors.append(f"README Version must be {label}")
    return errors


def check_subject(subject: str, cmake: str, previous: str | None = None) -> list[str]:
    label = ".".join(map(str, version(cmake)))
    release = SUBJECT.match(subject.strip().splitlines()[0] if subject.strip() else "")
    if previous is not None and version(previous) != version(cmake) and not release:
        return [f"CMake version changed to {label}; commit subject must start v-{label}"]
    if release and release.group(1) != label:
        return [f"commit says v-{release.group(1)} but CMake says {label}; use v-{label}"]
    return []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--commit-message-file", type=Path,
                    help="check proposed commit against staged source")
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()
    if args.self_check:
        cmake = "project(JCE VERSION 0.12.2 LANGUAGES C)"
        java = "EXPECTED_API_VERSION = 0x000C0200;"
        readme = "| Version | `0.12.2` (authoritative: `CMakeLists.txt`) |"
        assert not check_files(cmake, java, readme)
        assert not check_subject("v-0.12.2, Release", cmake)
        assert check_subject("v-0.12.1", cmake)
        assert check_subject("Fix", cmake, "project(JCE VERSION 0.12.1 LANGUAGES C)")
        assert check_files(cmake, "EXPECTED_API_VERSION = 0x000B0400;", readme)
        print("release version self-check: PASS")
        return 0

    paths = ("CMakeLists.txt", "engine/java/com/jce/JceRuntime.java", "README.md")
    if args.commit_message_file:
        sources = [git("show", f":{path}") for path in paths]
        previous = git("show", "HEAD:CMakeLists.txt")
        subject = args.commit_message_file.read_text(encoding="utf-8-sig")
        errors = check_files(*sources) + check_subject(subject, sources[0], previous)
    else:
        sources = [(ROOT / path).read_text(encoding="utf-8") for path in paths]
        errors = check_files(*sources)
        head_cmake = git("show", "HEAD:CMakeLists.txt")
        if version(head_cmake) == version(sources[0]):
            try:
                previous = git("show", "HEAD^:CMakeLists.txt")
            except RuntimeError:  # initial import has no parent commit
                previous = None
            errors += check_subject(git("log", "-1", "--format=%s"),
                                    head_cmake, previous)
    for error in errors:
        print(f"FAIL: {error}")
    if not errors:
        print("release version: PASS")
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
