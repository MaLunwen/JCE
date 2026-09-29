"""L2: a detached signature, produced by a program the operator names.

WHY NO SIGNING LIBRARY IS VENDORED HERE.  CN-02 requires owner approval for a
native dependency, and a crypto library is the last place to add one casually:
it pulls a build toolchain into a pure-Python tool layer, it has to be kept
patched, and it would make `pip install` a precondition for running the
automation gate in CI.  None of that buys anything, because the operator
already has a signer.

WHY AN EXTERNAL COMMAND IS THE RIGHT SHAPE, NOT A COMPROMISE.  This repository
already solved the identical problem once: tools/llm/jce_llm.py reaches ANY
language model by running a program, and that single provider is the reason
"any model" is a claim the file can make rather than a list it has to chase.
Signing has the same structure.  ssh-keygen signs Ed25519 and is on nearly
every developer machine; minisign, age, openssl, gpg, an HSM wrapper and a
corporate signing service are all programs too.  Supporting one library would
support one; supporting a command supports all of them, with no code here.

    JCE_PROV_SIGN_COMMAND    e.g.
        ssh-keygen -Y sign -f ~/.ssh/id_ed25519 -n jce {input}
    JCE_PROV_VERIFY_COMMAND  e.g.
        ssh-keygen -Y verify -f allowed_signers -I me@example.com -n jce \\
                   -s {signature} {input}

{input} and {signature} are replaced at every occurrence.  The signature is
read from {signature} when the command names one, otherwise from stdout.

NO KEY EVER PASSES THROUGH THIS PROCESS.  The command references the key; this
module never reads one, never holds one, never logs one, and has no parameter
that could carry one.  That is the same rule CN-05 states for model
credentials, for the same reason.
"""
from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

SIGN_ENV = "JCE_PROV_SIGN_COMMAND"
VERIFY_ENV = "JCE_PROV_VERIFY_COMMAND"


class SigningUnavailable(Exception):
    """No signer is configured.  Distinct from "signing failed": one is a
    setup fact, the other is a result, and an agent branches differently on
    them."""


def _expand(template, mapping):
    """Split the template FIRST, then substitute into the resulting tokens.

    Substituting first and splitting after means our own temporary paths have
    to survive the splitter, and on Windows they do not: a path with spaces
    needs quoting, and a quoted path only survives posix-mode splitting, which
    in turn eats the backslashes in the operator's own key path.  Splitting
    first sidesteps the whole problem -- `{input}` and `{signature}` contain no
    spaces, no quotes and no backslashes, so they are always exactly one token,
    and the path replaces that token verbatim.

    MEASURED.  The first version split last, in non-posix mode on Windows, so
    the operator's quoted key path kept its quotes and ssh-keygen answered

        Too few arguments for sign: missing namespace

    which names the wrong argument entirely: `-n jce` was present and correct,
    and the quotes on `-f` were what it could not parse.  An error that points
    at the wrong flag is worse than a crash, and this only surfaced because
    the path was run rather than reviewed.

    posix=True so a quoted path works.  An operator on Windows writes the key
    path with forward slashes or quotes it; both then behave.
    """
    argv = shlex.split(template, posix=True)
    out = []
    for token in argv:
        for key, value in mapping.items():
            token = token.replace("{%s}" % key, value)
        out.append(token)
    return out


def sign(payload: bytes, command=None, timeout=120):
    """Sign `payload`.  Returns {"signature": str, "command": str, "via": ...}.

    Raises SigningUnavailable when nothing is configured -- never a fabricated
    or empty signature, which would verify against nothing while looking like
    a signed artefact.
    """
    cmd = command or os.environ.get(SIGN_ENV, "").strip()
    if not cmd:
        raise SigningUnavailable(
            "no signer configured: set %s to a command that signs a file, e.g. "
            "'ssh-keygen -Y sign -f ~/.ssh/id_ed25519 -n jce {input}'" % SIGN_ENV)

    with tempfile.TemporaryDirectory(prefix="jce-prov-") as tmp:
        inp = Path(tmp) / "manifest.json"
        inp.write_bytes(payload)
        sig = Path(tmp) / "manifest.json.sig"
        argv = _expand(cmd, {"input": str(inp), "signature": str(sig),
                             "output": str(sig)})
        try:
            p = subprocess.run(argv, capture_output=True, text=True,
                               timeout=timeout, encoding="utf-8",
                               errors="replace")
        except FileNotFoundError:
            raise SigningUnavailable("the signing command %r is not on PATH"
                                     % argv[0])
        if p.returncode != 0:
            raise RuntimeError("the signing command exited %d: %s"
                               % (p.returncode, (p.stderr or p.stdout or "")[-400:]))
        # ssh-keygen writes <input>.sig next to the input regardless of the
        # placeholder, so both spellings are looked for before falling back to
        # stdout.  A signer that wrote nowhere and exited 0 must not be read as
        # an empty signature -- that is the one failure that would produce an
        # artefact claiming to be signed and verifying against nothing.
        for cand in (sig, Path(str(inp) + ".sig")):
            if cand.is_file() and cand.stat().st_size:
                return {"signature": cand.read_text(encoding="utf-8").strip(),
                        "command": argv[0], "via": "file"}
        if (p.stdout or "").strip():
            return {"signature": p.stdout.strip(), "command": argv[0],
                    "via": "stdout"}
        raise RuntimeError(
            "the signing command exited 0 but produced no signature -- it "
            "wrote neither a .sig file nor anything on stdout")


def verify(payload: bytes, signature: str, command=None, timeout=120):
    """Verify a detached signature.  Returns {"ok": bool, "detail": str}."""
    cmd = command or os.environ.get(VERIFY_ENV, "").strip()
    if not cmd:
        raise SigningUnavailable(
            "no verifier configured: set %s" % VERIFY_ENV)
    with tempfile.TemporaryDirectory(prefix="jce-prov-") as tmp:
        inp = Path(tmp) / "manifest.json"
        inp.write_bytes(payload)
        sig = Path(tmp) / "manifest.json.sig"
        sig.write_text((signature or "").rstrip() + "\n", encoding="utf-8")
        argv = _expand(cmd, {"input": str(inp), "signature": str(sig)})
        try:
            # THE MESSAGE GOES ON STDIN AS WELL AS TO A FILE.
            #
            # ssh-keygen -Y verify reads the signed data from STDIN and takes
            # no file argument for it -- unlike -Y sign, which does.  The first
            # version passed the payload only as a file, so ssh-keygen verified
            # an EMPTY message and answered
            #
            #     Could not verify signature.  incorrect signature
            #
            # for a manifest that was perfectly intact.  That failure is
            # indistinguishable from a real tamper, which makes it the worst
            # possible one: the tamper controls passed, the honest case failed,
            # and the whole layer would have read as "signing does not work".
            #
            # Feeding both costs nothing and covers either convention.
            p = subprocess.run(argv, input=payload, capture_output=True,
                               timeout=timeout)
        except FileNotFoundError:
            raise SigningUnavailable("the verify command %r is not on PATH"
                                     % argv[0])
        detail = ((p.stdout or b"") + (p.stderr or b"")).decode(
            "utf-8", "replace").strip()
        return {"ok": p.returncode == 0,
                "exit_code": p.returncode,
                "detail": detail[-400:]}


def available():
    """What is configured, without running anything."""
    return {"sign_configured": bool(os.environ.get(SIGN_ENV, "").strip()),
            "verify_configured": bool(os.environ.get(VERIFY_ENV, "").strip()),
            "sign_env": SIGN_ENV, "verify_env": VERIFY_ENV}
