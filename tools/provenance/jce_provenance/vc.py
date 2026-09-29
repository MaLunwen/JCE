"""W3C Verifiable Credentials 2.0 — the third L3 adapter.

§6.5.1 lists three standard adapters at L3: a C2PA manifest, an IPFS CID, and
a W3C VC 2.0 credential.  Two of them shipped and this one did not, and the
layer's own summary said "L0-L4 all landed" — a count of layers is not a count
of what is in them.  L3 was 2/3.

WHAT A CREDENTIAL IS FOR, AND WHAT IT IS NOT.  C2PA travels INSIDE an asset and
speaks to a viewer that already has the file.  A CID names bytes.  A credential
is the form the same claim takes when it has to be handed to a party that does
NOT have the artefact and does not run this engine: a store, a marketplace, an
auditor.  It says who asserts what about which digest, in a shape their
existing verifier already reads.

NOTHING HERE MINTS AN IDENTITY.  The issuer is whatever the caller passes — a
DID, an HTTPS URL, or nothing.  This module does not create a DID, resolve one,
hold a key or sign: signing is L2's job (jce_provenance.signing, an external
command), and a credential with no proof is an UNSIGNED credential, which is a
real and useful artefact as long as it says so.  `proof` is therefore absent
unless a detached L2 signature is supplied, and `signed: false` is reported
beside it rather than left to be inferred.

THE CLAIM IS THE SAME CLAIM.  Existence, time and signer — never authorship,
ownership or a licence grant (REQ-PROV-06).  The credential repeats that limit
in its own body rather than relying on a reader to remember it, because a
credential is precisely the artefact that travels away from its context.

VC 2.0, not 1.1: the context is https://www.w3.org/ns/credentials/v2, the date
field is `validFrom` (1.1's `issuanceDate` is gone), and `issuer` may be a
string or an object.  REQ-PROV-07 says the standard version is a field and the
adapter is independently upgradable, so the version constants sit at the top
and the mapping is one function.
"""
from __future__ import annotations

from .manifest import canonical_bytes, manifest_digest

VC_CONTEXT = "https://www.w3.org/ns/credentials/v2"
VC_VERSION = "2.0"

# A JCE-specific term set.  Named rather than inlined so an upgrade is one
# constant, and so a reader can tell our vocabulary from the W3C's.
JCE_CONTEXT = "https://jce.dev/credentials/provenance/v1"
CREDENTIAL_TYPE = "JceProvenanceCredential"


def credential(manifest, issuer=None, subject_id=None, signature=None,
               valid_from=None):
    """A VC 2.0 credential asserting this manifest's digest.

    `manifest`   a jce_provenance.manifest document.
    `issuer`     a DID or URL identifying who asserts it; None leaves the
                 credential ISSUERLESS and says so, rather than inventing one.
    `subject_id` how the artefact is addressed to the recipient -- an IPFS CID
                 (`ipfs://<cid>`) is the natural choice, since it names the
                 bytes the digest covers.
    `signature`  a detached L2 signature over the manifest, if there is one.
    `valid_from` an ISO-8601 instant; the manifest's own `created_at` is used
                 when it has one.
    """
    if not isinstance(manifest, dict) or "files" not in manifest:
        raise ValueError("credential() needs a provenance manifest")

    digest = manifest_digest(manifest)
    ai = manifest.get("ai") or {}

    subject = {
        "type": "SoftwareArtefact",
        "manifestDigest": digest,
        "fileCount": manifest.get("file_count"),
        "engineVersion": manifest.get("jce_version"),
        "buildVersion": manifest.get("build_version"),
        "changesetId": manifest.get("changeset_id"),
    }
    if subject_id:
        subject["id"] = subject_id
    # REQ-PROV-05: the AI disclosure travels WITH the credential.  A credential
    # that dropped it would be the most likely artefact to be read by someone
    # who never sees the manifest.
    if ai.get("model_id") or ai.get("prompt_sha256"):
        subject["aiDisclosure"] = {
            "modelId": ai.get("model_id"),
            "promptSha256": ai.get("prompt_sha256"),
            "statement": ai.get("disclosure"),
        }
    plan = manifest.get("plan") or {}
    if plan:
        subject["plan"] = {
            "frozenPlanHash": plan.get("plan_hash"),
            "materialisationAttestationSha256":
                plan.get("materialisation_attestation_sha256"),
            "materialisationAttested": plan.get("materialisation_attested"),
        }

    cred = {
        "@context": [VC_CONTEXT, JCE_CONTEXT],
        "type": ["VerifiableCredential", CREDENTIAL_TYPE],
        "credentialSubject": subject,
        # REQ-PROV-06, restated in the artefact that travels furthest from the
        # context that would otherwise carry it.
        "termsOfUse": {
            "type": "ProvenanceScope",
            "note": "This credential attests EXISTENCE, TIME and SIGNER of the "
                    "digest above. It does not establish authorship, "
                    "ownership or any licence grant, and it is not a "
                    "commercial-release clearance.",
        },
    }
    when = valid_from or manifest.get("created_at")
    if when:
        cred["validFrom"] = when            # VC 2.0; 1.1's issuanceDate is gone
    if issuer:
        cred["issuer"] = issuer

    if signature:
        # A DETACHED signature, carried as-is.  This module does not sign and
        # does not re-encode: L2 produced these bytes over the manifest, and
        # anything reshaped here would no longer verify against it.
        cred["proof"] = {
            "type": "DataIntegrityProof",
            "proofPurpose": "assertionMethod",
            "created": when,
            "verificationMethod": issuer,
            "detachedSignature": signature,
            "coversDigest": digest,
            "note": "the signature is over the MANIFEST bytes, not over this "
                    "credential; verify it with jce_provenance.signing "
                    "against the manifest whose digest is named above",
        }
    return cred


def describe(cred):
    """What this credential does and does not carry, as data.

    A credential with no issuer and no proof is a perfectly well-formed
    document that proves nothing, and it looks exactly like one that proves
    something.  Saying so is the point.
    """
    has_proof = bool((cred or {}).get("proof"))
    has_issuer = bool((cred or {}).get("issuer"))
    return {
        "spec": VC_VERSION,
        "context": VC_CONTEXT,
        "signed": has_proof,
        "issuer_named": has_issuer,
        "proves": (["existence of the digest", "the time asserted",
                    "the signer named in the proof"] if has_proof
                   else []),
        "proves_nothing_because": (
            None if has_proof else
            "there is no proof: this is an UNSIGNED credential. It records a "
            "claim in a standard shape; it does not evidence one. Sign the "
            "manifest at L2 and pass the signature in."),
        "never_proves": ["authorship", "ownership", "a licence grant"],
    }


def canonical(cred):
    """The credential's bytes, in the same canonical form manifests use, so a
    digest taken here and a digest taken by a consumer agree."""
    return canonical_bytes(cred)
