#!/usr/bin/env python3
"""
aid_cloud_sim.py -- the ai_dispatch T1 cloud gateway, backed by a REAL LLM.

Implements the ai_dispatch HTTP contract (spec F.2):

    POST {base}/v1/constraint-requests   Authorization: Bearer <token>
    req : { "schema": {name, version, fields:[{name,type,min,max,enum?}...]},
            "context": { <game-defined key/values incl. "prompt"> },
            "budget_ms": <int> }
    rsp : 200 { "fields": { "<name>": <value>, ... }, "model": "<info>" }
    GET  {base}/health -> 200

The engine reaches this over real HTTP, so the whole T1 path (request build,
round-trip, response parse, spec-K sanitising, record stamping) runs for real.

WHAT GENERATES THE SCENE:  a local LLM (Ollama).  The gateway is schema-driven
-- it reads the field list + bounds out of each request and asks the model to
fill EVERY field to match the caller's `prompt` context ("丛林" -> many trees,
"帐篷区" -> many tents).  The gateway writes NO fixed scene: it only describes
what each field means and lets the model decide the numbers.  Every call is a
fresh generation (temperature > 0), so the same prompt yields varied scenes --
the engine freezes each answer into a self-contained record, so replay stays
bit-exact regardless.

If Ollama is unreachable, it falls back to a small themed generator so the demo
never hard-fails (clearly logged as "fallback").

Run:
    python aid_cloud_sim.py --port 8770 --token demo-token --model llama3.1:8b
    python aid_cloud_sim.py --no-ollama          # force the themed fallback
"""
import argparse
import json
import random
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# One local model, one GPU/CPU: serialize generations so concurrent requests
# don't make Ollama 500 (it returns 500 while a model is still loading/busy).
_OLLAMA_LOCK = threading.Lock()

import functools
print = functools.partial(print, file=sys.stderr, flush=True)  # unbuffered diagnostics

# ---- per-field semantic hints (what each field DOES, not a fixed value) -----
# These let the LLM map a scene prompt onto the schema.  Describing a field's
# meaning is not "generating a fixed scene" -- the model still chooses every
# number.  Unknown fields simply get a generic in-bounds instruction.
FIELD_HINTS = {
    "season":        "overall season/mood",
    "key_light":     "sun/key light colour",
    "key_intensity": "sun brightness (low=dim/overcast, high=blazing)",
    "fog_density":   "atmospheric haze (high=misty/mysterious)",
    "fog_color":     "fog tint",
    "water_shallow": "shallow water colour",
    "water_deep":    "deep water colour",
    "water_ice":     "0=liquid ... 1=fully frozen (winter/ice)",
    "veg_shadow":    "foliage darkest tone",
    "veg_mid":       "foliage mid tone",
    "veg_high":      "foliage highlight tone",
    "veg_mult":      "foliage overall tint multiplier",
    "veg_scale":     "bush size (<0.6 sparse/open ... >1.4 huge/enclosed jungle)",
    "grass_density": "grass blades per patch",
    "grass_hue":     "grass hue shift",
    "prop_scale":    "rock/tree size multiplier",
    "prop_spread":   "how far scattered props spread from the pond centre",
    "fireflies":     "glowing firefly emitters (HIGH for night/magical/jungle)",
    "campfire":      "1=lit campfire + warm lights (camp/night), else 0",
    "show_bridge":   "1=wooden bridge present, 0=wild untouched water",
    "show_tent":     "1=tent + camp present, 0=open wilderness",
    "show_flowers":  "1=wildflowers scattered (meadow/spring), else 0",
    "layout_jitter": "how much the vegetation is rearranged from default (0..1)",
    "extra_clusters":"extra bush patches (HIGH=overgrown jungle)",
    "spawn_trees":   "extra TREE canopies (HIGH for forest/jungle prompts)",
    "spawn_tents":   "extra TENT instances (HIGH for camp/tent-area prompts)",
    "spawn_rocks":   "extra rock groups (higher for rocky/lake shores)",
}

# ---- themed fallback palettes (only used when Ollama is unreachable) --------
THEMES = {
    "verdant_dawn": dict(density=0.55, scale=(0.45, 0.55), blade_h=0.62, hue=0.42,
        root="#06210A", tip="#7FD08C", wind="NE", note="misty verdant dawn"),
    "lush_noon": dict(density=0.90, scale=(0.62, 0.70), blade_h=0.72, hue=0.20,
        root="#0A2E06", tip="#57B33C", wind="E", note="deep lush noon"),
    "golden_dusk": dict(density=0.62, scale=(0.35, 0.48), blade_h=0.50, hue=0.62,
        root="#241608", tip="#D8A93F", wind="SW", note="gold-washed dusk"),
    "moonlit_hush": dict(density=0.32, scale=(0.20, 0.35), blade_h=0.40, hue=0.78,
        root="#0A1420", tip="#6C86A8", wind="N", note="blue-grey moonlit hush"),
    "wild_bloom": dict(density=0.75, scale=(0.30, 0.85), blade_h=0.66, hue=0.30,
        root="#12300A", tip="#B7E24A", wind="W", note="a wilder, high-contrast bloom"),
}
THEME_ORDER = list(THEMES.keys())

# ---- HYBRID generation: the LLM decides the CREATIVE INTENT (cheap tokens);
# the gateway DERIVES the coherent colour palette from season + key light.
# This roughly halves the model's output (7 hex colours were the costly part)
# while the AI still chooses the place-type, season, counts and light. ----------
LLM_INTENT_FIELDS = {
    "season", "key_light", "key_intensity", "fog_density", "veg_scale",
    "fireflies", "campfire", "show_bridge", "show_tent", "show_flowers",
    "extra_clusters", "spawn_trees", "spawn_tents", "spawn_rocks",
}

# ---- v3 BOUNDED OP-LIST schema (op_00..op_NN slots) -------------------------
# The AI composes a free list of {target, action, value} ops; the gateway packs
# each into one I32 slot: (target<<24)|(action<<16)|(value&0xFFFF).  Unknown
# targets/actions are DROPPED (bounded-by-construction); values are clamped.
OP_TARGETS = {
    "foliage": 1, "bush": 3, "tree": 2, "tent": 4, "bridge": 5, "rocks": 6,
    "rock": 6, "flowers": 7, "flower": 7, "water": 8, "dirlight": 9,
    "light": 9, "sun": 9, "campfire": 10, "fire": 10, "firefly": 11,
    "fireflies": 11, "grass": 12, "fog": 13, "sky": 14, "layout": 15,
}
# layout archetype values (target=layout, action=set): the AI picks the macro
# spatial arrangement of ALL vegetation.
OP_LAYOUTS = {"scatter": 0, "dense": 1, "sparse": 2, "cluster": 3,
              "clustered": 3, "grove": 3, "groves": 3, "clearing": 4,
              "open": 2, "ring": 5}
OP_ACTIONS = {
    "spawn": 1, "hide": 2, "show": 3, "scale": 4, "spread": 5,
    "tint": 6, "rearrange": 7, "set": 8,
}
OP_FIXED_ACTIONS = {4, 5, 7}      # scale/spread/rearrange: value is 8.8 fixed
OP_COUNT_MAX = {1: 40}            # spawn counts clamp
OP_SLOT_PREFIX = "op_"


def is_oplist_schema(fields):
    return any((f.get("name") or "").startswith(OP_SLOT_PREFIX) for f in fields)


def encode_ops(fields, raw_ops):
    """LLM's natural ops -> packed I32 slot values {op_00: int, ...}."""
    slots = sorted(f["name"] for f in fields
                   if (f.get("name") or "").startswith(OP_SLOT_PREFIX))
    out = {s: 0 for s in slots}
    idx = 0
    if not isinstance(raw_ops, list):
        return out, 0
    for op in raw_ops:
        if idx >= len(slots) or not isinstance(op, dict):
            break
        t = OP_TARGETS.get(str(op.get("target", "")).strip().lower())
        a = OP_ACTIONS.get(str(op.get("action", "")).strip().lower())
        if not t or not a:
            continue                      # bounded: unknown -> dropped
        raw_v = op.get("value", 0)
        if t == 15:                        # layout: accept a name or an index
            if isinstance(raw_v, str):
                v = float(OP_LAYOUTS.get(raw_v.strip().lower(), 0))
            else:
                try: v = float(raw_v or 0)
                except (TypeError, ValueError): v = 0.0
            a = 8                          # force SET
        else:
            try:
                v = float(raw_v or 0)
            except (TypeError, ValueError):
                v = 0.0
        if a in OP_FIXED_ACTIONS:         # 8.8 fixed (0..255.996)
            iv = int(round(max(0.0, min(255.0, v)) * 256.0))
        elif a == 8 and v <= 4.0:         # SET with small float -> 8.8 too
            iv = int(round(max(0.0, min(255.0, v)) * 256.0))
        else:                             # counts / palette idx: plain int
            iv = int(round(max(0.0, min(65535.0, v))))
            if a in OP_COUNT_MAX:
                iv = min(iv, OP_COUNT_MAX[a])
        out[slots[idx]] = (t << 24) | (a << 16) | (iv & 0xFFFF)
        idx += 1
    return out, idx

# base palettes per season: veg (shadow/mid/high/mult), water (shallow/deep),
# fog -- each an (r,g,b).  The gateway tints these toward the LLM's key light so
# the AI's colour choice still colours the whole scene.
SEASON_PALETTE = {
    "spring": dict(vs=(8, 48, 15), vm=(46, 122, 42), vh=(143, 216, 110),
                   vx=(200, 255, 180), ws=(143, 208, 200), wd=(46, 110, 116),
                   fog=(232, 240, 224), gh=0.10, ice=0.0),
    "summer": dict(vs=(4, 22, 10), vm=(19, 78, 16), vh=(58, 128, 32),
                   vx=(88, 168, 56), ws=(58, 122, 94), wd=(14, 48, 32),
                   fog=(210, 228, 210), gh=0.08, ice=0.0),
    "autumn": dict(vs=(58, 30, 6), vm=(166, 100, 26), vh=(232, 178, 74),
                   vx=(255, 217, 138), ws=(159, 168, 106), wd=(74, 82, 48),
                   fog=(232, 200, 154), gh=0.24, ice=0.0),
    "winter": dict(vs=(42, 48, 56), vm=(110, 122, 120), vh=(205, 224, 218),
                   vx=(232, 242, 240), ws=(200, 220, 232), wd=(106, 132, 148),
                   fog=(228, 236, 244), gh=0.30, ice=0.85),
    "mystic": dict(vs=(4, 18, 30), vm=(22, 106, 122), vh=(79, 224, 192),
                   vx=(156, 255, 234), ws=(47, 232, 208), wd=(10, 58, 106),
                   fog=(10, 32, 56), gh=0.33, ice=0.0),
}


def clampf(v, lo, hi):
    return max(lo, min(hi, v))


def _hex_of(rgb):
    r, g, b = (int(clampf(round(c), 0, 255)) for c in rgb)
    return "#%02X%02X%02XFF" % (r, g, b)


def _blend(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def _parse_hex(s, default=(200, 200, 200)):
    if not isinstance(s, str):
        return default
    s = s.strip().lstrip("#")
    if len(s) >= 6:
        try:
            return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))
        except ValueError:
            pass
    return default


def q16_to_float(q):
    """Schema bounds arrive as Q16.16 fixed-point for F32Q fields."""
    return q / 65536.0


# ---- LLM (Ollama) generation ------------------------------------------------

def field_spec_line(f):
    """One human/LLM-readable spec line for a schema field."""
    name = f.get("name", "?")
    ftype = (f.get("type") or "").lower()
    hint = FIELD_HINTS.get(name, "in-bounds value")
    if ftype in ("enum", "tagset"):
        opts = "/".join(f.get("enum") or [])
        return f'- "{name}" (enum, one of: {opts}) -- {hint}'
    if ftype == "color8":
        return f'- "{name}" (color "#RRGGBBAA") -- {hint}'
    if ftype == "i32":
        return f'- "{name}" (integer {int(f.get("min",0))}..{int(f.get("max",0))}) -- {hint}'
    if ftype == "f32q":
        lo, hi = q16_to_float(f.get("min", 0)), q16_to_float(f.get("max", 65536))
        return f'- "{name}" (number {lo:.3g}..{hi:.3g}) -- {hint}'
    return f'- "{name}" (number) -- {hint}'


def build_messages_oplist(schema, ctx):
    """v3: the model writes a free OP LIST — its own bounded 'scene program'."""
    prompt = (ctx.get("prompt") or ctx.get("intent")
              or "a serene stylized pond at rest")
    system = (
        "You are the generation model behind a game engine's scene-recipe "
        "service.  The engine hosts a stylized 3D pond diorama built from a "
        "FIXED asset set (pond, bridge, tent, rocks, trees, bushes, grass, "
        "flowers, campfire, fireflies).  From ONE text prompt you write a "
        "small PROGRAM of bounded operations that recomposes those assets "
        "into a new place.\n\n"
        "Return ONLY a JSON object -- no prose, no explanation, no markdown "
        "code fences, nothing before the '{' or after the '}':\n"
        '{ "season": one of spring/summer/autumn/winter/mystic,\n'
        '  "key_light": "#RRGGBBAA" sun colour,\n'
        '  "key_intensity": 0.3-2.2,\n'
        '  "fog_density": 0-0.1,\n'
        '  "grass_density": 30-180,\n'
        '  "ops": [ {"target": T, "action": A, "value": V}, ... 4 to 8 ] }\n\n'
        "Targets: foliage(all bushes), tree, tent, bridge, rocks, flowers, "
        "water, dirlight, campfire, firefly, grass, fog.\n"
        "Actions and their value:\n"
        "- spawn: create N clones (tree/tent/rocks/foliage/firefly; N 0-40, "
        "tents max 24)\n"
        "- hide / show: value ignored (tent/bridge/flowers/campfire)\n"
        "- scale: size multiplier 0.4-2.0 (foliage/rocks/tree; OR target=water "
        "to resize the POND itself: 0.5 a small pool leaving lots of land for a "
        "forest/village, 1.8 a big lake that dominates)\n"
        "- spread: radial spread 0.8-1.4 (foliage/flowers)\n"
        "- rearrange: layout shuffle amount 0-1 (foliage)\n"
        "- tint: palette index 0-15 (water/dirlight/fog/foliage; 0 green 1 "
        "lime 2 gold 3 rust 4 blue 5 teal 6 purple 7 white ...)\n"
        "- set: scalar (water=ice 0-1)\n"
        "- layout: choose the WHOLE-scene spatial arrangement of vegetation. "
        "target=\"layout\", action=\"set\", value one of scatter/dense/sparse/"
        "cluster/clearing/ring.  Pick to match the place: dense forest -> "
        "\"dense\"; open meadow -> \"sparse\"; a camp or tribe -> \"cluster\"; "
        "an arena/amphitheatre -> \"clearing\".\n\n"
        "Compose BOLDLY to match the prompt: a jungle spawns MANY trees and "
        "scales foliage up; a tent camp spawns MANY tents and shows the "
        "campfire; a lake hides tent+bridge and spawns rocks; winter sets "
        "water ice.  Use 4 to 8 high-impact ops -- pick the ones that most "
        "define the place; do NOT pad the list.")
    user = f'Scene prompt: "{prompt}"\nWrite the JSON recipe program now.'
    return system, user, prompt


def build_messages(schema, ctx):
    fields = schema.get("fields") or []
    if is_oplist_schema(fields):
        return build_messages_oplist(schema, ctx)
    # HYBRID: ask the model only for the creative-intent fields (fast); the
    # gateway derives the rest.  Fall back to "all fields" for schemas whose
    # names we don't recognise.
    intent = [f for f in fields if f.get("name") in LLM_INTENT_FIELDS]
    if not intent:
        intent = fields
    spec = "\n".join(field_spec_line(f) for f in intent)
    prompt = (ctx.get("prompt") or ctx.get("intent")
              or "a serene stylized pond at rest")
    system = (
        "You are the generation model behind a game engine's scene-recipe "
        "service.  The engine hosts a stylized 3D pond diorama built from a "
        "FIXED asset set (a pond, a bridge, a tent, rocks, trees, bushes, "
        "grass, flowers, campfire and firefly emitters).  From ONE text prompt "
        "you output numbers that recompose those assets into a new place: "
        "colours, fog, water, vegetation size, and how many extra trees / "
        "tents / rocks / bush patches to spawn, plus which props are shown.\n\n"
        "Return ONLY a single JSON object with EXACTLY these keys and no others. "
        "Respect every range. Integers must be whole numbers within range. "
        "Colours are \"#RRGGBBAA\" hex strings. Pick values that VIVIDLY and "
        "boldly match the prompt -- exaggerate for a strong sense of place "
        "(a jungle is dense with many trees; a tent camp has many tents and a "
        "campfire; a lake is open water with few props).\n\n"
        "Fields:\n" + spec)
    user = f'Scene prompt: "{prompt}"\nGenerate the JSON recipe now.'
    return system, user, prompt


def preload_ollama(url, model, timeout, num_ctx=2048, keep_alive="30m"):
    """Load the model into VRAM at the given context size without generating.
    Ollama treats a prompt-less /api/generate as a pure load, so the first
    real request is already warm (~6s) instead of paying the ~12s cold load."""
    body = json.dumps({"model": model, "keep_alive": keep_alive,
                       "options": {"num_ctx": num_ctx}}).encode("utf-8")
    req = urllib.request.Request(url.rstrip("/") + "/api/generate", data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        resp.read()


def _extract_json(text):
    """Pull the JSON object out of a model reply.  We do NOT use Ollama's
    format="json" (see below), so the reply is usually clean JSON but may be
    wrapped in ```json fences or a sentence -- take the outermost {...}."""
    text = (text or "").strip()
    try:
        return json.loads(text)
    except Exception:
        pass
    a = text.find("{")
    b = text.rfind("}")
    if a >= 0 and b > a:
        return json.loads(text[a:b + 1])
    raise ValueError("no JSON object in model reply: %r" % text[:120])


def call_ollama(url, model, system, user, temperature, timeout,
                num_ctx=2048, num_predict=384, keep_alive="30m"):
    # Two latency levers, both large on an 8 GB laptop GPU:
    #  1. num_ctx: llama3.1's default context is 131072 tokens, whose KV cache
    #     (~13 GB) does not fit in 8 GB VRAM, so Ollama offloads ~60% of the
    #     model to CPU and generation crawls at ~8 tok/s. A recipe needs well
    #     under 2 K tokens; num_ctx=2048 keeps the whole model resident (full
    #     GPU) at ~50 tok/s. keep_alive holds it loaded past the ~12 s reload.
    #  2. NO format="json": Ollama's grammar-constrained JSON decoding adds
    #     ~6 s of fixed overhead per request on this build (measured: 10.5 s
    #     with vs 4.3 s without, identical output). We instruct JSON in the
    #     prompt and extract it ourselves (_extract_json) instead -- same
    #     result, ~half the latency.
    body = json.dumps({
        "model": model,
        "stream": False,
        "keep_alive": keep_alive,
        "options": {
            "temperature": temperature,
            "num_ctx": num_ctx,
            "num_predict": num_predict,
        },
        "messages": [
            {"role": "system", "content": system},
            {"role": "user", "content": user},
        ],
    }).encode("utf-8")
    req = urllib.request.Request(url.rstrip("/") + "/api/chat", data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as he:
        detail = ""
        try:
            detail = he.read().decode("utf-8", "replace")[:300]
        except Exception:
            pass
        raise RuntimeError("HTTP %s: %s" % (he.code, detail))
    # Expose generation throughput so the operator can see whether the GPU is
    # free (~50 tok/s) or being contended by a co-running renderer (~15 tok/s).
    ec = payload.get("eval_count") or 0
    ed = (payload.get("eval_duration") or 0) / 1e9
    call_ollama.last_rate = (ec / ed) if ed > 0 else 0.0
    content = (payload.get("message") or {}).get("content", "")
    return _extract_json(content)


call_ollama.last_rate = 0.0


# ---- sanitise the model's raw JSON to the schema (spec K happens engine-side
#      too, but we return clean, in-bounds, correctly-typed values) -----------

def coerce_field(f, raw, rng):
    name = f.get("name")
    ftype = (f.get("type") or "").lower()
    val = raw.get(name)

    if ftype in ("enum", "tagset"):
        names = f.get("enum") or []
        if isinstance(val, str) and val in names:
            return val
        if isinstance(val, str):
            low = val.strip().lower()
            for n in names:
                if n.lower() == low:
                    return n
        return names[0] if names else 0

    if ftype == "color8":
        if isinstance(val, str):
            s = val.strip()
            if s.startswith("#"):
                s = s[1:]
            if len(s) == 6:
                s += "FF"
            if len(s) == 8:
                try:
                    int(s, 16)
                    return "#" + s.upper()
                except ValueError:
                    pass
        return "#%02X%02X%02X FF".replace(" ", "") % (
            rng.randint(40, 220), rng.randint(60, 220), rng.randint(40, 200))

    lo = f.get("min", 0)
    hi = f.get("max", 0)
    if ftype == "i32":
        try:
            iv = int(round(float(val)))
        except (TypeError, ValueError):
            iv = int(round((lo + hi) / 2.0))
        return int(clampf(iv, lo, hi))
    if ftype == "f32q":
        lo, hi = q16_to_float(lo), q16_to_float(hi)
        try:
            fv = float(val)
        except (TypeError, ValueError):
            fv = (lo + hi) / 2.0
        return round(clampf(fv, lo, hi), 4)
    return val if isinstance(val, (int, float)) else 0


def expand_recipe(fields, llm_raw, rng):
    """Merge the LLM's intent fields with a derived, coherent colour palette to
    fill EVERY schema field.  The AI still decides season/light/counts; the
    gateway only expands the supporting palette + placement scalars."""
    by_name = {f.get("name"): f for f in fields if f.get("name")}

    # v3 op-list schema: 5 coerced globals + the model's ops packed into slots.
    if is_oplist_schema(fields):
        out = {}
        for name, f in by_name.items():
            if not name.startswith(OP_SLOT_PREFIX):
                out[name] = coerce_field(f, llm_raw, rng)
        slots, n_ops = encode_ops(fields, llm_raw.get("ops"))
        out.update(slots)
        out["_n_ops"] = n_ops          # diagnostic only; stripped before send
        return out
    out = {}
    # 1. take the intent fields straight from the model (coerced to bounds).
    for name, f in by_name.items():
        if name in LLM_INTENT_FIELDS:
            out[name] = coerce_field(f, llm_raw, rng)

    season = out.get("season")
    if season not in SEASON_PALETTE:
        season = "summer"
    pal = SEASON_PALETTE[season]
    key = _parse_hex(out.get("key_light"), (255, 244, 214))
    if "key_light" in by_name and "key_light" not in out:
        out["key_light"] = _hex_of(key)

    def jit(rgb, amt=10):
        return tuple(c + rng.uniform(-amt, amt) for c in rgb)

    # 2. derive the remaining colours from the season palette, tinted toward the
    #    AI's key light so its choice colours the whole scene.
    derived = {
        "fog_color":     _hex_of(jit(_blend(pal["fog"], key, 0.30))),
        "water_shallow": _hex_of(jit(_blend(pal["ws"], key, 0.15))),
        "water_deep":    _hex_of(jit(_blend(pal["wd"], key, 0.12))),
        "veg_shadow":    _hex_of(jit(_blend(pal["vs"], key, 0.10), 6)),
        "veg_mid":       _hex_of(jit(_blend(pal["vm"], key, 0.16))),
        "veg_high":      _hex_of(jit(_blend(pal["vh"], key, 0.20))),
        "veg_mult":      _hex_of(jit(_blend(pal["vx"], key, 0.22))),
    }
    # 3. derive the supporting scalars.
    vscale = out.get("veg_scale", 1.0)
    derived_num = {
        "water_ice":     pal["ice"],
        "grass_hue":     round(clampf(pal["gh"] + rng.uniform(-0.02, 0.02), 0.02, 0.35), 4),
        "grass_density": round(clampf(40 + vscale * 70 + rng.uniform(-15, 15), 30, 180), 1),
        "prop_scale":    round(clampf(0.9 + rng.uniform(-0.1, 0.4), 0.7, 1.45), 3),
        "prop_spread":   round(clampf(1.0 + rng.uniform(-0.15, 0.3), 0.8, 1.35), 3),
        "layout_jitter": round(clampf(0.35 + rng.uniform(-0.2, 0.35), 0.0, 1.0), 3),
    }
    for name, val in list(derived.items()) + list(derived_num.items()):
        if name in by_name and name not in out:
            out[name] = coerce_field(by_name[name], {name: val}, rng)
    # 4. anything still missing (unknown schema) -> generic in-bounds.
    for name, f in by_name.items():
        if name not in out:
            out[name] = coerce_field(f, llm_raw, rng)
    return out


def themed_fallback(fields, rng):
    """Only reached when Ollama is unreachable -- keep the demo alive."""
    theme = THEMES[rng.choice(THEME_ORDER)]
    out = {}
    for f in fields:
        name = (f.get("name") or "").lower()
        ftype = (f.get("type") or "").lower()
        if ftype in ("enum", "tagset"):
            names = f.get("enum") or []
            out[f["name"]] = rng.choice(names) if names else 0
        elif ftype == "color8":
            base = theme["tip"] if "high" in name or "tip" in name else theme["root"]
            out[f["name"]] = base
        elif ftype == "i32":
            if name.startswith(OP_SLOT_PREFIX):
                out[f["name"]] = 0     # op slots: empty (clean seasonal scene)
            else:
                lo, hi = int(f.get("min", 0)), int(f.get("max", 0))
                out[f["name"]] = rng.randint(lo, max(lo, hi))
        elif ftype == "f32q":
            lo, hi = q16_to_float(f.get("min", 0)), q16_to_float(f.get("max", 65536))
            out[f["name"]] = round(lo + rng.random() * (hi - lo), 4)
    return out, "themed=%s" % theme["note"]


def make_handler(args):
    counter = {"n": 0}

    class Handler(BaseHTTPRequestHandler):
        server_version = "aid-cloud-sim/2.0-ollama"

        def _send(self, code, obj):
            body = json.dumps(obj).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, fmt, *a):
            pass

        def do_GET(self):
            if self.path.rstrip("/") == "/health":
                self._send(200, {"status": "ok", "model": self.server_version})
            else:
                self._send(404, {"error": "not found"})

        def do_POST(self):
            if not self.path.startswith("/v1/constraint-requests"):
                self._send(404, {"error": "not found"})
                return
            if args.token:
                if self.headers.get("Authorization", "") != "Bearer " + args.token:
                    print("  \033[31m401 bad/missing bearer token\033[0m")
                    self._send(401, {"error": "unauthorized"})
                    return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                req = json.loads(self.rfile.read(length) or b"{}")
            except Exception as e:
                self._send(400, {"error": "bad json: %s" % e})
                return

            schema = req.get("schema") or {}
            fields = schema.get("fields") or []
            ctx = req.get("context") or {}
            counter["n"] += 1
            n = counter["n"]
            rng = random.Random()
            prompt = ctx.get("prompt") or ctx.get("intent") or "(none)"

            out = None
            model_tag = None
            if not args.no_ollama:
                system, user, pr = build_messages(schema, ctx)
                # serialize + retry: Ollama 500s transiently while loading.
                with _OLLAMA_LOCK:
                    for attempt in range(3):
                        try:
                            t0 = time.time()
                            raw = call_ollama(args.ollama_url, args.model, system,
                                              user, args.temperature,
                                              args.ollama_timeout,
                                              num_ctx=args.num_ctx,
                                              num_predict=args.num_predict,
                                              keep_alive=args.keep_alive)
                            out = expand_recipe(fields, raw, rng)
                            n_ops = out.pop("_n_ops", None)
                            model_tag = "ollama/%s (%.1fs, %.0f tok/s, %s)" % (
                                args.model, time.time() - t0,
                                call_ollama.last_rate,
                                ("oplist n=%s" % n_ops) if n_ops is not None
                                else "hybrid")
                            print("  \033[32m200\033[0m #%d  \033[35mLLM\033[0m %s  "
                                  "prompt=\033[33m%s\033[0m  season=%s"
                                  % (n, model_tag, pr, out.get("season")))
                            if n_ops is not None:
                                print("    ops: %s" % json.dumps(
                                    raw.get("ops"), ensure_ascii=False)[:400])
                            break
                        except Exception as e:
                            print("  \033[33mOllama attempt %d failed (%s)\033[0m"
                                  % (attempt + 1, e))
                            time.sleep(2.0)
                    if out is None:
                        print("  \033[33m-> themed fallback after retries\033[0m")

            if out is None:
                out, note = themed_fallback(fields, rng)
                model_tag = "aid-cloud-sim/2.0 fallback (%s)" % note
                print("  \033[32m200\033[0m #%d  \033[33mFALLBACK\033[0m %s  "
                      "prompt=\033[33m%s\033[0m" % (n, note, prompt))

            self._send(200, {"fields": out,
                             "model": "aid-cloud-sim/2.0 %s" % model_tag})

    return Handler


def main():
    ap = argparse.ArgumentParser(description="ai_dispatch T1 gateway backed by Ollama")
    ap.add_argument("--port", type=int, default=8770)
    ap.add_argument("--token", default="demo-token",
                    help="required bearer token ('' to disable auth)")
    ap.add_argument("--ollama-url", default="http://127.0.0.1:11434")
    ap.add_argument("--model", default="llama3.1:8b",
                    help="Ollama chat model that fills the recipe")
    ap.add_argument("--temperature", type=float, default=0.9,
                    help="model temperature (variety); >0 => same prompt varies")
    ap.add_argument("--ollama-timeout", type=float, default=60.0)
    ap.add_argument("--num-ctx", type=int, default=2048,
                    help="context window (small => model fits VRAM => full "
                         "GPU => ~6x faster; a recipe needs <2K tokens)")
    ap.add_argument("--num-predict", type=int, default=384,
                    help="max output tokens (a recipe is ~120)")
    ap.add_argument("--keep-alive", default="30m",
                    help="how long Ollama keeps the model resident between "
                         "requests (avoids the ~12s cold reload)")
    ap.add_argument("--no-ollama", action="store_true",
                    help="skip the LLM; always use the themed fallback")
    args = ap.parse_args()

    handler = make_handler(args)
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    print("\033[1maid_cloud_sim 2.0\033[0m -- ai_dispatch T1 gateway (Ollama-backed)")
    print("  listening   http://127.0.0.1:%d" % args.port)
    print("  endpoint    POST /v1/constraint-requests")
    print("  generation  %s" % ("THEMED FALLBACK (--no-ollama)" if args.no_ollama
                                 else "Ollama %s @ %s (temp %.2f)"
                                      % (args.model, args.ollama_url, args.temperature)))
    print("  auth        %s" % ("Bearer " + args.token if args.token else "(disabled)"))
    if not args.no_ollama:
        # Preload the model at the right num_ctx so the FIRST real prompt is
        # already warm (~6s) instead of paying the ~12s cold load.
        print("  warming     %s (num_ctx=%d) ..." % (args.model, args.num_ctx),
              end="", flush=True)
        try:
            t0 = time.time()
            preload_ollama(args.ollama_url, args.model, args.ollama_timeout,
                           num_ctx=args.num_ctx, keep_alive=args.keep_alive)
            print(" ready (%.1fs)" % (time.time() - t0))
        except Exception as e:
            print(" skipped (%s)" % e)
    print("  (Ctrl-C to stop)\n")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n  stopped.")
        srv.shutdown()


if __name__ == "__main__":
    sys.exit(main())
