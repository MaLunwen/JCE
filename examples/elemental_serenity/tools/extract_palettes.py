#!/usr/bin/env python3
"""
extract_palettes.py — pull every season x time color preset out of the
Elemental-Serenity source (SeasonManager / Skydome / Fog) into one JSON file
(tools/es_palettes.json) that feeds both bake_ground.py and gen_palettes_h.py.

Run:  python examples/elemental_serenity/tools/extract_palettes.py
"""
import argparse, json, os, re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--reference-root", required=True, help="Unmodified upstream source checkout (read only)")
ES = os.path.abspath(parser.parse_args().reference_root)
HERE = os.path.dirname(os.path.abspath(__file__))
OUT  = os.path.join(HERE, "es_palettes.json")

SEASONS = ["spring", "winter", "autumn", "rainy"]
TIMES   = ["day", "night"]

def read(p):
    return open(os.path.join(ES, p), encoding="utf-8").read()

COLOR_RE = re.compile(r"new\s+THREE\.Color\(\s*([^)]*?)\s*\)")
NUM_RE   = re.compile(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?")

def parse_color(arg_str):
    """THREE.Color(r,g,b) floats OR THREE.Color(0xRRGGBB) hex -> [r,g,b] linear."""
    s = arg_str.strip()
    if s.startswith("0x") or s.startswith("0X"):
        v = int(s, 16)
        return [((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0]
    nums = [float(x) for x in NUM_RE.findall(s)]
    if len(nums) == 1:  # single grey float
        return [nums[0]] * 3
    return nums[:3]

def hex_to_rgb(v):
    return [((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0]

def block(src, start_key, end_keys):
    """Source slice from start_key up to the nearest of end_keys (or EOF)."""
    i = src.find(start_key)
    if i < 0: return ""
    ends = [src.find(k, i + len(start_key)) for k in end_keys]
    ends = [e for e in ends if e >= 0]
    return src[i:min(ends)] if ends else src[i:]

def colors_in(seg):
    """Ordered list of (key, [r,g,b]) for `key: new THREE.Color(...)` entries."""
    out = []
    for m in re.finditer(r"(\w+)\s*:\s*new\s+THREE\.Color\(\s*([^)]*?)\s*\)", seg):
        out.append((m.group(1), parse_color(m.group(2))))
    return out

def nums_in(seg):
    """Dict of scalar `key: number` entries (intensity etc.)."""
    d = {}
    for m in re.finditer(r"(\w+)\s*:\s*([-+]?\d*\.?\d+)\s*[,\n]", seg):
        d[m.group(1)] = float(m.group(2))
    return d

# ── SeasonManager: per-season ground/grass/rocks/lighting ───────────────────
sm = read("src/Game/World/Managers/SeasonManager/SeasonManager.class.js")

result = {}
for si, season in enumerate(SEASONS):
    nxt = SEASONS[si + 1] + ":" if si + 1 < len(SEASONS) else "\x00never"
    sblock = block(sm, season + ":", [nxt, "getColorConfig"])
    entry = {}

    for sub in ["ground", "grass", "rocks"]:
        subblk = block(sblock, sub + ":", ["\n        },\n"])
        for tod in TIMES:
            tblk = block(subblk, tod + ":", ["night:" if tod == "day" else "\x00", "},\n\n"])
            cols = dict(colors_in(tblk))
            scal = nums_in(tblk)
            entry.setdefault(sub, {})[tod] = {**{k: v for k, v in cols.items()}, **scal}

    lblock = block(sblock, "lighting:", ["ground:"])
    for tod in TIMES:
        tblk = block(lblock, tod + ":", ["night:" if tod == "day" else "ground:"])
        lights = {}
        for lname in ["key", "fill", "ambient", "rim", "lamp"]:
            lb = block(tblk, lname + ":", ["\n            },"])
            hexm = re.search(r"color\s*:\s*0x([0-9a-fA-F]{6})", lb)
            inten = re.search(r"intensity\s*:\s*([-+]?\d*\.?\d+)", lb)
            posm = re.search(r"position\s*:\s*\[([^\]]*)\]", lb)
            lights[lname] = {
                "color": hex_to_rgb(int(hexm.group(1), 16)) if hexm else [1, 1, 1],
                "intensity": float(inten.group(1)) if inten else 1.0,
            }
            if posm:
                lights[lname]["position"] = [float(x) for x in NUM_RE.findall(posm.group(1))][:3]
        entry.setdefault("lighting", {})[tod] = lights

    # fallingLeaves / windLines / tent accent colors (season-level, no ToD)
    for sub in ["fallingLeaves", "windLines", "tent"]:
        subblk = block(sblock, sub + ":", ["\n        },\n"])
        cols = colors_in(subblk)
        if cols:
            entry[sub] = {cols[0][0]: cols[0][1]}

    result[season] = entry

# ── Skydome presets ──────────────────────────────────────────────────────────
sky = read("src/Game/World/Components/Skydome/Skydome.class.js")
skyblk = block(sky, "createSkyColorPresets", ["updateSkyColors"])
for si, season in enumerate(SEASONS):
    nxt = SEASONS[si + 1] + ":" if si + 1 < len(SEASONS) else "\x00never"
    sblock = block(skyblk, season + ":", [nxt, "};"])
    for tod in TIMES:
        tblk = block(sblock, tod + ":", ["night:" if tod == "day" else "};", "},\n      };"])
        cols = dict(colors_in(tblk))
        result[season].setdefault("sky", {})[tod] = cols

# ── Fog colors ───────────────────────────────────────────────────────────────
fog = read("src/Game/World/Components/Fog/Fog.class.js")
fogblk = block(fog, "createFogColorPresets", ["updateFog"])
if not fogblk:
    fogblk = fog
for si, season in enumerate(SEASONS):
    nxt = SEASONS[si + 1] + ":" if si + 1 < len(SEASONS) else "\x00never"
    sblock = block(fogblk, season + ":", [nxt, "};"])
    for tod in TIMES:
        tblk = block(sblock, tod + ":", ["night:" if tod == "day" else "};", "},"])
        cols = colors_in(tblk)
        nums = re.findall(r"near\s*:\s*([-+]?\d*\.?\d+)|far\s*:\s*([-+]?\d*\.?\d+)", tblk)
        e = {}
        if cols: e["color"] = cols[0][1]
        result[season].setdefault("fog", {})[tod] = e

with open(OUT, "w", encoding="utf-8") as f:
    json.dump(result, f, indent=1)

# summary
for season in SEASONS:
    e = result[season]
    have = [k for k in ["ground", "grass", "rocks", "lighting", "sky", "fog"] if k in e and e[k]]
    print(f"{season}: {have}")
    for k in ["ground", "sky", "fog"]:
        for tod in TIMES:
            v = e.get(k, {}).get(tod, {})
            print(f"  {k}.{tod}: {len(v)} keys" + (f" {list(v)[:4]}" if v else " <-- EMPTY"))
print(f"\nwrote {OUT}")
