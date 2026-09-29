#!/usr/bin/env python3
"""Package the `jce` skill into a reproducible, content-addressed archive.

The canonical public skill source is skills/jce/ in this repository.
Runtime entries point to that directory; private references are excluded.

Three properties this file exists to guarantee:

1. DETERMINISM.  Two runs over identical input produce a byte-identical zip.
   Python's default ZipFile.write stamps each entry with the file's mtime, so
   the same content packaged twice hashed differently and the version string
   identified nothing.  Every entry here gets a fixed timestamp, fixed external
   attributes, sorted order, and a pinned compression level.

2. HONEST LINE COUNTS.  Splitting on newline yields a trailing empty element
   for any file that ends in a newline -- i.e. every well-formed text file --
   so it overcounts by exactly one per file.  Twelve files, twelve phantom
   lines, and a manifest that published 2406 where the truth was 2394.
   splitlines() is the count.

3. COMPLETE COVERAGE.  The manifest hashes every file that ships, INSTALL.md
   included.  Hashing only the payload leaves the install instructions -- the
   one file a recipient reads first -- unverifiable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
# Package the canonical public repository skill.
SKILL_LINK = REPO_ROOT / "skills" / "jce"
SKILL_SRC = SKILL_LINK

ARCHIVE_ROOT = "jce"                  # the directory name inside the zip
FIXED_DATE = (1980, 1, 1, 0, 0, 0)    # the zip epoch; any constant would do
FIXED_MODE = 0o644 << 16
# ONLY the manifest is excluded from the content id, and only because it is
# derived from that id -- including it would be circular.  INSTALL.md was
# excluded too in the first version, which reintroduced the exact defect the
# content addressing exists to prevent: editing INSTALL.md changed what shipped
# without changing the archive's name.  Measured by unpacking an archive and
# diffing it against the source tree (63 lines vs 64, same content id).
DERIVED = ("MANIFEST.md",)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def _git(*args):
    try:
        out = subprocess.run(["git", "-C", str(REPO_ROOT)] + list(args),
                             capture_output=True, text=True, timeout=30)
        return out.stdout if out.returncode == 0 else None
    except Exception:
        return None


def git_commit():
    """The repo commit, recorded as CONTEXT -- it does not determine content."""
    out = _git("rev-parse", "--short=8", "HEAD")
    return out.strip() if out else "unknown"


def line_count(data):
    """Lines, counted the way a human counts them.

    Splitting on newline yields a trailing empty string for any file ending in
    a newline, so it reports one line more than the file has.
    """
    return len(data.splitlines())


def collect(skill_dir):
    """Every shipped file as (archive-relative posix path, bytes), sorted.

    Sorted because directory iteration order is a property of the filesystem,
    not of the content, and an unsorted archive is not reproducible.
    """
    files = []
    for p in sorted(skill_dir.rglob("*")):
        if not p.is_file():
            continue
        if "__pycache__" in p.parts or p.name.startswith("."):
            continue
        rel = p.relative_to(skill_dir).as_posix()
        files.append(("%s/%s" % (ARCHIVE_ROOT, rel), p.read_bytes()))
    files.sort(key=lambda kv: kv[0])
    return files


def content_id_of(payload):
    """A hash over names AND bytes, so a rename alone changes the identity."""
    blob = b"".join(n.encode("utf-8") + b"\0" + d for n, d in payload)
    return sha256_bytes(blob)[:12]


def build_manifest(files, cid, commit=None):
    """Derive metadata only from payload; Git context cannot change zip bytes."""
    chinese = ARCHIVE_ROOT == "jce-zh-cn"
    title = "JCE skill 清单" if chinese else "JCE skill manifest"
    explanation = ("内容标识覆盖文件路径与原始字节；本清单为派生数据。" if chinese
                   else "The content ID covers file paths and original bytes; this manifest is derived.")
    columns = "| 文件 | 行 | 字节 | SHA-256 |" if chinese else "| File | Lines | Bytes | SHA-256 |"
    body = [f"# {title}", "", f"<!-- content-id: {cid} -->", "", explanation, "",
            columns, "| --- | ---: | ---: | --- |"]
    body += ["| `%s` | %d | %d | `%s` |" % (name, line_count(data), len(data), sha256_bytes(data))
             for name, data in files]
    body += ["", ("解包后核验：" if chinese else "Verify after unpacking:"), "", "```bash",
             "python tools/pack_skill.py --verify <unpacked>/" + ARCHIVE_ROOT, "```", "",
             ("相同内容应产生逐字节相同的归档。私有扩展不包含在包内。" if chinese
              else "Identical payloads produce identical archives. Private extensions are excluded."), ""]
    return "\n".join(body).encode("utf-8")


def write_zip(dest, files):
    dest.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in files:
            info = zipfile.ZipInfo(name, date_time=FIXED_DATE)
            info.external_attr = FIXED_MODE
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3        # unix, so the mode above is honoured
            z.writestr(info, data)
    return dest.read_bytes()


def cmd_verify(target):
    if not target.is_dir():
        print("pack_skill: %s is not a directory" % target)
        return 1
    files = collect(target)
    payload = [(n, d) for n, d in files if not n.endswith(DERIVED)]
    cid = content_id_of(payload)
    manifest = target / "MANIFEST.md"
    if not manifest.is_file() or manifest.read_bytes() != build_manifest(payload, cid):
        print("pack_skill: FAIL - manifest or payload differs from its recorded content")
        return 1
    print("pack_skill: verified %d payload file(s), content id %s" % (len(payload), cid))
    return 0


def main(argv=None):
    global ARCHIVE_ROOT
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--locale", choices=["en-US", "zh-CN"], default="en-US",
                    help="skill edition (default: en-US)")
    ap.add_argument("--out", default=str(REPO_ROOT / "dist" / "skill"),
                    help="output directory (default: dist/skill, gitignored)")
    ap.add_argument("--install", default=None,
                    help="path to INSTALL.md (default: inside the skill dir)")
    ap.add_argument("--verify", default=None, metavar="DIR",
                    help="do not package; re-derive the content id of an "
                         "unpacked skill directory")
    args = ap.parse_args(argv)

    catalog = json.loads((REPO_ROOT / "contracts/skill-locales.json").read_text(encoding="utf-8"))
    edition = catalog["locales"][args.locale]
    if args.verify:
        target = Path(args.verify)
        for config in catalog["locales"].values():
            if target.name == config["name"]:
                edition = config
        ARCHIVE_ROOT = edition["name"]
        return cmd_verify(target)
    ARCHIVE_ROOT = edition["name"]
    skill_dir = REPO_ROOT / edition["root"]
    if not skill_dir.is_dir():
        print("pack_skill: skill edition is missing: %s" % skill_dir)
        return 1

    install_path = Path(args.install) if args.install else (skill_dir / "INSTALL.md")
    if not install_path.is_file():
        print("pack_skill: INSTALL.md not found at %s.  It ships in the archive "
              "and is hashed by the manifest; packaging without it would leave "
              "the first file a recipient reads unverifiable." % install_path)
        return 1
    install_bytes = install_path.read_bytes()

    payload = [(n, d) for n, d in collect(skill_dir)
               if not n.endswith(DERIVED)
               and not n.endswith("/INSTALL.md")]
    if not payload:
        print("pack_skill: the skill directory holds no payload files")
        return 1
    # INSTALL.md is part of the identity: it ships, so changing it changes what
    # was shipped, so it must change the name.
    shipped = payload + [("%s/INSTALL.md" % ARCHIVE_ROOT, install_bytes)]
    shipped.sort(key=lambda kv: kv[0])
    cid = content_id_of(shipped)
    commit = git_commit()
    manifest = build_manifest(shipped, cid, commit)
    files = shipped + [("%s/MANIFEST.md" % ARCHIVE_ROOT, manifest)]
    files.sort(key=lambda kv: kv[0])

    # Content-addressed, therefore immutable: one content can never be
    # published under two names, and two contents can never share one name.
    name = "%s-skill-%s.zip" % (ARCHIVE_ROOT, cid)
    dest = Path(args.out) / name
    blob = write_zip(dest, files)
    digest = sha256_bytes(blob)
    (dest.parent / (name + ".sha256")).write_text(
        "%s  %s\n" % (digest, name), encoding="utf-8", newline="\n")

    print("pack_skill: %s" % dest)
    print("  files      %d (%d identity-bearing + derived MANIFEST.md)"
          % (len(files), len(shipped)))
    print("  markdown   %d lines" % sum(line_count(d) for n, d in files
                                        if n.endswith(".md")))
    print("  content id %s   repo commit %s (context only)" % (cid, commit))
    print("  zip sha256 %s" % digest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
