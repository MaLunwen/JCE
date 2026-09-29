#!/usr/bin/env python3
"""Read-only audit of codec vendor sources against pinned upstream Git objects.

References are separate official-tag checkouts, supplied via --reference-cache.
The audit never writes into a vendor directory or repairs/patches a dependency.
Git archives are read from immutable commits, not reference working-tree files.
CRLF-only differences are reported separately from actual content changes.
"""

import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/build"))
from fetch_vendor_sources import PINS as SOURCE_PINS, verify

ROOT = Path(__file__).resolve().parents[2]
PINS = (
    ("fdk-aac", "engine/src/middleware/audio/third_party/fdk-aac",
     "jce_vendor_audit_fdk_v2_0_3", "716f4394641d53f0d79c9ddac3fa93b03a49f278",
     "https://github.com/mstorsjo/fdk-aac", "v2.0.3"),
    ("openh264", "engine/src/middleware/video/third_party/openh264",
     "jce_vendor_audit_openh264_v2_6_0", "652bdb7719f30b52b08e506645a7322ff1b2cc6f",
     "https://github.com/cisco/openh264", "v2.6.0"),
    ("libhevc", "engine/src/middleware/video/third_party/libhevc",
     "jce_vendor_audit_libhevc_v1_6_0", "8dd2b3392a24b2eed124e50531e79fee9ab48fd6",
     "https://github.com/ittiam-systems/libhevc", "v1.6.0"),
)


def git(directory, *args):
    return subprocess.check_output(["git", "-C", str(directory), *args])


def normalize(data):
    if b"\0" in data:
        return data
    try:
        data.decode("utf-8")
    except UnicodeDecodeError:
        return data
    return data.replace(b"\r\n", b"\n")


def audit_tree(local, reference, commit, origin, tag):
    actual_origin = git(reference, "remote", "get-url", "origin").decode().strip()
    if actual_origin.removesuffix(".git") != origin:
        raise ValueError(f"unexpected reference origin: {actual_origin}")
    actual_commit = git(reference, "rev-parse", f"{tag}^{{commit}}").decode().strip()
    if actual_commit != commit:
        raise ValueError(f"{tag} resolves to {actual_commit}, expected {commit}")
    archive = git(reference, "archive", "--format=tar", commit)
    result = dict(commit=commit, origin=origin, tag=tag, upstream_files=0,
                  byte_identical=0, newline_only=[], changed=[], missing=[], extras=[])
    known = set()
    with tarfile.open(fileobj=io.BytesIO(archive)) as tree:
        for item in tree:
            if not item.isfile() and not item.issym():
                continue
            known.add(item.name)
            result["upstream_files"] += 1
            path = local / item.name
            if not path.is_file():
                result["missing"].append(item.name)
                continue
            expected = item.linkname.encode() if item.issym() else tree.extractfile(item).read()
            actual = path.read_bytes()
            if actual == expected:
                result["byte_identical"] += 1
            elif normalize(actual) == normalize(expected):
                result["newline_only"].append(item.name)
            else:
                result["changed"].append(dict(path=item.name,
                    actual_sha256=hashlib.sha256(actual).hexdigest(),
                    upstream_sha256=hashlib.sha256(expected).hexdigest()))
    result["extras"] = sorted(p.relative_to(local).as_posix() for p in local.rglob("*")
                               if p.is_file() and ".git" not in p.relative_to(local).parts
                               and p.relative_to(local).as_posix() not in known)
    result["compliant"] = not (result["changed"] or result["missing"] or result["extras"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-cache", type=Path, default=Path(tempfile.gettempdir()))
    parser.add_argument("--output", type=Path, help="Write JSON outside vendor directories")
    parser.add_argument("--active-cache", type=Path,
                        help="Verify all pinned build sources instead of legacy vendor trees")
    args = parser.parse_args()
    if args.output and "third_party" in args.output.resolve().parts:
        parser.error("audit output must not be inside third_party")
    if args.active_cache:
        pins = json.loads(SOURCE_PINS.read_text(encoding="utf-8"))["sources"]
        report = dict(scope="All pinned active build sources; excludes legacy trees and Conan cache",
                      normalization="None: original archive bytes and full file inventory",
                      vendors={}, tracked_vendor_files=[])
        for name, pin in pins.items():
            entry = dict(version=pin["version"], url=pin["url"], sha256=pin["sha256"])
            try:
                entry["directory"] = str(verify(name, pin, args.active_cache.resolve()))
                entry["compliant"] = True
            except (OSError, ValueError, tarfile.TarError) as error:
                entry.update(compliant=False, error=str(error))
            report["vendors"][name] = entry
        tracked = git(ROOT, "ls-files", "-z", "*third_party*").decode().split("\0")
        report["tracked_vendor_files"] = sorted(p for p in tracked if p)
        report["compliant"] = (all(v["compliant"] for v in report["vendors"].values())
                               and not report["tracked_vendor_files"])
        if args.output:
            args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                                   encoding="utf-8")
        for name, vendor in report["vendors"].items():
            print(f"{name}: {'PASS' if vendor['compliant'] else 'FAIL'}")
            if "error" in vendor:
                print(vendor["error"])
        print(f"tracked vendor files: {len(report['tracked_vendor_files'])}")
        return 0 if report["compliant"] else 1
    report = dict(scope="Three manually downloaded codec trees plus minimp4; excludes Conan cache",
                  normalization="Only text CRLF is normalized; binary bytes are compared exactly",
                  vendors={}, tracked_vendor_files=[])
    for name, location, cache, commit, origin, tag in PINS:
        try:
            report["vendors"][name] = audit_tree(ROOT / location, args.reference_cache / cache,
                                                 commit, origin, tag)
        except (OSError, subprocess.CalledProcessError, ValueError) as error:
            report["vendors"][name] = dict(compliant=False, error=str(error))
    data = normalize((ROOT / "engine/src/middleware/video/third_party/minimp4.h").read_bytes())
    blob = hashlib.sha1(f"blob {len(data)}\0".encode() + data).hexdigest()
    expected_blob = "d461ffb30c52c737fa20ad7a1a25a16dd339397b"
    report["vendors"]["minimp4"] = dict(version="4575afb", blob_git_sha1=blob,
        upstream_blob_git_sha1=expected_blob, compliant=blob == expected_blob)
    tracked = git(ROOT, "ls-files", "-z", "*third_party*").decode().split("\0")
    report["tracked_vendor_files"] = sorted(p for p in tracked if p)
    report["compliant"] = (all(v["compliant"] for v in report["vendors"].values())
                           and not report["tracked_vendor_files"])
    rendered = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    for name, vendor in report["vendors"].items():
        print(f"{name}: {'PASS' if vendor['compliant'] else 'FAIL'} "
              f"changed={len(vendor.get('changed', []))} missing={len(vendor.get('missing', []))} "
              f"extras={len(vendor.get('extras', []))}")
        if "error" in vendor:
            print(vendor["error"])
        if "blob_git_sha1" in vendor and not vendor["compliant"]:
            print(f"  blob differs: {vendor['blob_git_sha1']} != {vendor['upstream_blob_git_sha1']}")
    print(f"tracked vendor files: {len(report['tracked_vendor_files'])}")
    return 0 if report["compliant"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
