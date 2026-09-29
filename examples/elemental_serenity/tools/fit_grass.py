#!/usr/bin/env python3
"""
fit_grass.py — one measurement pass of the grass render-correction loop.

Compares the grass-crop display-linear percentiles (p85 -> tip, p20 -> root)
of the latest ES_AUTOSHOOT shots against the live-reference captures and
COMPOUNDS the per-state per-channel ratio into tools/grass_corr.json
(consumed by gen_palettes_h.py).  Run:  measure -> regen -> rebuild -> shoot,
repeat until the printed ratios settle near 1.

Usage: python examples/elemental_serenity/tools/fit_grass.py [--damp 0.8]
"""
import json, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get("ES_REFERENCE_SHOTS", os.path.join(os.path.dirname(HERE), "build", "reference"))
SHOTS = os.path.join(os.path.dirname(HERE), "build", "windows-x86_64-release")
CORR_P = os.path.join(HERE, "grass_corr.json")

REFS = {
    ("spring", "day"):   "ref_spring_day_now.png",
    ("winter", "day"):   "ref8_winter_day.png",
    ("autumn", "day"):   "ref8_autumn_day.png",
    ("rainy", "day"):    "ref8_rain_day.png",
    ("spring", "night"): "ref8_spring_night.png",
    ("winter", "night"): "ref8_winter_night.png",
    ("autumn", "night"): "ref8_autumn_night.png",
    ("rainy", "night"):  "ref8_rain_night.png",
}

DAMP = 0.8
if "--damp" in sys.argv:
    DAMP = float(sys.argv[sys.argv.index("--damp") + 1])

def pcts(path):
    im = np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0
    im = im ** 2.2                       # display sRGB -> display linear
    h, w, _ = im.shape
    c = im[int(h*0.62):int(h*0.97), int(w*0.58):int(w*0.93)].reshape(-1, 3)
    return np.percentile(c, 85, axis=0), np.percentile(c, 20, axis=0)

corr = json.load(open(CORR_P, encoding="utf-8"))
worst = 1.0
for (season, tod), rf in REFS.items():
    rt, rr = pcts(os.path.join(ROOT, rf))
    ot, orr = pcts(os.path.join(SHOTS, f"shot_{season}_{tod}.png"))
    k = f"{season}|{tod}"
    for slot, ref_v, our_v in (("tip", rt, ot), ("root", rr, orr)):
        ratio = np.clip(ref_v / np.maximum(our_v, 1e-5), 0.33, 3.0)
        step = ratio ** DAMP             # damped multiplicative step
        cur = np.array(corr[k][slot], dtype=np.float64)
        corr[k][slot] = list(np.clip(cur * step, 0.05, 40.0).round(4))
        worst = max(worst, float(np.max(np.maximum(ratio, 1.0 / ratio))))
    print(f"{k:14} tip_ratio={np.round(np.clip(rt/np.maximum(ot,1e-5),0.33,3.0),2)}")
json.dump(corr, open(CORR_P, "w", encoding="utf-8"), indent=1)
print(f"updated grass_corr.json (worst channel ratio {worst:.2f})")
