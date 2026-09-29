#!/usr/bin/env python3
"""Fetch pinned original sources into a build cache, without vendor patches.

Existing source trees are verified, never repaired or overwritten. Pin changes
create new directories. No writes to vendor checkouts or Conan package caches.
"""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
PINS = ROOT / "contracts/vendor-sources.json"


def default_cache():
    """Keep upstream projects outside IDE source discovery in the JCE tree."""
    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local"))
    elif sys.platform == "darwin":
        base = Path.home() / "Library/Caches"
    else:
        base = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    return base / "JCE/upstream-sources"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def download(pin):
    request = urllib.request.Request(pin["url"], headers={"User-Agent": "JCE-source-fetch"})
    with urllib.request.urlopen(request, timeout=90) as response:
        data = response.read()
    if digest(data) != pin["sha256"]:
        raise ValueError("official download does not match pinned SHA256")
    return data


def archive_files(data):
    files = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        for item in archive:
            parts = PurePosixPath(item.name).parts
            if not parts or item.isdir():
                continue
            if (any(p in (".", "..") or ":" in p for p in parts)
                    or item.name.startswith("/") or "\\" in item.name):
                raise ValueError("unsafe archive path")
            name = "/".join(parts[1:])
            if not name or name in files or not (item.isfile() or item.issym()):
                raise ValueError("unsupported or duplicate archive member")
            content = item.linkname.encode() if item.issym() else archive.extractfile(item).read()
            files[name] = (content, item.mode, item.issym())
    return files


def verify_tree(directory, files):
    # rglob may suppress traversal errors and misreport an unreadable tree as
    # an inventory difference. Preserve the OS error instead of hiding it.
    def traversal_error(error):
        raise error

    actual = set()
    for root, directories, filenames in os.walk(directory, onerror=traversal_error):
        for name in filenames + directories:
            path = Path(root) / name
            if path.is_file() or path.is_symlink():
                actual.add(path.relative_to(directory).as_posix())
    if actual != set(files):
        raise ValueError(f"source tree inventory differs: {directory}")
    for name, (expected, _, symlink) in files.items():
        path = directory / name
        content = os.readlink(path).encode() if symlink and path.is_symlink() else path.read_bytes()
        if content != expected:
            raise ValueError(f"source differs from pinned upstream: {path}")


def fetch(name, pin, cache):
    directory = cache / f"{name}-{pin['sha256'][:16]}"
    if pin["kind"] == "file":
        if directory.exists():
            content = (directory / pin["filename"]).read_bytes()
            if digest(content) != pin["sha256"]:
                raise ValueError(f"source differs from pinned upstream: {directory}")
            verify_tree(directory, {pin["filename"]: (content, 0, False)})
            return directory
        files = {pin["filename"]: (download(pin), 0o644, False)}
    else:
        archives = cache / "downloads"
        archives.mkdir(exist_ok=True)
        archive_path = archives / f"{name}-{pin['sha256']}.tar.gz"
        if archive_path.exists():
            data = archive_path.read_bytes()
            if digest(data) != pin["sha256"]:
                raise ValueError(f"cached archive hash differs: {archive_path}")
        else:
            data = download(pin)
            with tempfile.NamedTemporaryFile(dir=archives, delete=False) as output:
                output.write(data)
                temporary = Path(output.name)
            temporary.replace(archive_path)
        files = archive_files(data)
        if directory.exists():
            verify_tree(directory, files)
            return directory
    with tempfile.TemporaryDirectory(prefix=f".fetch-{name}-", dir=cache) as staging:
        tree = Path(staging) / "source"
        tree.mkdir()
        for path, (content, mode, symlink) in files.items():
            target = tree / path
            target.parent.mkdir(parents=True, exist_ok=True)
            if symlink and os.name != "nt":
                target.symlink_to(content.decode())
            else:
                target.write_bytes(content)
                target.chmod(mode & 0o777)
        verify_tree(tree, files)
        tree.rename(directory)
    return directory


def verify(name, pin, cache):
    """Read only: absent or changed sources fail without download or repair."""
    directory = cache / f"{name}-{pin['sha256'][:16]}"
    if pin["kind"] == "file":
        content = (directory / pin["filename"]).read_bytes()
        if digest(content) != pin["sha256"]:
            raise ValueError(f"source differs from pinned upstream: {directory}")
        files = {pin["filename"]: (content, 0, False)}
    else:
        archive = cache / "downloads" / f"{name}-{pin['sha256']}.tar.gz"
        content = archive.read_bytes()
        if digest(content) != pin["sha256"]:
            raise ValueError(f"cached archive hash differs: {archive}")
        files = archive_files(content)
    verify_tree(directory, files)
    return directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name")
    parser.add_argument("--cache", type=Path, default=default_cache())
    parser.add_argument("--verify-only", action="store_true",
                        help="No downloads or writes; fail if sources are absent or changed")
    args = parser.parse_args()
    pins = json.loads(PINS.read_text(encoding="utf-8"))["sources"]
    if args.name not in pins:
        parser.error("unknown pinned source")
    cache = args.cache.resolve()
    if any(part.lower() in ("third_party", ".git") for part in cache.parts):
        parser.error("cache must be outside third-party checkouts")
    if args.verify_only:
        directory = verify(args.name, pins[args.name], cache)
    else:
        cache.mkdir(parents=True, exist_ok=True)
        directory = fetch(args.name, pins[args.name], cache)
    print(directory.as_posix())
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, tarfile.TarError) as error:
        print(f"JCE source verification failed: {error}", file=sys.stderr)
        sys.exit(1)
