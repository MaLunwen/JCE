#!/usr/bin/env python3
"""
run_coverage.py — Measure JCE unit-test line coverage and enforce a
minimum threshold (default 85%, per IMPLEMENTATION_PLAN 阶段 1).

Strategy:
  * Windows / MSVC  → OpenCppCoverage (Cobertura XML)
  * Linux / clang   → llvm-cov + llvm-profdata (planned, stub)
  * Linux / gcc     → gcov + gcovr (planned, stub)
  * macOS / clang   → llvm-cov + llvm-profdata (planned, stub)

Usage:
    python tools/coverage/run_coverage.py \\
        --build-dir build/desktop/windows-x64-debug \\
        --sources engine/src/os/core \\
        --threshold 85

Exit codes:
    0 — coverage met threshold
    1 — coverage below threshold
    2 — tooling missing / not implemented for host

Design:
  * No third-party Python deps — only stdlib (xml.etree).
  * Discovers ctest binary from PATH.
  * Filters report to the --sources tree so test code itself isn't
    counted in the denominator.
"""
from __future__ import annotations
import argparse, os, platform, shutil, subprocess, sys
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def find_opencppcoverage() -> str | None:
    exe = shutil.which("OpenCppCoverage")
    if exe:
        return exe
    for cand in (
        Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
            / "OpenCppCoverage" / "OpenCppCoverage.exe",
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
            / "OpenCppCoverage" / "OpenCppCoverage.exe",
    ):
        if cand.is_file():
            return str(cand)
    return None


def run_windows(build_dir: Path, sources: list[Path], out_xml: Path) -> int:
    exe = find_opencppcoverage()
    if not exe:
        print("ERROR: OpenCppCoverage not found. Install via "
              "`winget install OpenCppCoverage.OpenCppCoverage` or see "
              "https://github.com/OpenCppCoverage/OpenCppCoverage/releases",
              file=sys.stderr)
        return 2

    ctest = shutil.which("ctest") or "ctest"
    cmd = [exe, "--quiet", "--cover_children",
           "--export_type", f"cobertura:{out_xml}"]
    for s in sources:
        cmd += ["--sources", str(s.resolve())]
    cmd += ["--excluded_sources", "third_party"]
    cmd += ["--", ctest,
            "--test-dir", str(build_dir.resolve()),
            "--output-on-failure", "-L", "unit"]

    print("$", " ".join(f'"{c}"' if " " in c else c for c in cmd))
    rc = subprocess.call(cmd)
    if rc != 0:
        print(f"WARN: ctest under OpenCppCoverage returned {rc}", file=sys.stderr)
    return 0 if out_xml.exists() else (rc or 2)


def parse_cobertura(xml_path: Path) -> tuple[int, int]:
    """Return (lines_covered, lines_valid).

    OpenCppCoverage emits one ``<class>`` per (file × test-executable) pair
    when driven via ``--cover_children`` over CTest, so the same source file
    appears multiple times.  We dedupe by ``(filename, line_number)`` and
    keep the union — a line is covered if *any* test exe touched it — which
    matches what every other coverage tool reports.
    """
    root = ET.parse(xml_path).getroot()
    seen: dict[tuple[str, int], int] = {}
    for cls in root.iter("class"):
        fn = cls.attrib.get("filename", "")
        for ln in cls.iter("line"):
            num  = int(ln.attrib.get("number", 0))
            hits = int(ln.attrib.get("hits",   0))
            key  = (fn, num)
            prev = seen.get(key)
            if prev is None or hits > prev:
                seen[key] = hits
    valid   = len(seen)
    covered = sum(1 for h in seen.values() if h > 0)
    return covered, valid


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True, type=Path,
                    help="CMake build directory containing CTest config")
    ap.add_argument("--sources", required=True, action="append", type=Path,
                    help="Source directory to measure (repeatable)")
    ap.add_argument("--threshold", type=float, default=85.0,
                    help="Minimum line-coverage percentage (default: 85)")
    ap.add_argument("--out", type=Path,
                    default=REPO_ROOT / "build" / "coverage" / "cobertura.xml",
                    help="Where to write the Cobertura XML report")
    args = ap.parse_args()

    args.out.parent.mkdir(parents=True, exist_ok=True)

    system = platform.system()
    if system == "Windows":
        rc = run_windows(args.build_dir, args.sources, args.out)
    else:
        print(f"ERROR: coverage backend for {system} not yet implemented. "
              "TODO: wire llvm-cov / gcovr.", file=sys.stderr)
        return 2

    if rc != 0:
        return rc

    covered, valid = parse_cobertura(args.out)
    pct = (100.0 * covered / valid) if valid else 0.0
    print("=" * 70)
    print(f"coverage: {covered}/{valid} lines = {pct:.2f}% "
          f"(threshold {args.threshold:.2f}%)")
    print(f"report  : {args.out}")
    print("=" * 70)

    if pct + 1e-9 < args.threshold:
        print(f"FAIL: coverage {pct:.2f}% < required {args.threshold:.2f}%",
              file=sys.stderr)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
