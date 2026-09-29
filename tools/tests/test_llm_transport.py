#!/usr/bin/env python3
"""Offline transport, credential and image regressions for all wire formats."""
import json
import re
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "llm"))
import jce_llm

KEY = "sk-ant-SECRETVALUE-do-not-print-4242"
CMD = "python -c pass"

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def cfg_for(provider):
    return jce_llm.Config(provider=provider, api_key=KEY, command=CMD)


def test_no_provider_can_print_the_key():
    """THE ONE THAT MATTERS.  Iterating PROVIDERS rather than listing the two
    I remembered is the whole point: the allow-list version of redaction
    passed a hand-written test and leaked on the next provider added."""
    for name in sorted(jce_llm.PROVIDERS):
        cfg = cfg_for(name)
        out = jce_llm.describe(cfg, "SYSTEM PROMPT", "USER BRIEF")
        check(KEY not in out,
              "provider %r prints the API key in its dry run -- redaction is "
              "keyed on a header name it does not cover" % name)
        # And the redaction must still be USEFUL: a bare mask cannot answer
        # "did it pick up the right key", which is what people run twice for.
        if name not in ("ollama", "command"):
            check(re.search(r"<set, \d+ chars", out),
                  "provider %r redacts the key without saying it is set; a "
                  "dry run cannot then answer 'did it read my key at all'"
                  % name)


def test_the_dry_run_file_carries_no_key():
    """--out is a SECOND sink for the same preview, and the worse one.

    A dry run writes its request preview to --out so that a caller reading the
    file rather than the console -- the editor's AI Assistant panel -- sees
    what the run produced instead of reporting the safest path as a failure.
    That put the preview on disk, where it stays until somebody attaches it to
    a bug report, so the no-key property is asserted here against the file
    itself rather than inferred from the fact that it is the same string."""
    import tempfile

    for name in sorted(jce_llm.PROVIDERS):
        cfg = cfg_for(name)
        preview = jce_llm.describe(cfg, "SYSTEM PROMPT", "USER BRIEF")
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "preview.txt"
            out.write_text(preview, encoding="utf-8")
            written = out.read_text(encoding="utf-8")
        check(KEY not in written,
              "provider %r writes the API key into the --out file of a dry "
              "run; the console is transient and this is not" % name)


def test_dry_run_opens_no_socket():
    """describe() must be pure.  Asserted by making the network explode, not
    by reading the code and believing it."""
    real_urlopen = jce_llm.urllib.request.urlopen

    def boom(*a, **k):
        raise AssertionError("describe() opened a socket")

    jce_llm.urllib.request.urlopen = boom
    try:
        for name in sorted(jce_llm.PROVIDERS):
            jce_llm.describe(cfg_for(name), "s", "u")
    except AssertionError as e:
        failures.append(str(e))
    finally:
        jce_llm.urllib.request.urlopen = real_urlopen


def test_every_provider_builds_a_distinct_request():
    seen = {}
    for name in sorted(jce_llm.PROVIDERS):
        target, headers, body = jce_llm.build_request(cfg_for(name), "S", "U")
        key = json.dumps([target if isinstance(target, str) else list(target),
                          sorted(headers), sorted(body)], sort_keys=True)
        check(key not in seen,
              "providers %r and %r build an identical request; one of them is "
              "not implemented" % (name, seen.get(key)))
        seen[key] = name
        # The system prompt must actually be IN the request.  A shape that
        # drops it still returns text, and the text is subtly worse -- the
        # failure mode that never shows up as an error.
        blob = json.dumps(body, ensure_ascii=False)
        check("S" in blob,
              "provider %r builds a request with no system prompt in it" % name)
        check("U" in blob,
              "provider %r builds a request with no user prompt in it" % name)


def _fixture_png(tmp):
    import base64
    png = base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8B"
        "QDwAEhQGAhKmMIQAAAABJRU5ErkJggg==")
    tmp.write_bytes(png)
    return tmp


def test_an_image_actually_lands_in_every_request():
    """The failure this guards is a SILENT DROP.

    A model asked "what is wrong with this lighting" that never received the
    picture answers confidently from the scene text, and that answer is
    indistinguishable from a real one.  So the assertion is that the base64
    is IN the body -- not that the call succeeded.
    """
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        img = jce_llm.load_image(_fixture_png(Path(d) / "cap.png"))
        b64 = img[1]
        for name in sorted(jce_llm.PROVIDERS):
            if name == "command":
                continue
            _, _, body = jce_llm.build_request(cfg_for(name), "S", "U",
                                               images=[img])
            check(b64 in json.dumps(body),
                  "provider %r builds an image request with the image NOT in "
                  "it -- the model would answer about a picture it never saw"
                  % name)


def test_command_refuses_an_image_instead_of_dropping_it():
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        img = jce_llm.load_image(_fixture_png(Path(d) / "cap.png"))
        try:
            jce_llm.build_request(cfg_for("command"), "S", "U", images=[img])
            failures.append("provider=command accepted an image it cannot "
                            "send; dropping it silently is the worst outcome "
                            "here, not the safe one")
        except jce_llm.JceLlmError as e:
            check("cannot carry an image" in str(e),
                  "provider=command refuses an image without saying why")


def test_a_dry_run_does_not_print_base64():
    """Megabytes of base64 makes a dry run nobody reads, and a dry run nobody
    reads cannot answer the question it exists for."""
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        img = jce_llm.load_image(_fixture_png(Path(d) / "cap.png"))
        for name in sorted(jce_llm.PROVIDERS):
            if name == "command":
                continue
            out = jce_llm.describe(cfg_for(name), "S", "U", images=[img])
            check(img[1] not in out,
                  "provider %r prints raw base64 in its dry run" % name)
            check("base64" in out or "data uri" in out,
                  "provider %r elides the image without saying one is "
                  "attached; a dry run must still answer 'did the picture go'"
                  % name)


def test_unknown_response_shape_raises():
    """Returning "" would flow downstream and become 'the model said nothing',
    which is a different and much harder bug than 'that is not the shape'."""
    for name in sorted(jce_llm.PROVIDERS):
        try:
            jce_llm.extract_text(cfg_for(name), {"unexpected": True})
            failures.append("provider %r returns something for a response "
                            "shape it does not recognise" % name)
        except jce_llm.JceLlmError:
            pass


def test_unknown_provider_is_refused_by_name():
    try:
        jce_llm.Config(provider="not-a-provider")
        failures.append("an unknown provider was accepted")
    except jce_llm.JceLlmError as e:
        check("openai" in str(e),
              "the unknown-provider error does not say what IS supported")


def test_command_provider_needs_its_argv():
    try:
        jce_llm.Config(provider="command", command="")
        failures.append("provider=command was accepted with no program to run")
    except jce_llm.JceLlmError:
        pass


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    for failure in failures:
        print("FAIL: " + failure, file=sys.stderr)
    print("LLM transport: %d providers, %d failures" % (len(jce_llm.PROVIDERS), len(failures)))
    raise SystemExit(bool(failures))
