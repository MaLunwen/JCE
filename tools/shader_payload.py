"""Export an existing shader payload through the sole native reflection parser.

No shader compiler, engine, graphics device or BGFX container parser is used
here. The supplied inspector must support --json --payload-hex. Output is
exclusive: an existing file, directory or symlink is never replaced.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess

DEFAULT_LIMIT = 1024 * 1024
MAX_LIMIT = 64 * 1024 * 1024


def _digest_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_bounded(path, limit):
    with Path(path).open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError("shader container exceeds the byte limit")
    return data


def _owned_identity(info):
    return (info.st_dev, info.st_ino) if info.st_ino else None


def _write_new(output, payload):
    identity = None
    try:
        stream = output.open("xb")
        with stream:
            identity = _owned_identity(os.fstat(stream.fileno()))
            if stream.write(payload) != len(payload):
                raise OSError("short shader payload write")
            stream.flush()
    except BaseException:
        # Never unlink a pre-existing path or a replacement made by another
        # owner. Unknown file identity retains the partial file conservatively.
        if identity is not None:
            try:
                current = output.lstat()
                if stat.S_ISREG(current.st_mode) and _owned_identity(current) == identity:
                    output.unlink()
            except OSError:
                pass
        raise


def export_payload(inspector, source, output, *, max_bytes=DEFAULT_LIMIT):
    """Return exact CPU artifact identities after an exclusive payload export."""
    if type(max_bytes) is not int or not 0 < max_bytes <= MAX_LIMIT:
        raise ValueError("byte limit must be an integer from 1 to 67108864")
    inspector, source, output = map(Path, (inspector, source, output))
    container = _read_bounded(source, max_bytes)
    inspector_hash = _digest_file(inspector)
    result = subprocess.run(
        [str(inspector.resolve()), str(source.resolve()), "--json", "--payload-hex"],
        capture_output=True, check=True, timeout=30)
    if len(result.stdout) > max_bytes * 2 + 131072:
        raise ValueError("shader inspector output exceeds the byte limit")
    report = json.loads(result.stdout)
    if not isinstance(report, dict):
        raise ValueError("shader inspector did not return an object")
    offset, size = report.get("code_offset"), report.get("code_size")
    if (type(offset) is not int or type(size) is not int or offset < 0 or size < 0
            or type(report.get("container_size")) is not int
            or report["container_size"] != len(container)
            or offset > len(container) or size > len(container) - offset):
        raise ValueError("shader inspector returned an invalid payload range")
    encoded = report.get("payload_hex")
    if (not isinstance(encoded, str) or len(encoded) != size * 2
            or any(c not in "0123456789abcdef" for c in encoded)):
        raise ValueError("shader inspector returned invalid payload hex")
    payload = bytes.fromhex(encoded)
    if payload != container[offset:offset + size]:
        raise ValueError("shader inspector payload differs from the input range")
    if _read_bounded(source, max_bytes) != container:
        raise ValueError("shader container changed during inspection")
    if _digest_file(inspector) != inspector_hash:
        raise ValueError("shader inspector changed during inspection")
    _write_new(output, payload)
    return dict(schema=1, status="CPU_SHADER_PAYLOAD_EXPORTED",
        inspector=str(inspector.resolve()), inspector_sha256=inspector_hash,
        source=str(source.resolve()), container_bytes=len(container),
        container_sha256=hashlib.sha256(container).hexdigest(),
        code_offset=offset, payload_bytes=size,
        payload_sha256=hashlib.sha256(payload).hexdigest(), output=str(output.resolve()),
        scope="Existing native parser payload bytes only; no compilation, active draw, GPU or performance proof")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inspector", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--max-bytes", type=int, default=DEFAULT_LIMIT)
    args = parser.parse_args(argv)
    result = export_payload(args.inspector, args.input, args.out, max_bytes=args.max_bytes)
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
