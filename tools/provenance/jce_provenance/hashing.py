"""L0: content hashes, and the one content address that is worth computing
without a network.

TWO HASHES, TWO JOBS, AND CONFUSING THEM IS THE COMMON MISTAKE.

    identity     JCE already hashes assets for identity with xxhash -- fast,
                 excellent distribution, and NOT collision resistant.  That is
                 the right tool for a cache key and the wrong one for a proof:
                 an identity hash can be deliberately collided by anyone who
                 wants to, so "the hash matches" proves nothing about intent.

    proof        SHA-256.  Slower, and the only one of the two that means
                 anything in a sentence containing the word "signed".

So this module computes SHA-256 and says so in every field name.  Nothing here
replaces the engine's xxhash identity hashing, and nothing here should be used
as a cache key.

IPFS CIDv1 IS COMPUTED, NOT FETCHED.  A CID is a purely local function of the
bytes: multibase(base32) over multicodec(raw) + multihash(sha2-256).  Computing
it needs no daemon, no network and no dependency, and having it in the manifest
means the manifest is usable by anyone who later does pin the content -- while
JCE itself never talks to IPFS.
"""
from __future__ import annotations

import hashlib
from pathlib import Path

CHUNK = 1 << 20


def sha256_file(path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            block = fh.read(CHUNK)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _base32_nopad(data: bytes) -> str:
    """RFC 4648 base32, lower case, no padding -- multibase 'b'."""
    import base64
    return base64.b32encode(data).decode("ascii").rstrip("=").lower()


def _uvarint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def cid_v1_raw(data: bytes) -> str:
    """IPFS CIDv1 for `data` stored as a raw block.

    Deliberately the `raw` codec (0x55) rather than dag-pb: raw is what a
    single file under 256 KiB gets, and computing a dag-pb CID for a large file
    means reproducing IPFS's chunking parameters exactly.  Getting those subtly
    wrong produces a CID that looks correct, verifies against nothing, and is
    only discovered by somebody trying to fetch it.  So a large file reports
    the raw CID of its own bytes and says that it is not the chunked one.
    """
    digest = hashlib.sha256(data).digest()
    multihash = b"\x12\x20" + digest                 # sha2-256, 32 bytes
    cid = _uvarint(1) + _uvarint(0x55) + multihash   # version 1, raw codec
    return "b" + _base32_nopad(cid)


def cid_v1_for_file(path):
    """(cid, exact) -- `exact` is False when IPFS would have chunked the file
    and produced a different, dag-pb, CID."""
    data = Path(path).read_bytes()
    return cid_v1_raw(data), len(data) <= 262144


def digest_tree(root, paths):
    """SHA-256 of every named path, plus one digest over the whole set.

    The set digest is taken over "<sha>  <path>\\n" lines sorted by path, which
    is the same shape `sha256sum` writes, so it can be verified by a tool that
    has never heard of JCE.  Sorted because a set digest that depends on the
    order the caller happened to list its files in is not a digest of the set.
    """
    rows = []
    for virtual in sorted(paths):
        p = Path(root) / virtual
        if not p.is_file():
            rows.append({"path": virtual, "sha256": None, "missing": True})
            continue
        rows.append({"path": virtual, "sha256": sha256_file(p),
                     "bytes": p.stat().st_size})
    lines = "".join("%s  %s\n" % (r["sha256"], r["path"])
                    for r in rows if r.get("sha256"))
    return rows, hashlib.sha256(lines.encode("utf-8")).hexdigest()
