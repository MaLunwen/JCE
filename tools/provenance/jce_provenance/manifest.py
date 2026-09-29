"""L1: the manifest -- the document everything else signs, exports or anchors.

THE MANIFEST IS DETERMINISTIC AND SELF-CONTAINED.  Two properties, both load
bearing:

    deterministic   the same inputs produce byte-identical JSON.  Keys sorted,
                    no timestamp except the one passed in, LF endings, UTF-8.
                    A signature is over bytes; a manifest that re-serialised
                    differently on another machine would fail verification
                    there for no reason anyone could diagnose.

    self-contained  a verifier needs the manifest and the files, and nothing
                    else.  No lookup, no database, no network.  That is what
                    makes "check this build is what it says" a thing somebody
                    can do in five years.

AI DISCLOSURE IS A FIELD, NOT AN OPTION.  REQ-PROV-05.  When a model
contributed, the manifest records which model and the HASH of the prompt.  The
hash rather than the prompt itself, for two reasons that both matter: a prompt
carries the shape of unreleased work, and a hash still proves that a later
claimed prompt is the one that was used.  The field is always present -- an
absent key and "no model was involved" have to be distinguishable, so the value
is explicitly null rather than missing.

WHAT THE MANIFEST DOES NOT CLAIM.  REQ-PROV-06.  It records existence, time and
signer.  It is not an assertion of authorship or of a licence grant, and the
`legal_note` field says so inside every document this module writes, because
the manifest will outlive the conversation in which somebody was told.
"""
from __future__ import annotations

import hashlib
import json
import time
from pathlib import Path

from . import PROVENANCE_VERSION
from .hashing import cid_v1_for_file, digest_tree, sha256_bytes

LEGAL_NOTE = ("This manifest attests existence, time and signer only. It does "
              "not establish copyright ownership, authorship or a licence "
              "grant, and it is not legal advice.")


def prompt_hash(prompt_text):
    """The disclosure hash for a prompt.  None stays None."""
    if prompt_text is None:
        return None
    return "sha256:" + sha256_bytes(str(prompt_text).encode("utf-8"))


def build(root, paths, *, subject, author=None, license_id=None,
          creation_tool=None, ai_model_id=None, prompt=None,
          prompt_sha256=None, ai_contributions=None,
          jce_version=None, build_version=None, changeset_id=None,
          plan_hash=None, attestation_hash=None, created_at=None,
          modification_history=None, include_cid=True):
    """Assemble a manifest.  Pure: it reads files and returns a dict.

    `prompt` is the text, hashed here and never stored.  `prompt_sha256` is
    that same hash already computed, for a caller that HAS the hash and must
    not keep the text -- an attribution recorded when a scene was authored
    cannot carry the brief around with it, because a brief is the shape of an
    unreleased game and the tool that took it refuses to let it leave the
    machine.  Passing both is refused rather than resolved: two answers to
    one question is how a manifest starts describing something that did not
    happen, and picking a winner here would make which one silent.
    """
    if prompt is not None and prompt_sha256 is not None:
        if prompt_hash(prompt) != prompt_sha256:
            raise ValueError(
                "prompt and prompt_sha256 disagree: the text hashes to %s.  "
                "Pass one." % prompt_hash(prompt))
    prompt_digest = (prompt_sha256 if prompt is None else prompt_hash(prompt))
    rows, set_digest = digest_tree(root, paths)
    if include_cid:
        for r in rows:
            if r.get("sha256"):
                cid, exact = cid_v1_for_file(Path(root) / r["path"])
                r["ipfs_cid_v1"] = cid
                if not exact:
                    # Said out loud rather than omitted: a CID that is right
                    # for the bytes but not the one IPFS would assign is a
                    # correct value that will not resolve, and a consumer has
                    # to be able to tell those apart.
                    r["ipfs_cid_note"] = ("raw-codec CID of the whole file; "
                                          "IPFS chunks files over 256 KiB and "
                                          "would assign a different dag-pb CID")
    return {
        "provenance_version": PROVENANCE_VERSION,
        "subject": subject,
        "created_at": created_at or time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                                  time.gmtime()),
        "author": author,
        "license": license_id,
        "creation_tool": creation_tool or "jce-automation",
        # Always present, explicitly null when no model was involved.  REQ-PROV-05.
        "ai": {
            "model_id": ai_model_id,
            "prompt_sha256": prompt_digest,
            # Per-artefact detail, because the two fields above are ONE slot
            # and an artefact set can have several contributors.  Collapsing
            # three models into one name, or three prompt hashes into a hash
            # of hashes, would be a value that matches nothing that happened.
            # Always present (empty list when nobody recorded anything), so
            # "no model contributed" and "nobody wrote it down" do not read
            # the same -- that distinction is the whole point of this field.
            "contributions": list(ai_contributions or []),
            # Keyed on EITHER signal, not on model_id alone.  A recorded
            # attribution can arrive with a prompt hash and no model name,
            # and answering "no language model contributed" to that is the
            # one sentence this field exists to prevent.
            "disclosure": ("a language model contributed to this artefact"
                           if (ai_model_id or prompt_digest
                               or ai_contributions) else
                           "no language model contributed to this artefact"),
        },
        "jce_version": jce_version,
        "build_version": build_version,
        "changeset_id": changeset_id,
        "plan_hash": plan_hash,
        "attestation_hash": attestation_hash,
        "modification_history": list(modification_history or []),
        "files": rows,
        "file_count": len(rows),
        "set_sha256": set_digest,
        "legal_note": LEGAL_NOTE,
    }


def canonical_bytes(manifest) -> bytes:
    """The exact bytes a signature covers.

    Sorted keys, compact separators, UTF-8, one trailing newline.  A signature
    is over bytes and nothing else, so the function that produces those bytes
    is part of the format -- changing it invalidates every signature ever made,
    which is why it is one function with no options.
    """
    body = json.dumps(manifest, sort_keys=True, ensure_ascii=False,
                      separators=(",", ":"))
    return (body + "\n").encode("utf-8")


def manifest_digest(manifest) -> str:
    return "sha256:" + hashlib.sha256(canonical_bytes(manifest)).hexdigest()


def verify(root, manifest):
    """Re-hash every file the manifest names and report, per file.

    Returns {"ok": bool, "checked": n, "mismatched": [...], "missing": [...]}.
    Reports every problem rather than the first: a verifier's caller needs the
    whole list to decide whether one file drifted or the whole tree is from a
    different build.
    """
    mismatched, missing, checked = [], [], 0
    from .hashing import sha256_file
    for row in manifest.get("files") or []:
        virtual = row.get("path")
        expected = row.get("sha256")
        if not virtual or not expected:
            continue
        p = Path(root) / virtual
        if not p.is_file():
            missing.append(virtual)
            continue
        checked += 1
        actual = sha256_file(p)
        if actual != expected:
            mismatched.append({"path": virtual, "expected": expected,
                               "actual": actual})
    rows, set_digest = digest_tree(root, [r["path"] for r in
                                          (manifest.get("files") or [])
                                          if r.get("path")])
    return {
        "ok": not mismatched and not missing,
        "checked": checked,
        "mismatched": mismatched,
        "missing": missing,
        "set_sha256_expected": manifest.get("set_sha256"),
        "set_sha256_actual": set_digest,
        "set_matches": set_digest == manifest.get("set_sha256"),
    }
