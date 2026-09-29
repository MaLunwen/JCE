"""L4: anchoring -- and the exact, deliberate limits of what that means here.

WHAT ANCHORING IS.  Publishing one digest, plus a timestamp, somewhere whose
history is hard to rewrite, so that "this artefact existed by then" can be
checked later by someone who does not trust you.  That is the entire value
proposition, and it is a real one.

WHAT ANCHORING IS NOT, AND WHAT THIS MODULE THEREFORE CANNOT DO.  REQ-PROV-04:
no token, no NFT, no wallet, no transfer, no marketplace, no price.  This
module builds a payload and stops.  It holds no key, opens no socket, and has
no parameter through which a key could be passed -- so it cannot spend, mint or
transfer anything, and that is a property of the code rather than a promise in
a document.

SENDING IS SOMEBODY ELSE'S STEP, ON PURPOSE.  `payload()` returns what to
publish, in a shape any chain client accepts.  Transmitting it is a NETWORK
operation, which sits at the approval tier of the permission model, and a
digest leaving the machine is a decision for a person rather than a default.
The same reasoning as tools/llm/jce_llm.py, where nothing is sent without an
explicit --send.

WHY THE PAYLOAD IS CHAIN-NEUTRAL.  Three encodings cover essentially every
chain that can carry arbitrary bytes, and none of them needs a client library:

    op_return     32 raw bytes, the classic Bitcoin OP_RETURN shape
    hex           0x-prefixed, what an EVM data field takes
    memo          base64, what a memo/note field on most account chains takes

A chain client that understands any one of these can anchor a JCE artefact
without JCE ever depending on that chain.
"""
from __future__ import annotations

import base64
import time

# A digest is 32 bytes.  Anything that does not parse as one is refused rather
# than truncated or padded: an anchor of the wrong bytes is worse than no
# anchor, because it looks like proof.
DIGEST_BYTES = 32


def _digest_bytes(digest: str) -> bytes:
    raw = (digest or "").strip()
    if raw.startswith("sha256:"):
        raw = raw[len("sha256:"):]
    raw = raw.removeprefix("0x")
    try:
        data = bytes.fromhex(raw)
    except ValueError:
        raise ValueError("digest is not hexadecimal: %r" % digest)
    if len(data) != DIGEST_BYTES:
        raise ValueError("a sha-256 digest is %d bytes, got %d"
                         % (DIGEST_BYTES, len(data)))
    return data


def payload(digest, *, signature=None, at=None, label="jce"):
    """The bytes to publish, in three interchangeable encodings.

    `signature` is NOT embedded.  A signature is far larger than a chain's
    data field and putting it on-chain buys nothing: the signature is checked
    against the manifest, which the verifier already has, and the chain only
    has to establish that the digest existed by a certain time.  Its presence
    is recorded as a flag so a verifier knows to ask for it.
    """
    data = _digest_bytes(digest)
    stamp = at or time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    return {
        "label": label,
        "digest_sha256": data.hex(),
        "prepared_at": stamp,
        "signed": bool(signature),
        "encodings": {
            "op_return": data.hex(),
            "hex": "0x" + data.hex(),
            "memo": base64.b64encode(data).decode("ascii"),
        },
        "bytes": DIGEST_BYTES,
        "transmitted": False,
        "note": ("this payload has NOT been transmitted. Publishing it is a "
                 "network operation and a human decision; JCE has no chain "
                 "client, no key and no way to spend anything."),
        "verification_recipe": [
            "recompute the manifest's canonical bytes",
            "sha-256 them and compare with digest_sha256",
            "find the transaction carrying those bytes; its block time is the "
            "upper bound on when the artefact existed",
            "check the detached signature against the manifest separately -- "
            "the chain does not carry it",
        ],
        "limits": ("anchoring proves existence and time, and the signature "
                   "proves the signer. Neither proves authorship, ownership "
                   "or a licence."),
    }


def describe():
    """What this layer can and cannot do, as data -- so the answer is the same
    in the CLI, in MCP and in the protocol document."""
    return {
        "can": ["compute an anchor payload from a digest",
                "emit it in op_return, hex and memo encodings"],
        "cannot": ["transmit it", "hold a key", "mint or transfer a token",
                   "act as a wallet", "price anything"],
        "reason": "REQ-PROV-04, and the absence of any code path that could",
        # SAID OUT LOUD because the shape of this module invites the opposite
        # reading: a layer that stops one step short of sending looks like
        # half a feature, and the obliging thing to do with half a feature is
        # to finish it.  It is not half of anything.
        "status": "decided, not pending",
        "decided": "2026-09-20, by the owner: the payload-only design is the "
                   "terminal state. Connecting a real chain is a DEPLOYMENT "
                   "decision -- which chain, who holds the key, who pays the "
                   "fee -- and none of those three belong to this layer. Do "
                   "not add a transmitter here without that decision being "
                   "made again.",
    }
