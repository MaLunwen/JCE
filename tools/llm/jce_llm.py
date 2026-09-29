#!/usr/bin/env python3
"""One LLM interface for the JCE tooling.  Any model.

WHAT THIS IS FOR.  The AI CLI is meant to help author scenes and look through
the SDK/API layer, and to do that it has to talk to a model -- whichever one
the team has.  Five shapes cover the field:

    openai      POST <base>/chat/completions  -- OpenAI, Azure OpenAI, and
                every server that speaks it: vLLM, llama.cpp, LM Studio,
                Ollama's /v1, OpenRouter, Together, Groq, DeepSeek, Qwen,
                Mistral, Moonshot, Zhipu.  One --base-url away.
    anthropic   POST <base>/v1/messages       -- Claude.
    gemini      POST <base>/models/<m>:generateContent -- Google.
    ollama      POST <base>/api/chat          -- the local daemon natively:
                no key, no account, localhost, `ollama pull` is the setup.
    command     runs a PROGRAM: prompt on stdin, answer on stdout.

That last one is why "any model" is a claim this file can make rather than a
list it has to keep chasing.  If a model can be reached by any program on this
machine -- `ollama run`, `llm -m`, `llama-cli`, a shell script somebody wrote
around an internal endpoint -- then every JCE tool can reach it too, with no
code here and no network.

Everything above this file works in terms of `system` + `user` + a text
answer, and never sees which of the five it got.

NOTHING LEAVES THIS MACHINE UNLESS YOU SAY SO.  --dry-run is the DEFAULT and
prints the exact request that WOULD be sent, headers redacted.  Sending needs
an explicit --send.  That is not ceremony: a scene brief carries the shape of
an unreleased game, and the tool that ships it to a third party should not be
the one whose default did it.

NO NEW DEPENDENCY.  urllib from the standard library.  This tree already
refuses vendored HTTP stacks in its lint, and a tool that needs `pip install`
before it runs is a tool that does not run in CI.

CONFIGURATION, all overridable on the command line:

    JCE_LLM_PROVIDER    openai|anthropic|gemini|ollama|command
                                                    (default: anthropic)
    JCE_LLM_COMMAND     provider=command: the argv  (no default)
    JCE_LLM_BASE_URL    endpoint root               (default: the provider's)
    JCE_LLM_MODEL       model id                    (default: the provider's)
    JCE_LLM_API_KEY     credential                  (no default, ever)
    JCE_LLM_MAX_TOKENS  answer budget               (default: 8192)

The key is read from the environment and NEVER from a file in the repo, never
written to one, and never printed -- redact_headers() is what the dry run
shows, and there is a test that a real key does not appear in its output.
"""
import argparse
import json
import base64
import os
import re
import shlex
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

PROVIDERS = {
    "openai": {
        "base_url": "https://api.openai.com/v1",
        "path":     "/chat/completions",
        "model":    "gpt-4o",
    },
    "anthropic": {
        "base_url": "https://api.anthropic.com",
        "path":     "/v1/messages",
        "model":    "claude-sonnet-5",
    },
    # Ollama's NATIVE api.  It also speaks OpenAI at /v1, and either works --
    # this one is here because it is the zero-configuration path: no key, no
    # account, localhost, and `ollama pull <model>` is the whole setup.  For a
    # local model that is the difference between "supported" and "supported if
    # you first work out the base URL".
    "ollama": {
        "base_url": "http://localhost:11434",
        "path":     "/api/chat",
        "model":    "llama3.2",
    },
    "gemini": {
        "base_url": "https://generativelanguage.googleapis.com/v1beta",
        "path":     "",          # the model id is IN the path; see build_request
        "model":    "gemini-2.0-flash",
    },
    # THE ESCAPE HATCH, and the reason "any model" is a claim this file can
    # actually make.  It runs a program: the prompt goes in on stdin, the
    # answer comes back on stdout.  `ollama run llama3`, `llm -m mistral`,
    # `llama-cli -m model.gguf -p -`, or a shell script somebody wrote around
    # an internal endpoint -- if a program on this machine can reach the
    # model, so can every JCE tool, with no code here.
    #
    # It is also the only provider that needs no key and no network, which
    # makes it the one an air-gapped or offline build can use.
    "command": {
        "base_url": "",
        "path":     "",
        "model":    "",
    },
}

ANTHROPIC_VERSION = "2023-06-01"

# What a viewport capture can be.  Deliberately short: these are the formats
# jce_scene_kit's `shot` and `frames` produce, and accepting more would mean
# claiming a media type the providers may not take.
IMAGE_TYPES = {".png": "image/png", ".jpg": "image/jpeg",
               ".jpeg": "image/jpeg", ".webp": "image/webp",
               ".gif": "image/gif"}

# Providers refuse a request that is too large with an opaque error, and a
# 4K screenshot is comfortably over it.  Named here so the failure is about
# the picture rather than about the HTTP layer.
MAX_IMAGE_BYTES = 5 * 1024 * 1024
DEFAULT_MAX_TOKENS = 8192


class JceLlmError(RuntimeError):
    pass


class Config:
    """Resolved settings.  Explicit arguments beat the environment, which
    beats the provider's default -- the ordinary precedence, spelled out
    because a tool that silently prefers an env var to a flag is a tool people
    stop trusting."""

    def __init__(self, provider=None, base_url=None, model=None,
                 api_key=None, max_tokens=None, command=None):
        self.provider = (provider or os.environ.get("JCE_LLM_PROVIDER")
                         or "anthropic").strip().lower()
        if self.provider not in PROVIDERS:
            raise JceLlmError(
                "unknown provider %r; this interface speaks %s.  A server that "
                "is OpenAI-compatible (llama.cpp, vLLM, Ollama, LM Studio, "
                "OpenRouter) is provider=openai with --base-url pointed at it."
                % (self.provider, " or ".join(sorted(PROVIDERS))))
        d = PROVIDERS[self.provider]
        self.base_url = (base_url or os.environ.get("JCE_LLM_BASE_URL")
                         or d["base_url"]).rstrip("/")
        self.model = model or os.environ.get("JCE_LLM_MODEL") or d["model"]
        self.api_key = api_key or os.environ.get("JCE_LLM_API_KEY") or ""
        self.max_tokens = int(max_tokens
                              or os.environ.get("JCE_LLM_MAX_TOKENS")
                              or DEFAULT_MAX_TOKENS)
        # provider=command: the argv to run.  shlex, NOT a shell -- the brief
        # is user text that reaches this program, and nothing carrying it may
        # pass through something that would interpret it.
        raw = command or os.environ.get("JCE_LLM_COMMAND") or ""
        self.command = shlex.split(raw) if isinstance(raw, str) else list(raw)
        if self.provider == "command" and not self.command:
            raise JceLlmError(
                "provider=command needs the program to run: set "
                "JCE_LLM_COMMAND or pass --command, e.g. "
                "'ollama run llama3.2' or 'llm -m mistral'.  The prompt goes "
                "to its stdin and the answer is read from its stdout.")

    @property
    def url(self):
        return self.base_url + PROVIDERS[self.provider]["path"]

    def target_description(self):
        """What --send would actually do.  provider=command has no URL, and
        printing "call ." was the first version of this line."""
        if self.provider == "command":
            return "run %s" % " ".join(self.command)
        return "call %s" % self.url


def load_image(path):
    """(media_type, base64 data) for one capture.

    Refuses by extension rather than by sniffing: the point is to hand the
    provider a media type it will accept, and a .bmp renamed to .png would
    pass a sniff and fail at the far end with a message about neither.
    """
    p = Path(path)
    if not p.is_file():
        raise JceLlmError("no such image: %s" % p)
    mt = IMAGE_TYPES.get(p.suffix.lower())
    if not mt:
        raise JceLlmError(
            "%s is not an image this interface sends (%s).  jce_scene_kit's "
            "`shot` and `frames` produce PNG."
            % (p.name, ", ".join(sorted(IMAGE_TYPES))))
    raw = p.read_bytes()
    if len(raw) > MAX_IMAGE_BYTES:
        raise JceLlmError(
            "%s is %.1f MB; the limit here is %d MB.  Providers refuse an "
            "oversized request with an opaque error, so it is refused here "
            "where the message can name the picture."
            % (p.name, len(raw) / 1048576.0, MAX_IMAGE_BYTES // 1048576))
    return mt, base64.b64encode(raw).decode("ascii")


def build_request(cfg, system, user, temperature=0.0, images=None):
    """(url, headers, body) for one single-turn completion.

    Pure: no environment, no clock, no socket.  That is what lets the whole
    request be asserted in a unit test and printed by --dry-run, and it is why
    the two wire shapes are a data difference here rather than two code paths
    that drift.
    """
    images = list(images or [])
    if images and cfg.provider == "command":
        # LOUDLY, not silently.  A program reading a prompt on stdin has
        # nowhere to put a PNG.  Dropping it would leave a model asked "what
        # is wrong with this lighting" answering confidently from the scene
        # text it never saw a picture of -- and that answer is indistinguishable
        # from a real one.
        raise JceLlmError(
            "provider=command cannot carry an image: the prompt reaches the "
            "program on stdin and there is nowhere to put %d attachment(s).  "
            "Use --provider ollama with a vision model (llava, llama3.2-vision), "
            "or openai/anthropic/gemini." % len(images))

    if cfg.provider == "command":
        # No wire shape at all: the "request" is an argv and a stdin payload.
        # Returned in the same (target, headers, body) triple so describe()
        # and every caller stay one code path -- a second shape here is a
        # second thing to keep in sync.
        return (cfg.command, {},
                {"stdin": system + "\n\n" + user, "model": cfg.model})

    if cfg.provider == "ollama":
        # No auth header: it is a local daemon.  `stream: false` matters --
        # the default is a stream of newline-delimited JSON objects, and
        # json.loads on that raises on the second object, which reads as "the
        # server sent garbage" rather than "we forgot to ask for one reply".
        user_msg = {"role": "user", "content": user}
        if images:
            # Ollama takes bare base64 on the message, with no media type --
            # it infers one.  The odd shape out, which is why this is a
            # provider rather than a base_url override.
            user_msg["images"] = [data for _, data in images]
        return cfg.url, {"content-type": "application/json"}, {
            "model":    cfg.model,
            "stream":   False,
            "options":  {"temperature": temperature,
                         "num_predict": cfg.max_tokens},
            "messages": [
                {"role": "system", "content": system},
                user_msg,
            ],
        }

    if cfg.provider == "gemini":
        # Gemini puts the model in the PATH and the key in a header. Use the
        # canonical REST JSON names, including systemInstruction. Differences
        # from both of the others, which is why it is a provider rather than a
        # base_url override of openai.
        url = "%s/models/%s:generateContent" % (cfg.base_url, cfg.model)
        headers = {"content-type": "application/json",
                   "x-goog-api-key": cfg.api_key}
        parts = [{"text": user}]
        for mt, data in images:
            parts.append({"inlineData": {"mimeType": mt, "data": data}})
        body = {
            "systemInstruction": {"parts": [{"text": system}]},
            "contents": [{"role": "user", "parts": parts}],
            "generationConfig": {"temperature": temperature,
                                 "maxOutputTokens": cfg.max_tokens},
        }
        return url, headers, body

    if cfg.provider == "anthropic":
        headers = {
            "content-type":      "application/json",
            "x-api-key":         cfg.api_key,
            "anthropic-version": ANTHROPIC_VERSION,
        }
        body = {
            "model":       cfg.model,
            "max_tokens":  cfg.max_tokens,
            "temperature": temperature,
            # Anthropic carries the system prompt in its own top-level field,
            # not as a message.  Putting it in `messages` is accepted and then
            # weighted differently, which is the kind of "works, subtly worse"
            # that never shows up as an error.
            "system":      system,
            "messages":    [{"role": "user", "content": (
                [{"type": "text", "text": user}] +
                [{"type": "image",
                  "source": {"type": "base64", "media_type": mt,
                             "data": data}} for mt, data in images]
                if images else user)}],
        }
    else:
        headers = {
            "content-type":  "application/json",
            "authorization": "Bearer " + cfg.api_key,
        }
        body = {
            "model":       cfg.model,
            "max_tokens":  cfg.max_tokens,
            "temperature": temperature,
            "messages": [
                {"role": "system", "content": system},
                {"role": "user", "content": (
                    [{"type": "text", "text": user}] +
                    [{"type": "image_url",
                      "image_url": {"url": "data:%s;base64,%s" % (mt, data)}}
                     for mt, data in images]
                    if images else user)},
            ],
        }
    return cfg.url, headers, body


# Anything whose NAME suggests a credential.  A pattern, not a list of the
# headers I happened to remember: the first version named x-api-key and
# authorization, and adding Gemini added `x-goog-api-key`, which is neither --
# so --dry-run printed a live key in clear.  Every new provider is another
# chance to forget one, and that failure is silent and permanent, because the
# output goes into a terminal, a paste, a bug report.  Redacting a header that
# was safe costs nothing; the other direction costs a key.
SECRETISH = re.compile(r"key|auth|token|secret|password|credential", re.I)


def redact_headers(headers):
    """What --dry-run is allowed to print.

    The credential is replaced by its LENGTH and last four characters, not by
    a fixed mask: "set, 51 chars, ends 'mnop'" answers both "did it pick up my
    key" and "did it pick up the RIGHT one" without printing any of it.  A
    fixed mask answers only the first, which is the question people are not
    actually asking when they run a dry run twice.
    """
    out = {}
    for k, v in headers.items():
        if SECRETISH.search(k):
            raw = v[len("Bearer "):] if v.lower().startswith("bearer ") else v
            out[k] = ("<unset>" if not raw
                      else "<set, %d chars, ends %r>" % (len(raw), raw[-4:]))
        else:
            out[k] = v
    return out


def extract_text(cfg, response):
    """The answer, from either wire shape.

    Raises rather than returning "" on a shape it does not recognise: an empty
    string flows downstream and becomes "the model said nothing", which is a
    different and much harder bug than "the response was not what we expected".
    """
    try:
        if cfg.provider == "command":
            return response["text"]
        if cfg.provider == "ollama":
            return response["message"]["content"]
        if cfg.provider == "gemini":
            parts = response["candidates"][0]["content"]["parts"]
            text = "".join(p.get("text", "") for p in parts)
            if not text:
                raise KeyError("candidates[0].content.parts[].text")
            return text
        if cfg.provider == "anthropic":
            parts = [b.get("text", "") for b in response.get("content", [])
                     if b.get("type") == "text"]
            if not parts:
                raise KeyError("content[].text")
            return "".join(parts)
        choices = response["choices"]
        return choices[0]["message"]["content"]
    except (KeyError, IndexError, TypeError, AttributeError) as e:
        raise JceLlmError(
            "unrecognised %s response shape (%s).  Keys: %s"
            % (cfg.provider, e,
               sorted(response.keys()) if isinstance(response, dict)
               else type(response).__name__))


def send(cfg, system, user, temperature=0.0, timeout=120, images=None):
    """Actually call the endpoint.  Only reached behind an explicit --send."""
    if cfg.provider == "command":
        _, _, body = build_request(cfg, system, user, temperature, images)
        try:
            r = subprocess.run(cfg.command, input=body["stdin"],
                               capture_output=True, text=True,
                               encoding="utf-8", timeout=timeout)
        except FileNotFoundError:
            raise JceLlmError("no such program: %s" % cfg.command[0])
        except subprocess.TimeoutExpired:
            raise JceLlmError("%s did not answer within %ds"
                              % (cfg.command[0], timeout))
        if r.returncode != 0:
            raise JceLlmError("%s exited %d: %s"
                              % (cfg.command[0], r.returncode,
                                 (r.stderr or "").strip()[:2000]))
        # Zero exit with empty stdout is the one outcome a caller cannot tell
        # from "the model answered nothing", so it is named rather than
        # returned as an empty string.
        if not (r.stdout or "").strip():
            raise JceLlmError("%s exited 0 and printed nothing on stdout"
                              % cfg.command[0])
        return extract_text(cfg, {"text": r.stdout})

    if not cfg.api_key and cfg.provider != "ollama":
        raise JceLlmError(
            "JCE_LLM_API_KEY is empty.  Set it in the environment; this tool "
            "does not read a key from any file in the repository and does not "
            "write one.  (provider=ollama and provider=command need no key.)")
    url, headers, body = build_request(cfg, system, user, temperature, images)
    req = urllib.request.Request(
        url, data=json.dumps(body).encode("utf-8"),
        headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            payload = json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:2000]
        raise JceLlmError("%s returned HTTP %d: %s"
                          % (cfg.provider, e.code, detail))
    except urllib.error.URLError as e:
        raise JceLlmError("could not reach %s: %s" % (url, e.reason))
    return extract_text(cfg, payload)


def elide_images(body):
    """Base64 is unreadable and megabytes long; a dry run that prints it is a
    dry run nobody reads.  Replaced by its length, so the question a dry run
    IS asked -- did the picture get attached -- still has an answer."""
    # Elided by KEY, not by length.  A length threshold makes the behaviour
    # depend on the picture: a 4K capture is elided and a 1x1 fixture is
    # printed in full, so the test passes on a size nobody ships.  These three
    # keys are base64 wherever they appear, in every provider's shape.
    def walk(o, key=None):
        if isinstance(o, dict):
            return {k: walk(v, k) for k, v in o.items()}
        if isinstance(o, list):
            if key == "images":          # ollama: a bare list of base64
                return ["<base64, %d chars>" % len(x) if isinstance(x, str)
                        else walk(x) for x in o]
            return [walk(x, key) for x in o]
        if isinstance(o, str):
            if key == "data":
                return "<base64, %d chars>" % len(o)
            if key == "url" and o.startswith("data:"):
                return "<data uri, %d chars>" % len(o)
        return o
    return walk(body)


def describe(cfg, system, user, temperature=0.0, images=None):
    """The dry run: exactly what would go on the wire, minus the credential."""
    url, headers, body = build_request(cfg, system, user, temperature, images)
    body = elide_images(body)
    if cfg.provider == "command":
        return "\n".join([
            "RUN %s" % " ".join(url),
            "stdin (%d chars):" % len(body["stdin"]),
            body["stdin"],
        ])
    return "\n".join([
        "POST %s" % url,
        "headers: %s" % json.dumps(redact_headers(headers), indent=2),
        "body: %s" % json.dumps(body, ensure_ascii=False, indent=2),
    ])


def add_cli_args(ap):
    """Shared by every JCE tool that talks to a model, so the flags mean the
    same thing everywhere."""
    ap.add_argument("--provider", choices=sorted(PROVIDERS))
    ap.add_argument("--base-url")
    ap.add_argument("--model")
    ap.add_argument("--max-tokens", type=int)
    ap.add_argument("--command",
                    help="provider=command: the program to run.  The "
                         "prompt goes to its stdin and the answer comes "
                         "from its stdout, e.g. 'ollama run llama3.2' "
                         "or 'llm -m mistral'.")
    ap.add_argument("--temperature", type=float, default=0.0)
    ap.add_argument("--image", action="append", default=[],
                    help="attach an image (repeatable).  provider=command\n                         cannot carry one and says so rather than\n                         dropping it.")
    ap.add_argument("--send", action="store_true",
                    help="actually call the endpoint.  Without it this prints "
                         "the request and exits, which is the default on "
                         "purpose: a brief carries the shape of an unreleased "
                         "game and the default should not ship it anywhere.")


def config_from_args(args):
    return Config(provider=getattr(args, "provider", None),
                  base_url=getattr(args, "base_url", None),
                  model=getattr(args, "model", None),
                  max_tokens=getattr(args, "max_tokens", None),
                  command=getattr(args, "command", None))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    add_cli_args(ap)
    ap.add_argument("--system", default="You are a helpful assistant.")
    ap.add_argument("--user", required=True)
    args = ap.parse_args()

    try:
        cfg = config_from_args(args)
        images = [load_image(p) for p in getattr(args, "image", [])]
        if not args.send:
            print(describe(cfg, args.system, args.user, args.temperature,
                           images))
            print("\n(dry run -- nothing was sent.  Add --send to %s.)"
                  % cfg.target_description(), file=sys.stderr)
            return 0
        print(send(cfg, args.system, args.user, args.temperature,
                   images=images))
    except JceLlmError as e:
        print("jce_llm: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
