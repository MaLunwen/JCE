"""L3: a C2PA-shaped export, and an honest statement of what it is.

WHAT THIS PRODUCES.  The JSON content of a C2PA claim: the assertions, the
ingredient hashes and -- the part that matters for generated content -- the AI
disclosure action with its digitalSourceType.  A C2PA validator's data model
recognises this shape, and it can be handed to the C2PA toolkit to be signed
and embedded.

WHAT THIS DOES NOT PRODUCE, STATED IN EVERY DOCUMENT IT WRITES.  A conforming
C2PA manifest is a COSE_Sign1 structure embedded in the asset's own container
(JUMBF in a JPEG, a box in an MP4).  Producing one requires the C2PA toolkit
and a certificate from a recognised chain -- neither of which may be added here
without owner approval (CN-02).  So the export carries `conformance:
"claim-content-only"` and says so in prose as well.  The failure being avoided
is specific and expensive: a file described as "has C2PA provenance" that no
validator accepts, discovered by the first person who tries to validate it,
after it shipped.

THE STANDARD MOVES.  REQ-PROV-07: C2PA 2.x, and adapters must be independently
upgradable.  The version is a field, and the mapping is one function.
"""
from __future__ import annotations

# The controlled value for content generated with a model, from the IPTC
# digital source type vocabulary that C2PA references.  Spelled out rather
# than abbreviated because a wrong value here is an incorrect disclosure.
TRAINED_ALGORITHMIC_MEDIA = (
    "http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia")
DIGITAL_CAPTURE = (
    "http://cv.iptc.org/newscodes/digitalsourcetype/digitalCapture")
COMPOSITE_WITH_TRAINED = (
    "http://cv.iptc.org/newscodes/digitalsourcetype/compositeWithTrainedAlgorithmicMedia")

SPEC_VERSION = "2.1"


def export(manifest, *, title=None, generator="JCE Automation"):
    """A C2PA-shaped claim built from a JCE provenance manifest."""
    ai = manifest.get("ai") or {}
    model = ai.get("model_id")

    actions = [{
        "action": "c2pa.created",
        "softwareAgent": manifest.get("creation_tool") or generator,
        "when": manifest.get("created_at"),
    }]
    if model:
        actions.append({
            "action": "c2pa.created",
            "softwareAgent": model,
            "digitalSourceType": COMPOSITE_WITH_TRAINED,
            "when": manifest.get("created_at"),
            # The prompt itself is deliberately absent; only its hash travels.
            # A prompt carries the shape of unreleased work, and the hash still
            # settles any later dispute about which prompt was used.
            "parameters": {"prompt_sha256": ai.get("prompt_sha256")},
        })
    else:
        actions[0]["digitalSourceType"] = DIGITAL_CAPTURE

    assertions = [
        {"label": "c2pa.actions", "data": {"actions": actions}},
        {"label": "stds.schema-org.CreativeWork",
         "data": {"@context": "https://schema.org",
                  "@type": "CreativeWork",
                  "author": ([{"@type": "Person", "name": manifest["author"]}]
                             if manifest.get("author") else []),
                  "license": manifest.get("license")}},
        {"label": "org.jce.build",
         "data": {"jce_version": manifest.get("jce_version"),
                  "build_version": manifest.get("build_version"),
                  "changeset_id": manifest.get("changeset_id"),
                  "plan_hash": manifest.get("plan_hash"),
                  "attestation_hash": manifest.get("attestation_hash")}},
    ]

    ingredients = [
        {"title": row.get("path"),
         "relationship": "componentOf",
         "hash": {"alg": "sha256", "hash": row.get("sha256")},
         "ipfs_cid_v1": row.get("ipfs_cid_v1")}
        for row in (manifest.get("files") or []) if row.get("sha256")
    ]

    return {
        "claim_generator": generator,
        "claim_generator_info": [{"name": generator}],
        "spec_version": SPEC_VERSION,
        "title": title or manifest.get("subject"),
        "assertions": assertions,
        "ingredients": ingredients,
        "set_sha256": manifest.get("set_sha256"),
        # The two fields that keep this from being mistaken for the real thing.
        "conformance": "claim-content-only",
        "conformance_note": (
            "this is the CONTENT of a C2PA claim, not a conforming manifest: a "
            "conforming one is a COSE_Sign1 structure embedded in the asset "
            "container, which needs the C2PA toolkit and a certificate from a "
            "recognised chain. Hand this to that toolkit to sign and embed; do "
            "not describe the asset as C2PA-signed until you have."),
        "legal_note": manifest.get("legal_note"),
    }
