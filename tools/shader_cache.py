"""Content-addressed shader compilation; keep native build outputs isolated."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

_spec = importlib.util.spec_from_file_location(
    "jce_shader_cache_lock", Path(__file__).parent / "build" / "cache_lock.py")
_locks = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_locks)
cache_locks = _locks.cache_locks

SCHEMA = 1


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            value.update(block)
    return value.hexdigest()


def fingerprint(compiler, arguments, inputs):
    normalized = list(arguments)
    if normalized.count("-o") != 1:
        raise ValueError("exactly one -o output argument is required")
    index = normalized.index("-o") + 1
    if index == len(normalized):
        raise ValueError("missing output argument")
    normalized[index] = "<output>"
    files = sorted({str(Path(item).resolve()) for item in inputs})
    record = {"schema": SCHEMA, "compiler": digest(compiler),
              "arguments": normalized,
              "inputs": [(item, digest(item)) for item in files]}
    key = hashlib.sha256(json.dumps(record, sort_keys=True).encode()).hexdigest()
    return key, index


def publish(source, destination):
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(dir=destination.parent, prefix=".shader-")
    os.close(descriptor)
    try:
        shutil.copyfile(source, temporary)
        os.replace(temporary, destination)
    finally:
        Path(temporary).unlink(missing_ok=True)


def publish_output(blob, output, checksum):
    # Ninja restat can keep downstream packs clean when the blob is unchanged.
    if not output.is_file() or digest(output) != checksum:
        publish(blob, output)


def compile_cached(compiler, arguments, inputs, cache):
    key, index = fingerprint(compiler, arguments, inputs)
    output = Path(arguments[index])
    cache = Path(cache)
    blob = cache / (key + ".bin")
    checksum = cache / (key + ".sha256")
    with cache_locks([blob], cache / "locks"):
        if blob.is_file() and checksum.is_file():
            blob_checksum = checksum.read_text(encoding="ascii").strip()
            if digest(blob) == blob_checksum:
                publish_output(blob, output, blob_checksum)
                return "hit"
        cache.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=cache, prefix="compile-") as directory:
            temporary = Path(directory) / "shader.bin"
            actual = list(arguments)
            actual[index] = str(temporary)
            subprocess.run([str(compiler), *actual], check=True)
            if not temporary.is_file():
                raise RuntimeError("shader compiler did not produce an output")
            # A concurrently edited input must not be published under the old key.
            if fingerprint(compiler, arguments, inputs)[0] != key:
                raise RuntimeError("shader compilation inputs changed during compilation")
            stamp = Path(directory) / "checksum"
            blob_checksum = digest(temporary)
            stamp.write_text(blob_checksum + "\n", encoding="ascii")
            publish(temporary, blob)
            publish(stamp, checksum)
            publish_output(blob, output, blob_checksum)
        return "miss"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cache", required=True)
    parser.add_argument("--input", action="append", default=[])
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    arguments = args.arguments
    if arguments[:1] == ["--"]:
        arguments = arguments[1:]
    print("ShaderCache:", compile_cached(args.compiler, arguments, args.input, args.cache))


if __name__ == "__main__":
    main()
