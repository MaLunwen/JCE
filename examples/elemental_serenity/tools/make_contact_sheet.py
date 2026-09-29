#!/usr/bin/env python3
"""
make_contact_sheet.py — 8-state ours-vs-reference comparison sheet.

Each cell: reference capture (top) over our shot (bottom), labeled.
Output: es_compare_sheet.png at the repo root (~4200x2400).
"""
import os
from PIL import Image, ImageDraw

HERE  = os.path.dirname(os.path.abspath(__file__))
ROOT  = os.environ.get("ES_REFERENCE_SHOTS", os.path.join(os.path.dirname(HERE), "build", "reference"))
SHOTS = os.path.join(os.path.dirname(HERE), "build", "windows-x86_64-release")

STATES = [
    ("spring_day",   "ref_spring_day_now.png"),
    ("spring_night", "ref8_spring_night.png"),
    ("winter_day",   "ref8_winter_day.png"),
    ("winter_night", "ref8_winter_night.png"),
    ("autumn_day",   "ref8_autumn_day.png"),
    ("autumn_night", "ref8_autumn_night.png"),
    ("rainy_day",    "ref8_rain_day.png"),
    ("rainy_night",  "ref8_rain_night.png"),
]

CW, CH = 1024, 576            # per-image cell (16:9)
PAD, LABEL = 8, 26
COLS = 4

sheet = Image.new("RGB", (COLS * (CW + PAD) + PAD,
                          2 * (2 * CH + LABEL + 3 * PAD)), (18, 18, 22))
draw = ImageDraw.Draw(sheet)

for i, (state, ref_name) in enumerate(STATES):
    col, row = i % COLS, i // COLS
    x = PAD + col * (CW + PAD)
    y = PAD + row * (2 * CH + LABEL + 3 * PAD)
    ref = Image.open(os.path.join(ROOT, ref_name)).convert("RGB").resize((CW, CH))
    our = Image.open(os.path.join(SHOTS, f"shot_{state}.png")).convert("RGB").resize((CW, CH))
    draw.text((x + 4, y + 4), f"{state}   (top: reference / bottom: JCE)",
              fill=(230, 230, 230))
    sheet.paste(ref, (x, y + LABEL))
    sheet.paste(our, (x, y + LABEL + CH + PAD))

out = os.path.join(ROOT, "es_compare_sheet.png")
sheet.save(out)
print(f"wrote {out} ({sheet.size[0]}x{sheet.size[1]})")
