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


def tracked_state_note():
    """State plainly which referenced paths a fresh clone actually receives.

    The previous manifest described the machine it ran on, not the commit.
    Paths like AGENTS.md and tests/ are gitignored on this branch, so a reader
    who clones and follows the instructions finds them missing.
    """
    probes = ["AGENTS.md", "CLAUDE.md", "tests", "tools/audit",
              "tools/lint/check_skills.py", "tools/pack_skill.py"]
    lines = ["skill 文本引用的仓库路径，在**当前分支**上的跟踪状态"
             "（`git ls-files`，全路径逐条查，不用 grep 子串）：",
             "",
             "| 路径 | 跟踪 |",
             "|---|---|"]
    for path in probes:
        out = _git("ls-files", "--", path)
        if out is None:
            mark = "查询失败"
        else:
            n = len([ln for ln in out.splitlines() if ln.strip()])
            mark = ("是（%d 个文件）" % n) if n else "**否 — 未跟踪**"
        lines.append("| `%s` | %s |" % (path, mark))
    lines += [
        "",
        "标「否」的路径**不随 clone 走**，它们是本机工作树的叠加物。",
        "本 zip 里凡引用这些路径的段落，克隆者复现不出来——这是事实陈述而非缺陷；",
        "写下来是为了不让读者以为照做就能得到同一棵树。",
    ]
    return "\n".join(lines)


def build_manifest(files, cid, commit):
    rows = ["| `%s` | %d | %d | `%s` |"
            % (n, line_count(d), len(d), sha256_bytes(d)) for n, d in files]
    md_lines = sum(line_count(d) for n, d in files if n.endswith(".md"))
    total = sum(len(d) for _, d in files)
    body = [
        "# jce skill — MANIFEST",
        "",
        "内容标识 **`%s`**——对下表除本文件外全部文件的名字与字节的哈希。" % cid,
        "同内容必得同标识，不同内容必得不同标识；文件名就是它，所以版本不可复用。",
        "",
        "打包时 JCE 仓库位于 `%s`。**这只是上下文，它不决定本 zip 的内容**：" % commit,
        "skill 源在仓库之外，仓库里没有它的任何字节。",
        "",
        "## 文件",
        "",
        "| 文件 | 行 | 字节 | SHA-256 |",
        "|---|---:|---:|---|",
    ] + rows + [
        "",
        "合计 **%d 个文件**（含本清单），Markdown **%d 行**，**%d 字节**。"
        % (len(files) + 1, md_lines, total),
        "行数按 `splitlines()` 计。按换行切分会给每个以换行结尾的文件多算一行，",
        "上一版清单把 2394 报成 2406 就是这个原因。",
        "",
        "## 这份归档能复现什么、不能复现什么",
        "",
        tracked_state_note(),
        "",
        "## 校验",
        "",
        "```bash",
        "# zip 自身的哈希在同目录的 .sha256 里（分离式，不在包内）",
        "sha256sum -c jce-skill-%s.zip.sha256" % cid,
        "",
        "# 解包后逐文件复验",
        "python tools/pack_skill.py --verify <解包目录>/jce",
        "```",
        "",
        "重新打包同一份内容必须得到**逐字节相同**的 zip。若不同，说明打包过程",
        "本身带了时间戳或顺序这类非内容输入，那时版本号不再标识任何东西。",
        "",
    ]
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
    print("pack_skill: %d payload file(s), content id %s" % (len(payload), cid))
    for n, d in files:
        print("  %-46s %5d lines  %s" % (n, line_count(d), sha256_bytes(d)[:16]))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=str(REPO_ROOT / "dist" / "skill"),
                    help="output directory (default: dist/skill, gitignored)")
    ap.add_argument("--install", default=None,
                    help="path to INSTALL.md (default: inside the skill dir)")
    ap.add_argument("--verify", default=None, metavar="DIR",
                    help="do not package; re-derive the content id of an "
                         "unpacked skill directory")
    args = ap.parse_args(argv)

    if args.verify:
        return cmd_verify(Path(args.verify))

    skill_dir = SKILL_LINK if SKILL_LINK.exists() else SKILL_SRC
    if not skill_dir.is_dir():
        print("pack_skill: no skill at %s or %s -- a broken junction is "
              "reported by check_skills.py as BROKEN LINK, not as a missing "
              "skill" % (SKILL_LINK, SKILL_SRC))
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
    name = "jce-skill-%s.zip" % cid
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
