#!/usr/bin/env python3
"""
bake_ground.py — offline-evaluate the Elemental-Serenity ground shader
(ground.fragment_color_chunk.glsl, faithful port) into a baked diffuse
texture per season x time-of-day palette.

Reference pipeline (Ground.class.js + SeasonManager.class.js):
  ground mesh   = 5x5 tiles of 11  -> 55x55 plane
  vUv           = (world.xz + 33/2) / 33      (uGroundSize = WORLD_SIZE = 33)
  density map   : ClampToEdge  (768, path_data_rgb: G=grass density, B=water)
  displacement  : Repeat       (256, heightmap.r drives light->dark mix)
  perlin        : Repeat       (256, sampled at vUv*2)
  rock map / AO : Repeat       (256, sampled at vUv*6)

  densityMask = smoothstep(0.01, 1.25, density.g)
  X           = mix(perlin.rgb, 1.0, smoothstep(0.9, 1.0, density.g))
  rockMask    = smoothstep(0.0, 0.55, X.r)
  ground      = mix(uGroundColorLight, uGroundColorDark, height.r)
  ground      = mix(ground, uGroundColorBelowGrass, densityMask)
  rockColor   = rock.rgb * uRockColor * mix(1.0, rockAO, 2.0*rockMask)
  ground      = mix(rockColor, ground, rockMask)
  waterMask   = density.b   (softened where its gradient is steep)
  depthGrad   = pow(1 - smoothstep(0, 0.3, dist(vUv, (0.53, 0.535))), 1.0)
  waterColor  = mix(uWaterShallow, uWaterDeep, depthGrad)   # shallow = pond RIM
  final       = mix(ground, waterColor, waterMask)

All palette colors are LINEAR floats (three.js Color convention); the PNG
stores sRGB-encoded values so JCE's albedo decode (pow 2.2) round-trips.

Textures sample with three.js flipY: image row = (1 - v) * H.  The water
depth-gradient distance uses the UNFLIPPED vUv (it is a coordinate, not a
texture fetch).

Run:  python examples/elemental_serenity/tools/bake_ground.py
"""
import os
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
TEX  = os.path.join(PROJ, "resources", "assets", "textures")
OUTD = os.path.join(TEX, "ground_baked2")

SIZE       = 1280      # bake resolution (~23 texels/m, matches ref 768/33)
WORLD      = 55.0      # ground plane extent (reference 5x5 tiles of 11)
UGROUND    = 33.0      # ES uGroundSize (UV scale; density clamps beyond)
ROCK_TILE  = 6.0
WATER_CENTER = (0.53, 0.535)   # in vUv space (unflipped)

# ---- palettes: (season, time) -> dict of linear RGB triples ----------------
# Sourced from tools/es_palettes.json (run extract_palettes.py first).
import json as _json
with open(os.path.join(HERE, "es_palettes.json"), encoding="utf-8") as _f:
    _P = _json.load(_f)

PALETTES = {}
for _season, _e in _P.items():
    for _tod, _g in _e.get("ground", {}).items():
        PALETTES[(_season, _tod)] = dict(
            light=_g["uGroundColorLight"], dark=_g["uGroundColorDark"],
            below=_g["uGroundColorBelowGrass"], rock=_g["uRockColor"],
            shallow=_g["uWaterShallow"], deep=_g["uWaterDeep"])

def load_gray(path):
    im = Image.open(path).convert("L")
    return np.asarray(im, dtype=np.float32) / 255.0

def load_rgb(path):
    im = Image.open(path).convert("RGB")
    return np.asarray(im, dtype=np.float32) / 255.0

def _bilinear(tex, x, y):
    """Bilinear fetch at float pixel coords (already wrapped/clamped)."""
    h, w = tex.shape[0], tex.shape[1]
    x0 = np.floor(x).astype(np.int32); y0 = np.floor(y).astype(np.int32)
    fx = (x - x0).astype(np.float32);  fy = (y - y0).astype(np.float32)
    x1 = np.minimum(x0 + 1, w - 1);    y1 = np.minimum(y0 + 1, h - 1)
    x0 = np.clip(x0, 0, w - 1);        y0 = np.clip(y0, 0, h - 1)
    if tex.ndim == 3:
        fx = fx[..., None]; fy = fy[..., None]
    a = tex[y0, x0]; b = tex[y0, x1]; c = tex[y1, x0]; d = tex[y1, x1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy

def sample_repeat(tex, u, v):
    """three.js RepeatWrapping + flipY: image row = (1 - v) * H."""
    h, w = tex.shape[0], tex.shape[1]
    x = np.mod(u, 1.0) * (w - 1)
    y = np.mod(1.0 - v, 1.0) * (h - 1)
    return _bilinear(tex, x, y)

def sample_clamp(tex, u, v):
    """three.js ClampToEdgeWrapping + flipY."""
    h, w = tex.shape[0], tex.shape[1]
    x = np.clip(u, 0.0, 1.0) * (w - 1)
    y = np.clip(1.0 - v, 0.0, 1.0) * (h - 1)
    return _bilinear(tex, x, y)

def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)

def mix(a, b, t):
    return a * (1.0 - t) + b * t

def bake(season, tod):
    P = PALETTES[(season, tod)]
    density  = load_rgb(os.path.join(TEX, "grass", "path_data_rgb_768x768.png"))
    height   = load_gray(os.path.join(TEX, "grass", "displacement_map_256x256.png"))
    perlin   = load_gray(os.path.join(TEX, "noises", "perlin_noise_256x256.png"))
    rocktex  = load_rgb(os.path.join(TEX, "ground", "rocks_height_256x256.png"))
    rockao   = load_gray(os.path.join(TEX, "ground", "rocks_ao_256x256.png"))

    # output texel grid -> world -> reference vUv (may exceed [0,1] on the
    # 55-plane; density clamps there, the tiled maps repeat, like three.js)
    px = (np.arange(SIZE, dtype=np.float32) + 0.5) / SIZE
    u_plane, v_plane = np.meshgrid(px, px)
    wx = (u_plane - 0.5) * WORLD
    wz = (v_plane - 0.5) * WORLD
    u = (wx + UGROUND * 0.5) / UGROUND
    v = (wz + UGROUND * 0.5) / UGROUND     # vUv.y, unflipped coordinate

    dens = sample_clamp(density, u, v)                          # HxWx3
    hgt  = sample_repeat(height,  u, v)                         # HxW
    per  = sample_repeat(perlin,  u * 2.0, v * 2.0)
    rk   = sample_repeat(rocktex, u * ROCK_TILE, v * ROCK_TILE)
    ao   = sample_repeat(rockao,  u * ROCK_TILE, v * ROCK_TILE)

    def col(c):  # broadcast a palette triple
        return np.array(c, dtype=np.float32).reshape(1, 1, 3)

    dens_g = dens[..., 1]
    densityMask = smoothstep(0.01, 1.25, dens_g)[..., None]
    X = mix(per, 1.0, smoothstep(0.9, 1.0, dens_g))
    rockMask = smoothstep(0.0, 0.55, X)[..., None]

    ground = mix(col(P["light"]), col(P["dark"]), hgt[..., None])
    ground = mix(ground, col(P["below"]), densityMask)

    # mix(1.0, rockAO, 2.0*rockMask) EXTRAPOLATES past t=1 like GLSL mix
    ao_term = 1.0 + (ao[..., None] - 1.0) * (2.0 * rockMask)
    rockColor = rk * col(P["rock"]) * np.clip(ao_term, 0.0, None)
    ground = mix(rockColor, ground, rockMask)

    # water mask with the reference's screen-space edge softening,
    # approximated on the texel grid (dFdx -> per-texel finite difference)
    wmask = dens[..., 2]
    gy, gx = np.gradient(wmask)
    edgeStrength = np.sqrt(gx * gx + gy * gy) * 5.0
    edgeSoftness = smoothstep(0.0, 1.0, edgeStrength)
    wmask = mix(wmask, wmask * (1.0 - edgeSoftness * 0.7),
                np.minimum(edgeStrength, 1.0))

    dist = np.sqrt((u - WATER_CENTER[0]) ** 2 + (v - WATER_CENTER[1]) ** 2)
    depthGradient = 1.0 - smoothstep(0.0, 0.3, dist)   # uWaterDepthIntensity=1
    water = mix(col(P["shallow"]), col(P["deep"]), depthGradient[..., None])
    final = mix(ground, water, wmask[..., None])

    # linear -> sRGB for the PNG (JCE albedo path decodes with pow 2.2)
    srgb = np.clip(final, 0.0, 1.0) ** (1.0 / 2.2)
    img = Image.fromarray((srgb * 255.0 + 0.5).astype(np.uint8), "RGB")
    os.makedirs(OUTD, exist_ok=True)
    out = os.path.join(OUTD, f"{season}_{tod}.png")
    img.save(out)
    print(f"baked {out}")

def bake_water_data():
    """Emit textures/water_data2.png for the STYLIZED water overlay:
    R = water depth (authored water_depth_map_256x256.png .b, the field the
    reference ripple shader rings march along), G = in-water mask
    (smoothstep(0.05, 0.15, density.b), the reference uDensityMaskMin/Max).
    Baked over the water PLANE's local UV so fs_water's v_texcoord0 lands on
    the same values the reference reads via world-space vUv.  Grid must match
    gen_scene.py's Water entity: center (-0.2, 1.3), size 11.5 x 13."""
    density = load_rgb(os.path.join(TEX, "grass", "path_data_rgb_768x768.png"))
    wdepth  = load_rgb(os.path.join(TEX, "water", "water_depth_map_256x256.png"))
    N = 256
    cx, cz, sx, sz = -0.2, 1.3, 11.5, 13.0
    px = (np.arange(N, dtype=np.float32) + 0.5) / N
    uq, vq = np.meshgrid(px, px)
    wx = cx + (uq - 0.5) * sx
    wz = cz + (vq - 0.5) * sz
    u = (wx + UGROUND * 0.5) / UGROUND
    v = (wz + UGROUND * 0.5) / UGROUND
    dens  = sample_clamp(density, u, v)
    depth = sample_clamp(wdepth,  u, v)[..., 2]          # .b channel
    mask  = smoothstep(0.05, 0.15, dens[..., 2])         # uDensityMaskMin/Max

    img = np.zeros((N, N, 3), dtype=np.uint8)
    img[..., 0] = (np.clip(depth, 0, 1) * 255.0 + 0.5).astype(np.uint8)
    img[..., 1] = (np.clip(mask,  0, 1) * 255.0 + 0.5).astype(np.uint8)
    outp = os.path.join(os.path.dirname(OUTD), "water_data2.png")
    Image.fromarray(img, "RGB").save(outp)
    print(f"baked {outp}")

if __name__ == "__main__":
    for key in PALETTES:
        bake(*key)
    bake_water_data()
