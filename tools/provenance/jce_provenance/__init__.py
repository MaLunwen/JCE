"""Provenance for JCE artefacts: what exists, when, and who says so.

WHAT THIS IS FOR, AND WHAT IT IS NOT.  A blockchain does not make a game engine
better.  What has value is narrower and quite concrete: being able to say that
a specific version of a specific asset, scene or build existed at a specific
time, hashed to a specific value, and was signed by a specific party.  Every
layer here serves that sentence and nothing else.

    L0  hash        content hashes.  No dependency, no configuration.
    L1  manifest    who, what, when, which engine, which changeset, and --
                    when a model was involved -- which model and which prompt.
    L2  signature   a detached signature over the manifest, produced by a
                    PROGRAM the operator names.  No signing library is vendored
                    here (CN-02: a native crypto dependency needs owner
                    approval), and the external-command shape is the same one
                    tools/llm/jce_llm.py uses to reach any model.
    L3  adapters    C2PA-shaped manifest export and IPFS CIDv1 computation,
                    both pure-Python, both optional.
    L4  anchor      formats (hash, signature, time) for anchoring on any chain.
                    It does not transmit.  Only a hash ever leaves, and only
                    when a human runs the send.

EVERY LAYER IS OPTIONAL AND THE ENGINE KNOWS ABOUT NONE OF THEM.  PR-08 and
REQ-PROV-02/03.  Deleting this whole directory leaves JCE complete; nothing
under engine/ or editor/ imports it, and no build or package step requires it.

WHAT IS FORBIDDEN HERE, PERMANENTLY.  REQ-PROV-04: no token, no NFT, no wallet,
no transfer, no marketplace, no price.  The anchor layer accepts a digest and
emits a payload; it has no key that can spend anything and no code path that
could acquire one.  A pull request adding any of that is out of scope for this
module by construction, not by convention.

AND WHAT A SIGNATURE DOES NOT MEAN.  REQ-PROV-06: this proves existence, time
and signer.  It does not establish authorship, ownership or a licence, and no
output here is worded as if it does.
"""

PROVENANCE_VERSION = 1

__all__ = ["PROVENANCE_VERSION"]
