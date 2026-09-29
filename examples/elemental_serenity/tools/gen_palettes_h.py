#!/usr/bin/env python3
"""
gen_palettes_h.py — emit src/es_palettes.h (C preset table) from
tools/es_palettes.json (extract_palettes.py output).

One EsPreset per (season x time): dome colors, fog, ambient, key/fill light,
tent-lamp intensity, water shallow/deep + ripple/ice ratios, grass root/tip,
rocks tint, baked ground texture path.

Run:  python examples/elemental_serenity/tools/gen_palettes_h.py
"""
import json, os

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
SRC  = os.path.join(HERE, "es_palettes.json")
OUT  = os.path.join(PROJ, "src", "es_palettes.h")

SEASONS = ["spring", "winter", "autumn", "rainy"]
TIMES   = ["day", "night"]

P = json.load(open(SRC, encoding="utf-8"))

# Leaf-card tint per (season, tod). Approximations of the reference's bush
# palettes (collage panels); refine by extending extract_palettes.py to pull
# the SeasonManager bush block if closer matching is needed.
LEAF_TINTS = {
    ("spring", "day"):   [0.10, 0.34, 0.08],
    ("spring", "night"): [0.05, 0.16, 0.05],
    ("winter", "day"):   [0.86, 0.90, 0.92],
    ("winter", "night"): [0.55, 0.62, 0.72],
    ("autumn", "day"):   [0.95, 0.42, 0.05],
    ("autumn", "night"): [0.45, 0.20, 0.03],
    ("rainy",  "day"):   [0.05, 0.20, 0.06],
    ("rainy",  "night"): [0.03, 0.10, 0.04],
}

def f3(v):
    return "{ %.6ff, %.6ff, %.6ff }" % (v[0], v[1], v[2])

def mix(a, b, t):
    return [a[i] + (b[i] - a[i]) * t for i in range(3)]

def scale(v, k):
    return [v[0] * k, v[1] * k, v[2] * k]

# ---- day self-lit color compensation ---------------------------------------
# The reference Skydome/fog colors are RAW DISPLAY values: its skydome is a
# custom ShaderMaterial that three.js does NOT tone-map, so uHorizonColor
# (0.46,0.74,0.93) lands on screen as bytes (117,189,237).  Our dome runs
# through the scene postfx (ACES, exposure 0.92, gamma 2.2), so feeding the
# raw values washes them out.  Invert the exact fs_tonemap.sc ACES chain so
# the dome DISPLAYS the reference bytes:  c = aces_inv(v^2.2) / exposure.
ES_EXPOSURE = 0.92   # must match gen_scene.py postfx "exposure"

def aces_inv_lin(y, exposure=ES_EXPOSURE, cap=1.3):
    """Post-tonemap LINEAR value -> pre-tonemap linear for JCE's ACES chain."""
    y = min(max(y, 0.0), 0.995)             # ACES asymptote guard
    a = 2.51 - 2.43 * y
    b = 0.03 - 0.59 * y
    c0 = -0.14 * y
    x = (-b + (b * b - 4.0 * a * c0) ** 0.5) / (2.0 * a)
    return min(x / exposure, cap)

def aces_inv_disp(v):
    """Display sRGB fraction (0..1) -> pre-tonemap linear."""
    return aces_inv_lin(max(v, 0.0) ** 2.2)

def aces_inv3(v):
    return [aces_inv_disp(c) for c in v]

def day_fog3(v):
    """Day fog: unlike the skydome (raw ShaderMaterial bytes), fog is mixed
    IN-SCENE in the reference and passes their day tonemap (Linear x1.75),
    so the displayed band is min(raw*1.75, 1) — pale cyan-white, not the raw
    #3282CD blue.  Invert our ACES chain against that displayed value."""
    return [aces_inv_lin(min(c * 1.75, 1.0)) for c in v]

# Our grass shader PAINTS its colors (no lighting), while the reference's
# grass is a lit MeshStandardMaterial under Linear tonemapping x1.75 whose
# response varies per season key light.  Instead of modelling that, target
# the OBSERVED displayed grass directly: per-season sRGB percentiles sampled
# from live reference captures over a grass-dominated crop (p85 -> tip,
# p20 -> root; our shader derives the dark 3rd tone as root*0.3), then
# ACES-invert so the painted blades display the same bytes.
# Display-LINEAR targets (post-tonemap linear), fitted per state from the
# live reference captures over a grass-dominated crop (p85 -> tip, p20 ->
# root) and one-step corrected against our own rendered carpet (target^2 /
# rendered): the dense 103/m^2 carpet shows mostly-tip pixels, so raw
# percentile targets rendered ~1.2x bright on sunny states and dark on rainy.
try:
    GRASS_CORR = json.load(open(os.path.join(HERE, "grass_corr.json"), encoding="utf-8"))
except FileNotFoundError:
    GRASS_CORR = {}

GRASS_DISPLAY = {
    ("spring", "day"):   {"tip": (0.2682, 0.4918, 0.0103), "root": (0.0880, 0.4011, 0.0089)},
    ("winter", "day"):   {"tip": (0.2933, 0.4769, 0.4471), "root": (0.1479, 0.2946, 0.3062)},
    ("autumn", "day"):   {"tip": (0.5188, 0.2667, 0.0852), "root": (0.3384, 0.1511, 0.0747)},
    ("rainy", "day"):    {"tip": (0.1116, 0.1198, 0.0320), "root": (0.0817, 0.0736, 0.0205)},
    ("spring", "night"): {"tip": (0.0178, 0.0838, 0.0000), "root": (0.0000, 0.0657, 0.0000)},
    ("winter", "night"): {"tip": (0.0001, 0.0207, 0.0728), "root": (0.0000, 0.0178, 0.0802)},
    ("autumn", "night"): {"tip": (0.1066, 0.0415, 0.0000), "root": (0.0246, 0.0020, 0.0000)},
    ("rainy", "night"):  {"tip": (0.0003, 0.0093, 0.0041), "root": (0.0000, 0.0047, 0.0000)},
}

# Night dimming: the reference's night panels read much darker than the raw
# preset values do through JCE's linear->ACES->sRGB chain (their moody look
# leaned on Linear tonemapping + a dim env). One global factor per group,
# tuned against shot_spring_night vs the reference collage.
NIGHT_DIM_DOME  = 0.30
NIGHT_DIM_LIGHT = 0.22  # ~1/pi: our PBR lacks the reference's Lambert/pi — day saturates near 1 and hides it, night exposes it (rock 26x, tent 10x, shore 4.9x at 0.70)
NIGHT_DIM_SURF  = 0.60   # grass / rocks / water tints

# The reference lantern is a three.js PointLight(intensity 10, decay 1.5):
# physically-attenuated, so the tent reads as a contained warm patch. Our
# point falloff is far gentler and 10 floods the whole tent white — scale to
# match the reference patch (tent canvas night lum ~21 vs our 227 at 1.0).
LAMP_SCALE = 0.04

rows = []
for season in SEASONS:
    e = P[season]
    for tod in TIMES:
        sky   = e["sky"][tod]
        fog   = e["fog"][tod].get("color", [0, 0, 0])
        light = e["lighting"][tod]
        gnd   = e["ground"][tod]
        grass = e["grass"][tod]
        rocks = e["rocks"][tod]

        zen  = sky["zenithColor"]; hor = sky["horizonColor"]; skg = sky["groundColor"]
        # day: sun disk color (winter/rainy day have no disk -> intensity 0 handled below)
        body = sky.get("sunColor") or sky.get("moonColor") or [1, 1, 1]
        glow = sky.get("sunGlowColor") or sky.get("moonGlowColor") or body
        if tod == "night":
            zen = scale(zen, NIGHT_DIM_DOME); hor = scale(hor, NIGHT_DIM_DOME)
            skg = scale(skg, NIGHT_DIM_DOME)
            mid = mix(zen, hor, 0.5)
        else:
            # Reference skydome altitude curve: mix(horizon, zenith, a^2.5);
            # our dome's mid stop sits at midPos 0.45 -> t = 0.45^2.5 = 0.136.
            # Blend in DISPLAY space first, then invert the ACES chain so each
            # stop lands on the reference's raw bytes.
            mid = aces_inv3(mix(hor, zen, 0.45 ** 2.5))
            zen = aces_inv3(zen); hor = aces_inv3(hor); skg = aces_inv3(skg)
            # Reference disc = sunColor*1.2 + additive white center highlight;
            # our dome paints the disc with sunColor directly, so fold the
            # highlight into the display target before inverting.
            body = aces_inv3([min(body[i] * 1.2 + 0.3, 1.0) for i in range(3)])
            glow = aces_inv3(glow)
        # ES: sun disk renders only for spring/autumn day; moon for every night.
        disk_on = 1.0 if (tod == "night" or season in ("spring", "autumn")) else 0.0

        amb  = light["ambient"]; key = light["key"]; fil = light["fill"]
        lamp = light["lamp"]

        # season water extras: rainy = rings+splash-ish, winter = ice, else rings
        # Night rings read dimmer in the reference captures than the day
        # ones (their tonemap crushes the white strokes); halve the ratio.
        ripple = 0.0 if season == "winter" else (0.5 if tod == "night" else 1.0)
        ice    = 1.0 if season == "winter" else 0.0

        # rocks tint = mid of uRockColor1/2 (matches our single-tint approximation)
        rt = mix(rocks["uRockColor1"], rocks["uRockColor2"], 0.5)
        lt = LEAF_TINTS[(season, tod)]
        gr = grass["shadow"]
        gt = grass["light"]

        # JCE water semantics: shallow/deep mix by VIEW ANGLE (Fresnel), so a
        # top-down camera shows "shallow" across the whole plane. The
        # reference's uWaterShallow paints the SHORE (already baked into the
        # ground texture); the visible pond surface is its uWaterDeep. Feed
        # deep-teal into both JCE slots (deep slightly darker for grazing).
        wsh = gnd["uWaterDeep"]
        wdp = mix(gnd["uWaterDeep"], [0, 0, 0], 0.45)

        amb_int = light["ambient"]["intensity"]
        key_int = key["intensity"]
        fil_int = fil["intensity"]
        if tod == "night":
            rt  = scale(rt,  NIGHT_DIM_SURF * 0.5)  # rocks: ref reads near-black at night
            wsh = scale(wsh, NIGHT_DIM_SURF)
            wdp = scale(wdp, NIGHT_DIM_SURF)
            # key/fill must dim too — scaling only ambient (0.08) left the
            # dominant key (1.25) untouched and nights barely darkened.
            # Winter night reads notably brighter in the reference (blue
            # moonlit snow) than the other nights — give it a lighter dim.
            _dim = NIGHT_DIM_LIGHT * (1.8 if season == "winter" else 1.0)
            amb_int *= _dim
            key_int *= _dim
            fil_int *= _dim
        fit = GRASS_DISPLAY[(season, tod)]
        # Render-loop correction (tools/grass_corr.json, iterated by
        # fit_grass.py): our grass is LIT (ambient + wrapped dir lights in
        # fs_grass), so the authored albedo needs a per-state multiplier the
        # analytic fit can't see — especially at night where the 0.22x light
        # dim would otherwise crush the fitted values.
        _ck = f"{season}|{tod}"
        _c = GRASS_CORR.get(_ck, {"tip": [1, 1, 1], "root": [1, 1, 1]})
        gr = [aces_inv_lin(v) * m for v, m in zip(fit["root"], _c["root"])]
        gt = [aces_inv_lin(v) * m for v, m in zip(fit["tip"],  _c["tip"])]

        rows.append(f"""    {{ /* {season} / {tod} */
        .dome_zenith  = {f3(zen)}, .dome_mid = {f3(mid)},
        .dome_horizon = {f3(hor)}, .dome_ground = {f3(skg)},
        .body_color   = {f3(body)}, .glow_color = {f3(glow)}, .disk_on = {disk_on:.1f}f,
        .fog_color    = {f3(scale(fog, NIGHT_DIM_DOME) if tod == "night" else day_fog3(fog))},
        .ambient      = {f3(amb["color"])}, .ambient_intensity = {amb_int:.4f}f,
        .key_color    = {f3(key["color"])}, .key_intensity = {key_int:.4f}f,
        .fill_color   = {f3(fil["color"])}, .fill_intensity = {fil_int:.4f}f,
        .lamp_intensity = {lamp["intensity"] * LAMP_SCALE:.4f}f,
        .water_shallow = {f3(wsh)}, .water_deep = {f3(wdp)},
        .shore_ripple = {ripple:.1f}f, .ice_ratio = {ice:.1f}f,
        .grass_root   = {f3(gr)}, .grass_tip = {f3(gt)},
        .rocks_tint   = {f3(rt)}, .leaf_tint = {f3(lt)},
        .ground_tex   = "textures/ground_baked2/{season}_{tod}.png",
    }},""")

body_txt = "\n".join(rows)
hdr = f"""/*
 * es_palettes.h — GENERATED by build/gen_palettes_h.py from es_palettes.json
 * (extracted from the Elemental-Serenity reference). Do not edit by hand.
 *
 * Index: [season * 2 + tod] with season {{0=spring,1=winter,2=autumn,3=rainy}}
 * and tod {{0=day,1=night}}. All colors are LINEAR floats.
 */
#ifndef ES_PALETTES_H
#define ES_PALETTES_H

typedef struct {{
    float dome_zenith[3], dome_mid[3], dome_horizon[3], dome_ground[3];
    float body_color[3], glow_color[3];   /* sun (day) / moon (night) disk  */
    float disk_on;                        /* 1 = disk visible (ES gating)    */
    float fog_color[3];
    float ambient[3], ambient_intensity;
    float key_color[3], key_intensity;
    float fill_color[3], fill_intensity;
    float lamp_intensity;                 /* tent lantern                    */
    float water_shallow[3], water_deep[3];
    float shore_ripple, ice_ratio;
    float grass_root[3], grass_tip[3];
    float rocks_tint[3];
    float leaf_tint[3];                   /* bush/canopy card tint           */
    const char *ground_tex;               /* baked ground albedo (PAK path)  */
}} EsPreset;

enum {{ ES_SEASON_SPRING = 0, ES_SEASON_WINTER, ES_SEASON_AUTUMN, ES_SEASON_RAINY }};
enum {{ ES_TOD_DAY = 0, ES_TOD_NIGHT }};

static const EsPreset ES_PRESETS[8] = {{
{body_txt}
}};

#define ES_PRESET(season, tod) (&ES_PRESETS[(season) * 2 + (tod)])

#endif /* ES_PALETTES_H */
"""
open(OUT, "w", encoding="utf-8").write(hdr)
print(f"wrote {OUT} ({len(rows)} presets)")
