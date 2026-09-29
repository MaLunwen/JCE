#!/usr/bin/env python3
"""
gen_scene.py — emit resources/assets/scenes/elemental_serenity.scene.json
from the extracted Elemental-Serenity reference data.

Static "summer-night" (ES spring+night) diorama: 5 placed models + material
overrides, 39 bush leaf-clusters, night light rig, VirtualCamera beauty pose,
GPU grass field, stylized water pond, and the night rendering block.

Run:  python examples/elemental_serenity/tools/gen_scene.py
"""
import json, os, math, random

random.seed(1337)  # deterministic scatter

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
OUT  = os.path.join(PROJ, "resources", "assets", "scenes", "elemental_serenity.scene.json")

_next_id = [1]
def nid():
    _next_id[0] += 1
    return _next_id[0]

def ent(name, comps, parent=0, eid=None):
    return {"id": eid if eid is not None else nid(), "name": name, "parentId": parent, "components": comps}

def transform(p=(0,0,0), r=(0,0,0), s=(1,1,1)):
    return {"type":"Transform","posX":p[0],"posY":p[1],"posZ":p[2],
            "rotX":r[0],"rotY":r[1],"rotZ":r[2],"scaleX":s[0],"scaleY":s[1],"scaleZ":s[2]}

def meta(name):
    return {"type":"EditorMeta","name":name}

def mesh(path, base=(1,1,1,1), rough=0.9, metal=0.0, albedo_tex="", normal_tex="",
         double_sided=False, alpha_mode=0, alpha_cutoff=0.5, emissive=(0,0,0),
         casts=True, receives=True):
    m = {"type":"MeshRenderer","meshPath":path,"meshShape":0,
         "baseColorR":base[0],"baseColorG":base[1],"baseColorB":base[2],"baseColorA":base[3],
         "metallic":metal,"roughness":rough,
         "emissiveR":emissive[0],"emissiveG":emissive[1],"emissiveB":emissive[2],
         "normalScale":1,"aoStrength":1,"alphaMode":alpha_mode,"alphaCutoff":alpha_cutoff,
         "doubleSided":double_sided,"castsShadow":casts,"receivesShadow":receives}
    if albedo_tex: m["albedoTex"] = albedo_tex
    if normal_tex: m["normalTex"] = normal_tex
    return m

def light_dir(color, intensity, direction, shadow=False):
    return {"type":"Light","lightType":0,"colorR":color[0],"colorG":color[1],"colorB":color[2],
            "intensity":intensity,"dirX":direction[0],"dirY":direction[1],"dirZ":direction[2],
            "castsShadow":shadow}

def light_point(color, intensity, radius):
    return {"type":"Light","lightType":1,"colorR":color[0],"colorG":color[1],"colorB":color[2],
            "intensity":intensity,"radius":radius,"castsShadow":False}

entities = []

# ---------------------------------------------------------------- lights ----
def norm(v):
    l = math.sqrt(sum(c*c for c in v)) or 1.0
    return tuple(c/l for c in v)

# Key light — reference day key from pos(-15,12,8) => ~35 deg elevation (was a
# grazing 25 deg that left the ground/paths in long foliage shadow).  Higher
# sun = shorter shadows + more light on the flat ground.  Driver retints/rescales
# per state; direction stays.
entities.append(ent("Sun", [transform(p=(-15,12,8)),
    light_dir((0.42,0.55,0.72), 3.0, norm((15,-12,-8)), shadow=True), meta("Sun")]))
# Fill (cool, from pos(10,5,-6))
entities.append(ent("Fill", [transform(p=(10,5,-6)),
    light_dir((0.24,0.35,0.48), 0.15, norm((-10,-5,6))), meta("Fill")]))
# Rim light (reference lights.rim, pos(5,10,-12)): warm peach by day, cool
# blue-grey by night. Driver retints per day/night.
entities.append(ent("Rim", [transform(p=(5,10,-12)),
    light_dir((0.478,0.561,0.667), 0.1, norm((-5,-10,12))), meta("Rim")]))
# Tent lamp (ES 0xFFE286 @10 dist 20 decay 1.5; JCE point falloff is tighter-
# radius based — r20 flooded half the diorama, r9 reads like the reference)
entities.append(ent("TentLamp", [transform(p=(2.9,4.6,-5.5)),
    light_point((1.0,0.886,0.525), 5.0, 9.0), meta("TentLamp")]))
# Campfire flickering point lights (ES Fire.class; flicker added by driver in M3)
entities.append(ent("CampfireLight", [transform(p=(-5.5,1.0,-7.0)),
    light_point((0.97,0.42,0.106), 4.0, 4.0), meta("CampfireLight")]))
entities.append(ent("CampfireLight2", [transform(p=(-5.5,0.5,-7.0)),
    light_point((0.97,0.5,0.18), 2.0, 1.0), meta("CampfireLight2")]))

# ---------------------------------------------------------------- camera ----
entities.append(ent("BeautyCam", [transform(p=(18.25,10.69,27.32)),
    {"type":"VirtualCamera","name":"BeautyCam","priority":100,"active":True,"trackMode":0,
     "posX":18.25,"posY":10.69,"posZ":27.32,"lookX":0,"lookY":0,"lookZ":0,
     "fov":25,"offX":0,"offY":0,"offZ":0,"damping":0,"followTarget":0,"lookAtTarget":0},
    meta("BeautyCam")]))

# ------------------------------------------------------------ static models -
# Tent keeps its glTF materials (canvas/wood/lamp colors baked correctly).
# doubleSided: the canvas is an open single-layer sheet — with backface
# culling its inside face vanishes at eye level.
entities.append(ent("Tent", [transform(p=(2.5,0.6,-9), r=(0,-3,0), s=(1.1,1.1,1.1)),
    mesh("models/tent.glb", base=(1.0,1.0,1.0,1), double_sided=True), meta("Tent")]))
# bridge.glb lost its per-node X-90 rotation + (1.09,2.10,1.41) scale in the
# Draco->flat conversion (geometry is Z-up: z=height). Entity compensates:
# rotX -90 restores Y-up; scale maps model (x=len, y=width, z=height) to the
# reference proportions. Wood texture + warm tint per the reference.
# Reference transforms verbatim (the fixed --convert-model bakes each node's
# TRS into the vertices, so no entity-side compensation is needed).
entities.append(ent("Bridge", [transform(p=(-8,1.5,1.25), r=(0,180,6), s=(0.8,0.8,0.85)),
    mesh("models/bridge.glb", base=(0.55,0.40,0.18,1),
         albedo_tex="textures/wood/wood_color_256x256.png",
         normal_tex="textures/wood/wood_normal_256x256.png"),
    meta("Bridge")]))
entities.append(ent("Rocks", [transform(), mesh("models/rocks.glb", base=(0.68,0.57,0.31,1), rough=1.0),
    meta("Rocks")]))
entities.append(ent("Trees", [transform(), mesh("models/treeTrunks.glb", base=(0.45,0.30,0.16,1), rough=1.0),
    meta("Trees")]))
# Camp: the glb carries its OWN two flat materials ("rocks" 0.639,0.446,0.247
# orange-tan fire-pit stones + "Wood" 0.316,0.169,0.078 dark logs), matching the
# reference (Camp.class.js adds the model untouched — no texture, no tint).  A
# non-white MeshRenderer base multiplies through as a per-instance TINT, so the
# former (0.7,0.68,0.62) darkened + desaturated both browns ~30% into a muddy
# uniform blob that read as "no texture".  White = no tint = the model's own
# vivid two-tone materials, 1:1 with the reference.
entities.append(ent("Camp", [transform(),
    mesh("models/camp.glb", base=(1,1,1,1), rough=1.0),
    meta("Camp")]))

# ------------------------------------------------------------------ ground --
# Flat ground plane (baked biome texture wired in a later task; solid dark
# earth for now). meshShape 0 needs a mesh; reuse a large flat scaled quad by
# stretching the leaf card is wrong, so we drive ground via a big scaled cube
# primitive is unavailable here -> use the terrain-less approach: a wide, thin
# scaled plane from the grass system's ground. For M1 we place a large dark
# disc via the water? No -> emit a MeshRenderer that references a generated
# ground.glb if present; otherwise skip (grass + water read as the surface).
GROUND_GLB = os.path.join(PROJ, "resources", "assets", "models", "ground_plane55.glb")
if os.path.exists(GROUND_GLB):
    entities.append(ent("Ground", [transform(),
        # single-sided: with cull-none the flat quad shades as its BACK face
        # (flipped normal -> no sun N.L + hemisphere ground tint) and the
        # whole basin rendered dark navy regardless of the baked texture.
        mesh("models/ground_plane55.glb", base=(1.0,1.0,1.0,1), rough=1.0, casts=False,
             double_sided=False,
             albedo_tex="textures/ground_baked2/spring_night.png"),
        meta("Ground")]))

# ------------------------------------------------------------------ bushes --
# 39 leaf-clusters. Each = a small scatter of leaf.glb cards (green, double-sided).
# type: default (low rim bushes), tree/birch (elevated canopy puffs).
LEAF = "models/leaf.glb"
LEAF_ALPHA = "textures/bush/leave_alpha_map_256x256.png"
BUSHES = [
    # (x,y,z, scale, type)
    (7.3,1.0,3,1.2,'d'),(9,0.2,4.1,0.6,'d'),(10,0.3,0.0,0.6,'d'),(11,0.1,1.5,0.8,'d'),
    (-10,0.7,-5.5,1.2,'d'),(-12,1.0,-5.5,2.0,'d'),(-11,0.2,-8.5,0.7,'d'),(-2,0.2,-7.5,1.0,'d'),
    (8,0.5,-9.5,0.6,'d'),(-4.0,0.5,10.5,0.7,'d'),(0.0,0.5,11.5,0.5,'d'),(1.8,0.2,9.5,0.5,'d'),
    (-4,0.0,-15.5,1.0,'d'),(-6,0.0,-15,0.9,'d'),(-9.8,0.5,4.5,1.2,'d'),(-8.8,0.5,8.5,1.0,'d'),
    (-6.5,0.1,8.5,0.8,'d'),
    (12.0,5.0,-0.2,0.6,'t'),(12.0,7.0,1.5,0.7,'t'),(12.5,5.0,3.2,0.7,'t'),(13.5,5.0,0.5,0.6,'t'),
    (11.0,6.0,2.5,0.6,'t'),(-10.5,4.5,0.0,1.0,'t'),(-9.5,5.0,-2.5,1.0,'t'),(-8,4.0,-2.5,1.0,'t'),
    (-7,3.7,-9.0,1.0,'t'),(-7,5.0,-11.0,1.0,'t'),(-5,3.7,-11.0,1.0,'t'),(-10,6.0,7.0,1.0,'t'),
    (-11,6.0,5.0,1.0,'t'),(-12,4.0,4.0,1.0,'t'),(-12,6.0,6.0,1.0,'t'),(-12,4.0,7.0,1.0,'t'),
    (8.1,6.5,-5.5,1.0,'b'),(8.5,7.5,-8.5,1.0,'b'),(6.0,7.5,-7.5,1.0,'b'),(-3.1,8.0,10.5,1.0,'b'),
    (-3.0,6.0,10.5,1.5,'b'),(-5.0,7.5,11.5,1.0,'b'),(-4.0,6.0,12.5,1.0,'b'),
]
# FoliageCluster entities (engine module): billboard cards on a squashed
# sphere shell + 3-tone toon ramp keyed on the shell normal — the reference
# BushManager model. Names prefix by type so the Lua director can retint
# each variant group per season x time-of-day.
FC_NAME  = {'d': 'BushC', 't': 'TreeC', 'b': 'BirchC'}
# spring/day authored colors (director re-applies at boot): per type
FC_COLORS = {
  'd': ((0.003,0.074,0.003),(0.06,0.23,0.0),(0.44,0.5,0.0),(0.46,0.65,0.3)),
  't': ((0.03,0.07,0.003),(0.06,0.23,0.0),(0.45,0.55,0.002),(0.77,0.71,0.35)),
  'b': ((0.09,0.03,0.0),(0.2,0.03,0.0),(1.0,0.58,0.1),(0.68,0.56,0.22)),
}
LC30 = {14, 15, 16}   # reference bushes with leafCount 30 (rest default 45)
for i,(x,y,z,sc,ty) in enumerate(BUSHES):
    sh, mi, hi, mu = FC_COLORS[ty]
    entities.append(ent(f"{FC_NAME[ty]}_{i}", [transform(p=(x,y,z)),
        {"type": "FoliageCluster",
         "leafCount": 30 if i in LC30 else 45,
         # shell = reference bushEmitter.glb: a FULL sphere of radius 0.84
         # (unscaled — only the CARDS grow with the def scale). Our old 1.2 +
         # squash 0.8 spread the same cards over a 43% bigger shell, reading
         # as wispy see-through canopies with horizon gaps.
         "radius": 0.84, "squashY": 1.0, "leafScale": sc, "seed": 1000+i,
         "shadowR": sh[0], "shadowG": sh[1], "shadowB": sh[2],
         "midR": mi[0], "midG": mi[1], "midB": mi[2],
         "highR": hi[0], "highG": hi[1], "highB": hi[2],
         "multR": mu[0], "multG": mu[1], "multB": mu[2],
         "alphaTex": LEAF_ALPHA, "visible": True},
        meta(f"{FC_NAME[ty]}_{i}")]))

# ------------------------------------------------------------------- grass --
# Four strips ringing the pond footprint (x ~[-6.4,6.0], z ~[-5.2,7.9]) so no
# blades grow through the water (JCE GrassField has no density mask).
# Density mask carves the dirt paths + pond + camp clearing out of the grass
# (reference GrassManager grows blades only where density >= 0.9). The mask's
# GREEN channel is grass density; world size 33 = ES uGroundSize so it aligns
# with the baked ground paths.
# .bin extension: passes through the cook verbatim (a cooked jtex is not
# stb-decodable); the scatter mask loader content-sniffs the PNG inside.
GRASS_MASK = "textures/grass/path_data_mask.bin"
def grass_field(name, cx, cz, ax, az):
    # y=-0.3: reference sinks the whole field so blade roots sit below ground.
    return ent(name, [transform(p=(cx,-0.3,cz)),
        # density 103/m² = reference 12500 blades/tile over 11x11 m — at the
        # old 18/m² sparse blades read as isolated TUFTS with bare ground
        # between them instead of the reference's continuous carpet.
        {"type":"GrassField","density":103.0,"seed":1337,"areaX":ax,"areaZ":az,
         "maxSlopeDeg":30,"scaleMin":1.40,"scaleMax":2.84,"bladeHeight":0.515,"bladeWidth":0.16,"cards":1,
         "rootR":0.0014,"rootG":0.024,"rootB":0.0,"tipR":0.136,"tipG":0.186,"tipB":0.016,
         "windDirX":1.0,"windDirZ":0.0,"windSpeed":0.9,"windAmplitude":0.5,
         "fadeStart":40,"fadeEnd":90,"hueJitter":0.08,"castShadow":False,"visible":True,
         "densityMaskPath":GRASS_MASK,"densityThreshold":0.9,"maskWorldSize":33.0},
        meta(name)])
# Full-meadow coverage in a 2x2 grid (each < instance cap); the mask carves the
# paths, pond and camp clearing, so no hand-authored strips are needed.
H = 16.5
entities.append(grass_field("GrassNW", -H/2,  H/2, H, H))  # y offset via transform below
entities.append(grass_field("GrassNE",  H/2,  H/2, H, H))
entities.append(grass_field("GrassSW", -H/2, -H/2, H, H))
entities.append(grass_field("GrassSE",  H/2, -H/2, H, H))

# ------------------------------------------------------------------- water --
# STYLIZED ripple OVERLAY (reference architecture): the water BODY color is
# painted in the baked ground; this flat plane draws only shore-hugging
# ripple strokes + rain splash circles + winter ice plates (fs_water mode 2,
# discard elsewhere). shore-distance map baked by bake_ground.bake_water_data.
entities.append(ent("Pond", [transform(p=(-0.2,0.1,1.3)),
    {"type":"Water","sizeX":11.5,"sizeZ":13.0,"baseHeight":0.0,"waveCount":0,
     "shallowR":0.03,"shallowG":0.25,"shallowB":0.3,"deepR":0.017,"deepG":0.14,"deepB":0.17,
     "transparency":0.85,"sunSpecular":0.0,"shoreRipple":1.0,"iceRatio":0.0,
     "splashRatio":0.0,"dataTex":"textures/water_data2.png",
     "visible":True,"waterMode":2},
    meta("Pond")]))

# --------------------------------------------------------------- particles --
def emitter(asset, rate, lmin, lmax):
    return {"type":"ParticleEmitter","assetPath":asset,
            "emitRate":rate,"lifetimeMin":lmin,"lifetimeMax":lmax,"gpu":True}

# ES fire/amber emitters at (-5.4,1.0,-6.9); smoke at y=1.9; fireflies ring around origin.
entities.append(ent("FireFlame", [transform(p=(-5.4,1.0,-6.9)),
    emitter("particles/campfire_flame.particles.json", 90, 0.45, 1.15), meta("FireFlame")]))
entities.append(ent("FireEmbers", [transform(p=(-5.4,1.0,-6.9)),
    emitter("particles/campfire_embers.particles.json", 22, 1.6, 3.0), meta("FireEmbers")]))
entities.append(ent("FireSmoke", [transform(p=(-5.4,1.9,-6.9)),
    emitter("particles/campfire_smoke.particles.json", 16, 2.2, 3.4), meta("FireSmoke")]))
# Fireflies: reference ring of 50 in an annulus r=9..16 around the pond,
# approximated by 12 low-spread hovering emitters (~4 alive each ~= 48).
entities.append(ent("Firefly_0", [transform(p=(10.50,1.0,0.00)),
    emitter("particles/fireflies_ring.particles.json", 1.4, 2.5, 5.0), meta("Firefly_0")]))
entities.append(ent("Firefly_1", [transform(p=(12.12,1.0,7.00)),
    emitter("particles/fireflies_ring.particles.json", 1.4, 2.5, 5.0), meta("Firefly_1")]))
entities.append(ent("Firefly_2", [transform(p=(5.25,1.0,9.09)),
    emitter("particles/fireflies_ring.particles.json", 1.4, 2.5, 5.0), meta("Firefly_2")]))
entities.append(ent("Firefly_3", [transform(p=(0.00,1.0,14.00)),
    emitter("particles/fireflies_ring.particles.json", 1.4, 2.5, 5.0), meta("Firefly_3")]))
entities.append(ent("Firefly_4", [transform(p=(-5.25,1.0,9.09)),
    emitter("particles/fireflies_ring.particles.json", 1.4, 2.5, 5.0), meta("Firefly_4")]))
entities.append(ent("Firefly_5", [transform(p=(-12.12,1.0,7.00)),
    emitter("particles/fireflies_ring.particles.json", 1.4, 2.5, 5.0), meta("Firefly_5")]))
entities.append(ent("Firefly_6", [transform(p=(-10.50,1.0,0.00)),
    emitter("particles/fireflies_ring.particles.json", 1.4, 2.5, 5.0), meta("Firefly_6")]))
entities.append(ent("Firefly_7", [transform(p=(-12.12,1.0,-7.00)),
    emitter("particles/fireflies_ring.particles.json", 1.4, 2.5, 5.0), meta("Firefly_7")]))
entities.append(ent("Firefly_8", [transform(p=(-5.25,1.0,-9.09)),
    emitter("particles/fireflies_ring.particles.json", 1.4, 2.5, 5.0), meta("Firefly_8")]))
entities.append(ent("Firefly_9", [transform(p=(-0.00,1.0,-14.00)),
    emitter("particles/fireflies_ring.particles.json", 1.4, 2.5, 5.0), meta("Firefly_9")]))
entities.append(ent("Firefly_10", [transform(p=(5.25,1.0,-9.09)),
    emitter("particles/fireflies_ring.particles.json", 1.4, 2.5, 5.0), meta("Firefly_10")]))
entities.append(ent("Firefly_11", [transform(p=(12.12,1.0,-7.00)),
    emitter("particles/fireflies_ring.particles.json", 1.4, 2.5, 5.0), meta("Firefly_11")]))

# ------------------------------------------------------------------ flowers -
# Reference (GrassManager.createFlowersWithAtlas): 20 candidates per tile x
# 3x3 tiles of 11, kept only where the density map's GREEN channel >= 0.9 at
# the candidate's world position (u = x/33+0.5, image row = (1-v)*H — same
# V-flipped sampling as the grass mask).  Each kept flower is a VERTICAL
# 0.4x0.4 quad facing +Z at y = 0.7 + rand*0.2, scale 0.6..1.0.
FLOWER_TEX = ["textures/flowers/flower_1_128x128.png",
              "textures/flowers/flower_2_128x128.png"]

from PIL import Image as _Img
_dens_img = _Img.open(os.path.join(
    PROJ, "resources", "assets", "textures", "grass",
    "path_data_rgb_768x768.png")).convert("RGB")
_dens_w, _dens_h = _dens_img.size
def _grass_density(wx, wz, _px=_dens_img.load()):
    u = wx / 33.0 + 0.5
    v = wz / 33.0 + 0.5
    x = max(0, min(_dens_w - 1, int(u * _dens_w)))
    y = max(0, min(_dens_h - 1, int((1.0 - v) * _dens_h)))
    return _px[x, y][1] / 255.0

fi = 0
for ti in range(3):
    for tj in range(3):
        tile_x = -11.0 + ti * 11.0
        tile_z = -11.0 + tj * 11.0
        for _ in range(20):                      # FLOWERS_PER_TILE
            fx = tile_x - 5.5 + random.random() * 11.0
            fz = tile_z - 5.5 + random.random() * 11.0
            if _grass_density(fx, fz) < 0.9:
                continue
            fsc = 0.4 * (0.6 + random.random() * 0.4)   # quad 0.4 x scale 0.6..1.0
            fy  = 0.7 + random.random() * 0.2
            entities.append(ent(f"flower_{fi}", [
                transform(p=(fx, fy, fz), r=(90, 0, 0), s=(fsc, fsc, fsc)),
                mesh("models/card.glb", base=(1.0, 1.0, 1.0, 1), rough=1.0,
                     double_sided=True, casts=False,
                     albedo_tex=FLOWER_TEX[fi % 2], alpha_mode=1, alpha_cutoff=0.4),
                meta(f"flower_{fi}")]))
            fi += 1

# ----------------------------------------------------------------- weather --
# Season-gated emitters (driver toggles emitting): rain (rainy), snow (winter).
# ES bounds: precipitation spawns y 15..20 over x/z ~±20 around the diorama —
# approximated by a high emitter with a wide lateral velocity spread.
# Rain: point transform at y=17; the .particles.json spawnBox [26,4,26] fans the
# drops across the whole pond area (reference RainSystem uses a 40x40 area, ~800
# concurrent near-vertical drops, additive).  NOTE: with assetPath set the asset
# fields are authoritative — these emitter() args are ignored (kept in sync for
# readers).
entities.append(ent("Rain", [transform(p=(0.0,17.0,0.0)),
    emitter("particles/rain.particles.json", 820, 1.1, 1.6), meta("Rain")]))
entities.append(ent("Snow", [transform(p=(0.0,17.0,0.0)),
    emitter("particles/snow.particles.json", 80, 6.0, 9.0), meta("Snow")]))
# Falling leaves (ES FallingLeavesSystem: ~35 drifting leaf cards, slow fall +
# rotation, respawn at top). One wide emitter high over the diorama; the
# director tints per season. Always emitting, denser in autumn.
entities.append(ent("FallingLeaves", [transform(p=(-4.0,8.5,10.0)),
    emitter("particles/falling_leaves.particles.json", 4.7, 5.0, 7.0),
    meta("FallingLeaves")]))
entities.append(ent("FallingLeaves2", [transform(p=(4.0,8.5,-10.0)),
    emitter("particles/falling_leaves.particles.json", 4.7, 5.0, 7.0),
    meta("FallingLeaves2")]))

# Wind-line swoosh holders — LineRenderers driven by es_runtime.c (3 pooled
# ribbons, ES WindLines: spawn every 0.3-2s, 4s sweep at y=3, season-tinted).
for wi in range(3):
    entities.append(ent(f"WindLine{wi}", [transform(),
        {"type":"LineRenderer","materialPath":"","positionCount":0,
         # reference WindLine thickness 0.25 total, TAPERED both ends via its
         # vertex window; our line lerps width linearly, so keep it thin+even
         "widthStart":0.09,"widthEnd":0.09,
         "colorStartR":1.0,"colorStartG":1.0,"colorStartB":1.0,"colorStartA":0.0,
         "colorEndR":1.0,"colorEndG":1.0,"colorEndB":1.0,"colorEndA":0.0,
         "useWorldSpace":True,"loop":False},
        meta(f"WindLine{wi}")]))

# Lightning arc holder — LineRenderer driven entirely by es_lightning.c
# (position_count 0 = invisible until a strike fires).
# Lightning strike-point explosion burst (director repositions + bursts it).
entities.append(ent("LightningBurst", [transform(p=(0.0,0.4,0.0)),
    emitter("particles/lightning_burst.particles.json", 0, 0.4, 0.9), meta("LightningBurst")]))
entities.append(ent("LightningArc", [transform(),
    {"type":"LineRenderer","materialPath":"","positionCount":0,
     "widthStart":0.07,"widthEnd":0.03,
     "colorStartR":0.0,"colorStartG":0.0,"colorStartB":1.0,"colorStartA":1.0,
     "colorEndR":0.0,"colorEndG":1.0,"colorEndB":1.0,"colorEndA":1.0,
     "useWorldSpace":True,"loop":False},
    meta("LightningArc")]))

# ---------------------------------------------------------------- rendering -
rendering = {
  "version": 1,
  "ambient": { "color": [0.29,0.33,0.40], "intensity": 0.08 },
  "iblEnabled": False,
  "fog": { "enabled": True, "mode": 1, "color": [0.0,0.011,0.039], "density": 0.0,
           "start": 47, "end": 57, "heightFalloff": 0.0, "heightOrigin": 0 },
  "shadows": { "distance": 300, "cascades": 4, "splitLambda": 0.7, "resolution": 4096, "soft": 1 },
  "postfx": { "tonemap": True, "bloom": True, "fxaa": False, "vignette": False,
              "exposure": 0.92, "gamma": 2.2, "bloomThreshold": 1.15, "bloomIntensity": 0.24,
              "ssao": False, "ssaoIntensity": 0.0, "ssaoRadius": 0.85, "ssr": False },
  "environment": {
    "timeOfDay": { "enabled": False, "hour": 22 },
    "weather": { "type": 0, "intensity": 0 },
    "sky": { "mode": 3, "turbidity": 2.5,
      "dome": { "zenith": [0.0084,0.021,0.063], "mid": [0.0147,0.0315,0.084], "midPos": 0.45,
                "horizon": [0.021,0.042,0.105], "ground": [0.042,0.063,0.126],
                "glow": [0.021,0.025,0.042], "glowFalloff": 10.0,
                "sunColor": [0.95,0.95,1.0], "sunSize": 0.9992, "sunSoftness": 0.0015,
                # tight halo: pow^40 keeps the glow hugging the disk (a wide
                # pow^8 * 0.15 cone washed the whole night sky + tripped bloom)
                "haloPower": 40.0, "haloStrength": 0.07,
                # sun rays authored OFF at boot (night); the director turns
                # them on for day states (reference animeSun 12 petals)
                # reference sky = SphereGeometry(150) at the WORLD origin:
                # horizon + sun/moon parallax with the camera
                "anchorRadius": 150.0,
                "sunRayCount": 0.0, "sunRayLength": 0.0352,
                "sunRaySharpness": 8.0, "sunRayStrength": 0.55,
                # near-horizon moon, low enough to sit INSIDE the fov-25 frame
                "sunDir": [-0.5, -0.085, -1.0] } },
    "floatingOrigin": { "enabled": False, "threshold": 4096 }
  },
  # rimIntensity MUST stay 0: the look rim (meadow_valley template leftover)
  # adds pow(1-N.V,3)*rimColor to every PBR surface — the diorama ground is
  # always grazing from the beauty camera, so it painted a constant pale
  # blue-gray slab over the basin day AND night (immune to light changes).
  "look": { "wrap": 0.35, "hemisphere": True, "groundColor": [0.10,0.15,0.30],
            "rimColor": [0.48,0.56,0.67], "rimPower": 3.0, "rimIntensity": 0.0,
            "tonemapOp": 0, "lutPath": "", "lutStrength": 0.0, "toonCharacter": False, "bloomKnee": 0.5 }
}

# ------------------------------------------------------------ director + HUD
# The season/weather/audio driver is a scene-bound Lua script so editor Play
# and the shipped runtime simulate identically (no app-exe logic).
entities.append(ent("Director", [transform(),
    {"type": "Script", "scriptPath": "scripts/es_director.lua"},
    meta("Director")]))

# ------------------------------------------------- the other four languages
# One entity per language, each owning work the Lua director does NOT do, so
# no two scripts write the same field of the same component:
#
#   Fireflies    python  scripts/es_fireflies.py     Firefly_* TRANSFORM +
#                                                    colour (director owns
#                                                    `emitting`)
#   Campfire     java    scripts/EsCampfire.java     CampfireLight2, which the
#                                                    director never resolves
#   FlowerSway   cpp     scripts/EsFlowerSway.jcecpp flower_* ROTATION
#                                                    (director owns `visible`)
#   PropSurface  c       scripts/EsPropSurface.jcec  Rocks/Trees/Bridge/Camp/
#                                                    Tent MeshRenderer
#                                                    baseColor + ROUGHNESS
#
# PropSurface is the newest and it took over a field the director could not
# reach: its rocks retint iterated jce.find_by_prefix("rock") and the entity
# is named "Rocks" — the host's prefix match is a strncmp, so that loop has
# always run zero times and every state's authored rocks_tint went nowhere.
# That loop and the rocks_tint column are gone from gen_director.py; the C
# script reads the value straight out of src/es_palettes.h, which is the file
# gen_director.py itself parses.  Roughness is not a handover at all: nothing
# in this project has ever written it, so nothing in the scene gets wet.
#
# THE LANGUAGE COMES FROM THE PATH, NOT FROM A SETTING.  The runtime asks
# jce_script_vm_language_for_path() per Script component and stands up one VM
# per language (jce_rt_script.c).  `.py` is claimed by the Python backend's
# own register(), `.java` and `.class` by the Java backend's, `.jcecpp` by the
# cpp backend's.
#
# `.jcecpp` WAS `.escpp` UNTIL 2026-08-16, AND THE DIFFERENCE IS WHO OWNS IT.
# The cpp backend used to claim no extension at all, so a Script component
# naming a C++ class resolved to no language and was refused; the only way
# through was for THIS PROJECT to invent a claim and to name its class after
# the whole string stored here, extension included.  It worked, the editor
# flagged the working script amber because its offline catalog could not know
# about a runtime claim, and editor Play could not run it at all.  The engine
# now claims `.jcecpp` itself and resolves the class by stripping the
# directory and the extension, so this row is an ordinary path again and the
# class is spelled the way C++ spells it.
#
# JCE_SCRIPT_LANGUAGE would also "work" and is not an option: it is a
# whole-process override and this scene runs five languages at once.
entities.append(ent("Fireflies", [transform(),
    {"type": "Script", "scriptPath": "scripts/es_fireflies.py"},
    meta("Fireflies")]))
entities.append(ent("Campfire", [transform(),
    {"type": "Script", "scriptPath": "scripts/EsCampfire.java"},
    meta("Campfire")]))
entities.append(ent("FlowerSway", [transform(),
    {"type": "Script", "scriptPath": "scripts/EsFlowerSway.jcecpp"},
    meta("FlowerSway")]))
entities.append(ent("PropSurface", [transform(),
    {"type": "Script", "scriptPath": "scripts/EsPropSurface.jcec"},
    meta("PropSurface")]))

# ------------------------------------------------------------- probe entities
# Transform-only entities the five scripts write their own counters into:
# x = on_start count, y = on_update count, z = entities resolved by name.
# src/es_script_probe.c reads them back and prints the table.  They render
# nothing and cost nothing; parked far below the basin so a stray gizmo or a
# future debug-draw cannot appear in frame.
for _pname in ("EsLuaProbe", "EsPyProbe", "EsJavaProbe", "EsCppProbe",
               "EsCProbe"):
    entities.append(ent(_pname, [transform(p=(0, -1000, 0)), meta(_pname)]))

# Looping soundscape voices (volume 0 authored; the director slews live
# volumes per state via jce.audio_set_volume).
AUDIO_LOOPS = [
    ("AudioFire",     "audio/sounds/fire/fire_burning.mp3"),
    ("AudioCrickets", "audio/sounds/crickets/crickets.mp3"),
    ("AudioBirds",    "audio/sounds/birds/birds_1.mp3"),
    ("AudioRain",     "audio/sounds/rain/rain.mp3"),
    ("AudioWaves",    "audio/sounds/waves/lake_waves.mp3"),
    ("AudioOwlHowl",  "audio/sounds/owl/owl_howling.mp3"),
    ("AudioOwlHoot",  "audio/sounds/owl/owl_hooting.mp3"),
    ("AudioWolf",     "audio/sounds/wolf/wolf_howling.mp3"),
    ("AudioThunderD", "audio/sounds/thunder/distant/thunder_distant.mp3"),
]
for aname, clip in AUDIO_LOOPS:
    entities.append(ent(aname, [transform(),
        {"type": "AudioSource", "clipPath": clip, "volume": 0.0, "loop": True,
         "playOnAwake": True, "spatialBlend": 0.0},
        meta(aname)]))
# Music playlist: 3 looping tracks (reference MusicManager rotates through them
# with a crossfade).  All start silent; the director fades one in at a time and
# rotates every ~110s.  Track 0 also seeds AudioMusic (legacy name kept).
MUSIC_TRACKS = ["forest_dreams", "morning_petals", "window_light"]
for mi, track in enumerate(MUSIC_TRACKS):
    name = "AudioMusic" if mi == 0 else "AudioMusic%d" % mi
    entities.append(ent(name, [transform(),
        {"type": "AudioSource", "clipPath": "audio/musics/%s.mp3" % track,
         "volume": 0.0, "loop": True, "playOnAwake": True, "spatialBlend": 0.0,
         "mixerBus": "Music"},
        meta(name)]))

# HUD: UICanvas buttons wired to the director's global handlers — consumed
# by BOTH hosts (editor Play + runtime dispatch the same onClickHandler).
hud_id = nid()
entities.append(ent("HudCanvas", [transform(),
    {"type": "Canvas", "renderMode": 0, "sortOrder": 0,
     "refResX": 1280.0, "refResY": 720.0},
    meta("HudCanvas")], eid=hud_id))
BTNS = [
    ("BtnSpring",  "Spring",    "es_btn_spring",  -318),
    ("BtnWinter",  "Winter",    "es_btn_winter",  -212),
    ("BtnAutumn",  "Autumn",    "es_btn_autumn",  -106),
    ("BtnRain",    "Rain",      "es_btn_rain",       0),
    ("BtnTod",     "Day-Night", "es_btn_daynight", 106),
    ("BtnStrike",  "Lightning", "es_btn_strike",   212),
    ("BtnMusic",   "Music",     "es_btn_music",    318),
]
for bname, blabel, bhandler, bx in BTNS:
    entities.append(ent(bname, parent=hud_id, comps=[transform(),
        {"type": "UIImage", "colorR": 0.08, "colorG": 0.10, "colorB": 0.12,
         "colorA": 0.72,
         "anchorMinX": 0.5, "anchorMinY": 1.0, "anchorMaxX": 0.5, "anchorMaxY": 1.0,
         "pivotX": 0.5, "pivotY": 1.0, "anchoredX": bx, "anchoredY": -30,
         "sizeW": 96, "sizeH": 34},
        {"type": "UIButton", "onClickHandler": bhandler,
         "normalR": 0.08, "normalG": 0.10, "normalB": 0.12, "normalA": 0.72,
         "highlightR": 0.16, "highlightG": 0.19, "highlightB": 0.22, "highlightA": 0.85,
         "pressedR": 0.04, "pressedG": 0.05, "pressedB": 0.06, "pressedA": 0.9},
        {"type": "UIText", "text": blabel, "fontPath": "fonts/LXGWWenKai-Regular.ttf",
         "fontSize": 16, "alignment": 1,
         "colorR": 0.93, "colorG": 0.93, "colorB": 0.90, "colorA": 1.0,
         "anchorMinX": 0.5, "anchorMinY": 1.0, "anchorMaxX": 0.5, "anchorMaxY": 1.0,
         "pivotX": 0.5, "pivotY": 1.0, "anchoredX": bx, "anchoredY": -36,
         "sizeW": 96, "sizeH": 24},
        meta(bname)]))
# Music "Now Playing" indicator (reference MusicControlUI) — bottom-left; the
# director rewrites its text on each track change.  A Skip button advances.
entities.append(ent("NowPlaying", parent=hud_id, comps=[transform(),
    {"type": "UIText", "text": "♪ Forest Dreams", "fontPath": "fonts/LXGWWenKai-Regular.ttf",
     "fontSize": 15, "alignment": 0,
     "colorR": 0.90, "colorG": 0.90, "colorB": 0.82, "colorA": 0.9,
     "anchorMinX": 0.0, "anchorMinY": 0.0, "anchorMaxX": 0.0, "anchorMaxY": 0.0,
     "pivotX": 0.0, "pivotY": 0.0, "anchoredX": 20, "anchoredY": 20,
     "sizeW": 260, "sizeH": 24},
    meta("NowPlaying")]))
entities.append(ent("BtnSkip", parent=hud_id, comps=[transform(),
    {"type": "UIImage", "colorR": 0.08, "colorG": 0.10, "colorB": 0.12, "colorA": 0.72,
     "anchorMinX": 0.0, "anchorMinY": 0.0, "anchorMaxX": 0.0, "anchorMaxY": 0.0,
     "pivotX": 0.0, "pivotY": 0.0, "anchoredX": 20, "anchoredY": 48, "sizeW": 60, "sizeH": 26},
    {"type": "UIButton", "onClickHandler": "es_btn_skip",
     "normalR": 0.08, "normalG": 0.10, "normalB": 0.12, "normalA": 0.72,
     "highlightR": 0.16, "highlightG": 0.19, "highlightB": 0.22, "highlightA": 0.85,
     "pressedR": 0.04, "pressedG": 0.05, "pressedB": 0.06, "pressedA": 0.9},
    {"type": "UIText", "text": "Skip ⏭", "fontPath": "fonts/LXGWWenKai-Regular.ttf",
     "fontSize": 13, "alignment": 1,
     "colorR": 0.93, "colorG": 0.93, "colorB": 0.90, "colorA": 1.0,
     "anchorMinX": 0.0, "anchorMinY": 0.0, "anchorMaxX": 0.0, "anchorMaxY": 0.0,
     "pivotX": 0.0, "pivotY": 0.0, "anchoredX": 20, "anchoredY": 50, "sizeW": 60, "sizeH": 20},
    meta("BtnSkip")]))
entities.append(ent("IntroTitle", parent=hud_id, comps=[transform(),
    {"type": "UIText", "text": "ELEMENTAL SERENITY", "fontPath": "fonts/LXGWWenKai-Regular.ttf",
     "fontSize": 42, "alignment": 1,
     "colorR": 0.95, "colorG": 0.93, "colorB": 0.88, "colorA": 1.0,
     "anchorMinX": 0.5, "anchorMinY": 0.5, "anchorMaxX": 0.5, "anchorMaxY": 0.5,
     "pivotX": 0.5, "pivotY": 0.5, "anchoredX": 0, "anchoredY": -40,
     "sizeW": 720, "sizeH": 60},
    meta("IntroTitle")]))

scene = {
  "contract": { "name": "jce.scene", "major": 1, "minor": 0 },
  "scene": { "version": 1, "entities": entities, "rendering": rendering }
}

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8") as f:
    json.dump(scene, f, indent=2)
print(f"wrote {OUT}: {len(entities)} entities")
