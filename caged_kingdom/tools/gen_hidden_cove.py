#!/usr/bin/env python3
"""Hidden Cove -- a coastal-bay scene that exercises sky / land / water / light.

COMPOSITION (standard coastal practice, three depth layers):

  foreground   rocky spit + trees on the left, framing the view (repoussoir)
  midground    the bay itself -- a crescent of water between two headlands
  background   a ridge rising behind, so the eye reads distance

LIGHT is the subject. The sun sits low (~34 deg) out past the bay mouth and
back-lights through the gap between the headlands, so:
  - long shadows rake across the beach, which is what makes contact shadows
    and the cascade blend visible at all;
  - the volumetric fog march has a low sun to scatter, which IS the Tyndall
    effect -- god rays are not a separate feature, they are what the
    shadow-aware march produces when a low sun is occluded by terrain.

Values marked GATE are not preferences. Each was found by reading the engine,
and each one, if violated, renders NOTHING while the scene still loads and
still looks correctly authored.
"""

import json
import math
import pathlib
import random

ROOT = pathlib.Path(__file__).resolve().parents[1]          # caged_kingdom/
ASSETS = ROOT / "resources" / "assets"

# --- terrain geometry, from the cooked asset -------------------------------
TERRAIN_PATH = "terrains/hidden_cove.terrain.json"
WORLD = 400.0
MAX_H = 98.3632
SEA01 = 0.152410
SEA_Y = MAX_H * SEA01                                        # 14.99 m

# The terrain's CORNER sits at its entity transform and it extends into +X/+Z
# (jce_terrain.c: vertex (i,j) -> local (i/(W-1)*size_x, h*max_h, j/(H-1)*size_z)).
# It is NOT centred, so the entity is offset by half the world to put the bay
# at the origin. Getting this wrong puts the water inside the hillside.
TERRAIN_ORIGIN = (-WORLD * 0.5, 0.0, -WORLD * 0.5)

_next_id = [1000]


def eid():
    _next_id[0] += 1
    return _next_id[0]


def xform(p=(0, 0, 0), r=(0, 0, 0), s=(1, 1, 1)):
    return {"type": "Transform",
            "posX": p[0], "posY": p[1], "posZ": p[2],
            "rotX": r[0], "rotY": r[1], "rotZ": r[2],
            "scaleX": s[0], "scaleY": s[1], "scaleZ": s[2]}


def ent(name, comps, parent=0):
    return {"id": eid(), "name": name, "parentId": parent,
            "components": comps + [{"type": "EditorMeta", "name": name}]}


# --- the same height field the cooker consumed -----------------------------
# Reproduced so props sit ON the terrain rather than floating above or sinking.
def _h(i, j, seed=7):
    n = (i * 374761393 + j * 668265263 + seed * 144665) & 0xffffffff
    n = (n ^ (n >> 13)) * 1274126177 & 0xffffffff
    return ((n ^ (n >> 16)) & 0xffff) / 65535.0


def fbm(x, z, oct=5, seed=7):
    v, amp, f, norm = 0.0, 1.0, 1.0, 0.0
    for _ in range(oct):
        px, pz = x * f, z * f
        i, j = math.floor(px), math.floor(pz)
        fx, fz = px - i, pz - j
        sx, sz = fx * fx * (3 - 2 * fx), fz * fz * (3 - 2 * fz)
        a, b = _h(i, j, seed), _h(i + 1, j, seed)
        c, d = _h(i, j + 1, seed), _h(i + 1, j + 1, seed)
        top = a + (b - a) * sx
        bot = c + (d - c) * sx
        v += (top + (bot - top) * sz) * amp
        norm += amp
        amp *= 0.5
        f *= 2.0
    return v / norm


def terrain_h(wx, wz):
    """Ground height ABOVE SEA LEVEL, in metres -- the heightmap generator's
    own space, where the waterline is exactly 0."""
    d_bay = math.hypot(wx - 0.0, wz - (-60.0))
    land = (d_bay - 105.0) * 0.16
    for hx, hz, hr, hh in ((-115.0, -95.0, 62.0, 34.0), (118.0, -88.0, 58.0, 29.0)):
        d = math.hypot(wx - hx, wz - hz)
        if d < hr:
            t = 1.0 - d / hr
            land += hh * t * t * (3 - 2 * t)
    ridge_t = max(0.0, (wz - 40.0) / 160.0)
    land += 46.0 * ridge_t * ridge_t
    land += (fbm(wx * 0.012, wz * 0.012, 5) - 0.5) * 11.0
    land += (fbm(wx * 0.05, wz * 0.05, 3, 19) - 0.5) * 2.6
    return land


def terrain_y(wx, wz):
    """WORLD Y of the ground at (wx,wz).

    The two are NOT the same, and conflating them is what put every prop 15 m
    underground.  The cooker normalised the field as (raw - BASE) / MAX_H and
    the renderer reconstructs vertex Y as normalised * MAX_H, so

        world_y = raw - BASE = raw + SEA_Y

    because BASE is the lowest sea-floor point and the waterline sits at raw 0.
    The water plane was right by accident -- it was authored at SEA_Y directly
    -- which is why the sea looked correct while everything on land sank."""
    return terrain_h(wx, wz) + SEA_Y



# ── Impostor terminal LOD ───────────────────────────────────────────────
#
# Thin opaque leaf geometry is sub-pixel at distance, and sub-pixel geometry
# shimmers under camera motion. Measured on a rotating camera, every tree in
# frame was a bright blob in the motion-compensated residual -- the largest
# flicker source left in the scene once the ground was fixed.
#
# It is NOT an alpha-test problem (these meshes are alphaMode 0, opaque) and
# MSAA cannot reach it: the editor viewport renders into an offscreen target
# that carries no MSAA flag, and its depth is sampled as a texture by SSAO, fog
# and underwater, so it cannot become multisampled without a resolve pass. TAA
# is the pipeline's antialiasing and measurably tops out around 10% on this
# content.
#
# So the answer is geometric: past `impostor_distance` the mesh is replaced by
# a single camera-facing card sampling a pre-baked octahedral atlas. The
# sub-pixel triangles stop existing, so their shimmer stops existing.
#
# Atlases are baked by caged_kingdom/tools/bake_impostors.sh (headless, via the
# editor's JCE_IMPOSTOR_BAKE hook).
IMPOSTORS = {
    "models/nature/CommonTree_1.gltf": "impostors/commontree_1.impostor.json",
    "models/nature/CommonTree_2.gltf": "impostors/commontree_2.impostor.json",
    "models/nature/CommonTree_3.gltf": "impostors/commontree_3.impostor.json",
    "models/nature/CommonTree_4.gltf": "impostors/commontree_4.impostor.json",
    "models/nature/CommonTree_5.gltf": "impostors/commontree_5.impostor.json",
    "models/nature/Pine_1.gltf":       "impostors/pine_1.impostor.json",
    "models/nature/Pine_5.gltf":       "impostors/pine_5.impostor.json",
}

# Distance at which the card replaces the mesh. Chosen so a tree is already
# small on screen when it switches -- close enough to remove the shimmer, far
# enough that the silhouette change is not the thing you notice instead.
IMPOSTOR_DISTANCE = 70.0


def lod_group_for(mesh_path):
    """A terminal-impostor LODGroup, or None for meshes with no baked atlas."""
    meta = IMPOSTORS.get(mesh_path)
    if not meta:
        return None
    return {"type": "LODGroup",
            "levelCount": 1,
            "meshPath0": mesh_path,
            "distance0": IMPOSTOR_DISTANCE,
            "hysteresis": 0.08,
            "fadeWidth": 12.0,          # cross-fade so the swap is not a pop
            "cullWhenTooFar": False,
            "impostorMetaPath": meta,
            "impostorDistance": IMPOSTOR_DISTANCE}


entities = []

# -- land -------------------------------------------------------------------
entities.append(ent("Cove_Terrain", [
    xform(TERRAIN_ORIGIN),
    {"type": "Terrain",
     "terrainPath": TERRAIN_PATH,
     "layerAlbedoPath0": "textures/cove_ground.png",
     "layerAlbedoPath1": "",
     "layerAlbedoPath2": "", "layerAlbedoPath3": "",
     "tileScale": 16.0,   # measured: see gen_cove_ground.py
     "tintR": 1.0, "tintG": 1.0, "tintB": 1.0,
     "visible": True, "splatEnabled": False},
]))

# -- water: the bay ---------------------------------------------------------
# GATE: clarity > 0 is the MASTER SWITCH for caustics AND shoreFoam -- both sit
#       inside the Beer-Lambert block in fs_water.sc. clarity 0 with
#       shoreFoam 2.2 silently produces neither.
# GATE: caustics require waterMode 1 (FFT); they read the FFT displacement
#       texture, which only that branch creates.
# GATE: fftPatchSize must be non-zero or the surface is a flat bobbing slab.
entities.append(ent("Bay_Water", [
    xform((0.0, SEA_Y, -40.0)),
    {"type": "Water",
     "waterMode": 1,
     "sizeX": 300.0, "sizeZ": 300.0,
     "baseHeight": 0.0,
     "ocean": False,
     "visible": True, "depthWrite": False,

     "fftResolution": 128, "fftPatchSize": 96.0,
     "fftWindSpeed": 7.5, "fftWindDirX": 0.2, "fftWindDirZ": 1.0,
     "fftAmplitude": 0.00045,
     "fftFetch": 12000.0,
     "fftSwell": 0.25,

     "shallowR": 0.30, "shallowG": 0.62, "shallowB": 0.62,
     "deepR": 0.02, "deepG": 0.11, "deepB": 0.19,
     "transparency": 0.72, "sunSpecular": 26.0,

     "clarity": 9.0,
     "caustics": 0.65,
     "shoreFoam": 2.2,
     "shoreSurge": 7.0,
     "shoreRipple": 0.0, "iceRatio": 0.0, "splashRatio": 0.0,
     "waveCount": 0},
]))

# -- light ------------------------------------------------------------------
#
# Sun elevation, measured rather than chosen for the sound of it.
#
# 13.5 degrees was picked for a golden-hour look and it made the scene FLAT.
# At that elevation, on this terrain, a probe that separates back-facing ground
# from genuinely occluded ground measured:
#
#     elevation   back-facing   occluded   penumbra   lit & clear
#      13.5 deg      64.2%        1.6%       5.8%        28.4%
#      40   deg      21.3%        4.7%      11.7%        62.4%
#
# Two thirds of the visible ground faced away from the sun, so it had no direct
# light for a shadow to remove -- and the result reads as "the shadows are
# broken" when the shadows are working exactly as asked. Raising the sun both
# lights the terrain and triples the amount of real cast shadow.
#
# 34 degrees keeps a low, warm, side-lit character (the point of the original
# choice) while leaving most of the ground actually facing the sun.
ELEV = math.radians(34.0)
AZI = math.radians(8.0)
SUN_DIR = (math.sin(AZI) * math.cos(ELEV),
           -math.sin(ELEV),
           math.cos(ELEV) * math.cos(AZI))
entities.append(ent("Sun", [
    xform((0.0, 90.0, -150.0)),
    {"type": "Light", "lightType": 0,
     "colorR": 1.0, "colorG": 0.83, "colorB": 0.62,
     "intensity": 4.6,
     "dirX": SUN_DIR[0], "dirY": SUN_DIR[1], "dirZ": SUN_DIR[2],
     "castsShadow": True},
]))
entities.append(ent("Sky_Fill", [
    xform((0.0, 70.0, 60.0)),
    {"type": "Light", "lightType": 0,
     "colorR": 0.42, "colorG": 0.55, "colorB": 0.78,
     "intensity": 0.55,
     "dirX": -0.25, "dirY": -0.93, "dirZ": -0.26,
     "castsShadow": False},
]))

# -- forest -----------------------------------------------------------------
rng = random.Random(20260805)


def scatter(name, pool, cx, cz, radius, count, y_lo, y_hi,
            scale_rng, max_slope, y_bias):
    placed, tries = 0, 0
    while placed < count and tries < count * 60:
        tries += 1
        a = rng.random() * math.tau
        r = radius * math.sqrt(rng.random())
        wx, wz = cx + math.cos(a) * r, cz + math.sin(a) * r
        if abs(wx) > 185.0 or abs(wz) > 185.0:
            continue
        h = terrain_h(wx, wz)          # metres above sea level: the filter
        if h < y_lo or h > y_hi:
            continue
        y = h + SEA_Y                  # world Y: the placement
        d = 2.0
        gx = (terrain_h(wx + d, wz) - terrain_h(wx - d, wz)) / (2 * d)
        gz = (terrain_h(wx, wz + d) - terrain_h(wx, wz - d)) / (2 * d)
        if math.hypot(gx, gz) > max_slope:
            continue
        s = rng.uniform(*scale_rng)
        mesh = rng.choice(pool)
        comps = [
            xform((wx, y + y_bias, wz),
                  (0.0, rng.random() * math.tau, 0.0),
                  (s, s * rng.uniform(0.9, 1.15), s)),
            {"type": "MeshRenderer", "meshPath": mesh,
             "baseColorR": 1.0, "baseColorG": 1.0, "baseColorB": 1.0,
             "baseColorA": 1.0,
             "metallic": 0.0, "roughness": 0.9,
             "normalScale": 1.0, "aoStrength": 1.0,
             "alphaMode": 0, "alphaCutoff": 0.5, "doubleSided": False,
             "castsShadow": True, "receivesShadow": True},
        ]
        # Impostor LODs are DISABLED. They were authored to fix sub-pixel tree
        # shimmer, were never measured to help, and are the obvious suspect for
        # "distant models have the wrong size/position/rotation" -- a card with
        # a mis-computed anchor or radius looks exactly like that. The atlases
        # and bake script stay; re-enable only with a before/after capture.
        # lod = lod_group_for(mesh)
        # if lod:
        #     comps.append(lod)
        entities.append(ent("%s_%d" % (name, placed), comps))
        placed += 1
    return placed


# Quaternius-style Ultimate Nature kit, relocated into the asset tree from
# caged_kingdom/halloween-test/1/model/glTF. Each model carries its OWN
# materials (Bark_* + Leaves_*), so albedoTex is left empty -- overriding it
# with a single texture would paint the bark texture onto the leaves.
#
# Measured heights, which is why the scales below are what they are:
#   CommonTree 7.3   Pine 7.3   TwistedTree 16.7   Bush 1.6
#   Fern 2.7 (9.0 wide)   Rock_Medium 2.3   Grass_Common_Tall 1.9
NAT = "models/nature/"

CONIFER  = [NAT + "Pine_%d.gltf" % i for i in range(1, 6)]
BROADLEAF = [NAT + "CommonTree_%d.gltf" % i for i in range(1, 6)]
TWISTED  = [NAT + "TwistedTree_%d.gltf" % i for i in range(1, 6)]
DEAD     = [NAT + "DeadTree_%d.gltf" % i for i in range(1, 6)]
BUSH     = [NAT + "Bush_Common.gltf", NAT + "Bush_Common_Flowers.gltf"]
UNDER    = [NAT + "Fern_1.gltf", NAT + "Plant_1.gltf", NAT + "Plant_1_Big.gltf",
            NAT + "Plant_7.gltf", NAT + "Plant_7_Big.gltf"]
SHOREGRASS = [NAT + "Grass_Common_Tall.gltf", NAT + "Grass_Wispy_Tall.gltf",
              NAT + "Grass_Common_Short.gltf"]
ROCKS    = [NAT + "Rock_Medium_%d.gltf" % i for i in range(1, 4)]
PEBBLES  = [NAT + "Pebble_Round_%d.gltf" % i for i in range(1, 6)] +            [NAT + "Pebble_Square_%d.gltf" % i for i in range(1, 7)]
FLOWERS  = [NAT + "Flower_3_Group.gltf", NAT + "Flower_4_Group.gltf",
            NAT + "Clover_1.gltf", NAT + "Clover_2.gltf"]

# name, mesh pool, centre, radius, count, y range, scale range, max slope
ZONES = [
    # Windward headlands: conifers, which is what grows on a sea-facing slope,
    # plus wind-bent twisted trees on the crest.
    ("Pine_West",   CONIFER,   -122.0, -72.0, 74.0,  95, 4.0, 44.0, (0.85, 1.45), 0.70),
    ("Pine_East",   CONIFER,    118.0, -70.0, 66.0,  78, 4.0, 42.0, (0.85, 1.40), 0.70),
    ("Twisted_Crest", TWISTED, -118.0, -88.0, 46.0,  14, 16.0, 42.0, (0.45, 0.75), 0.55),
    ("Twisted_East",  TWISTED,  120.0, -84.0, 40.0,  10, 14.0, 40.0, (0.45, 0.72), 0.55),
    # Sheltered inland slope: broadleaf.
    ("Oak_Ridge",   BROADLEAF,   0.0, 118.0, 155.0, 140, 6.0, 62.0, (0.95, 1.60), 0.74),
    ("Oak_Inland",  BROADLEAF, -40.0,  40.0, 105.0,  95, 5.0, 36.0, (0.90, 1.45), 0.68),
    ("Oak_East",    BROADLEAF,  70.0,  60.0,  90.0,  70, 5.0, 40.0, (0.90, 1.45), 0.68),
    # Dead trees at the treeline, for silhouette against the low sun.
    ("Snag",        DEAD,        0.0,  60.0, 160.0,  26, 8.0, 54.0, (0.70, 1.20), 0.78),
    # Undergrowth: what makes a forest read as a forest rather than as trees
    # standing on a lawn.
    ("Bush",        BUSH,        0.0,  20.0, 172.0, 260, 2.5, 50.0, (0.9, 1.8), 0.80),
    ("Under",       UNDER,       0.0,  30.0, 168.0, 200, 3.0, 46.0, (0.35, 0.7), 0.80),
    ("Flowers",     FLOWERS,   -20.0,  30.0, 140.0, 150, 3.0, 34.0, (0.6, 1.1), 0.62),
    # Shore: rocks at the waterline where the foam band reads, pebbles just
    # above it, marram grass on the dune line.
    ("Rock_Shore",  ROCKS,       0.0, -60.0, 126.0, 130, -2.5, 3.6, (0.7, 2.0), 3.0),
    ("Pebble",      PEBBLES,     0.0, -60.0, 120.0, 220, -0.8, 2.8, (0.6, 1.4), 3.0),
    ("Marram",      SHOREGRASS,  0.0, -60.0, 138.0, 300, 0.5, 9.0, (0.8, 1.5), 1.4),
]

counts = {}
for (nm, pool, cx, cz, rad, cnt, ylo, yhi, srng, slope) in ZONES:
    counts[nm] = scatter(nm, pool, cx, cz, rad, cnt, ylo, yhi, srng, slope, 0.18)

# -- grass ------------------------------------------------------------------
# GATE: GrassField is OFF PROJECT-WIDE unless resources/assets/render_settings.json
#       carries "grassEnabled": 1 (jce_render_settings.c: s.grass_enabled = 0).
#       Written below for exactly that reason.
for nm, gx, gz in (("Grass_West", -120.0, -40.0), ("Grass_East", 118.0, -40.0)):
    entities.append(ent(nm, [
        xform((gx, terrain_y(gx, gz), gz)),
        {"type": "GrassField",
         "density": 6.0, "seed": 4242, "areaX": 80.0, "areaZ": 80.0,
         "maxSlopeDeg": 34.0, "scaleMin": 0.8, "scaleMax": 1.35,
         "bladeHeight": 0.42, "bladeWidth": 0.06, "cards": 4,
         "rootR": 0.16, "rootG": 0.26, "rootB": 0.10,
         "tipR": 0.46, "tipG": 0.68, "tipB": 0.30,
         "windDirX": 0.2, "windDirZ": 1.0,
         "windSpeed": 1.6, "windAmplitude": 0.22,
         "fadeStart": 45.0, "fadeEnd": 110.0, "hueJitter": 0.09,
         "castShadow": False, "visible": True},
    ]))

# -- player -----------------------------------------------------------------
# Dropped on the beach at the west end, facing along the bay. The controller
# needs the terrain collider under it, which the runtime builds from the
# Terrain component -- no separate collider entity.
PLAYER_X, PLAYER_Z = -78.0, 12.0
PLAYER_Y = terrain_y(PLAYER_X, PLAYER_Z) + 0.4
PLAYER_ID = 100
entities.append({
    "id": PLAYER_ID, "name": "Player", "parentId": 0,
    "components": [
        xform((PLAYER_X, PLAYER_Y, PLAYER_Z), (0.0, 150.0, 0.0)),
        {"type": "MeshRenderer", "meshPath": "models/UAL1_Standard.glb",
         "materialPath": "", "meshShape": 0,
         "baseColorR": 1.0, "baseColorG": 1.0, "baseColorB": 1.0,
         "baseColorA": 1.0, "metallic": 0.0, "roughness": 0.9,
         "castsShadow": True, "receivesShadow": True},
        {"type": "SkeletalAnimator",
         "skeletonPath": "models/UAL1_Standard.glb",
         "stateMachine": "anim/mannequin_locomotion.anim_sm.json",
         "autoSpeed": True, "useBlendTree": False, "blendParam": 0,
         "blendThresholds": [0.0, 2.0, 4.0, 6.0, 0, 0, 0, 0],
         "speed": 1, "loop": True, "playing": True, "activeClip": -1,
         "clipNames": ["Idle_Loop", "Walk_Loop", "Jog_Fwd_Loop", "Sprint_Loop"]},
        {"type": "CharacterController",
         "height": 1.8, "radius": 0.35, "stepOffset": 0.4,
         "slopeLimit": 48.0, "moveSpeed": 3.2, "sprintMult": 2.6,
         "jumpSpeed": 5.2, "accel": 40.0, "airControl": 0.35,
         "turnSpeed": 720.0},
        {"type": "EditorMeta", "name": "Player"},
    ]})
entities.append({
    "id": PLAYER_ID + 1, "name": "PlayerCamera", "parentId": PLAYER_ID,
    "components": [
        xform((0.0, 1.65, 0.0)),
        {"type": "Camera", "fov": 62.0, "nearClip": 0.1, "farClip": 1400.0,
         "primary": True, "orthographic": False},
        {"type": "EditorMeta", "name": "PlayerCamera"},
    ]})

# -- beauty camera ----------------------------------------------------------
entities.append(ent("BeautyCam", [
    xform((-96.0, 26.0, 96.0), (-0.20, 2.62, 0.0)),
    {"type": "Camera", "fov": 52.0, "nearPlane": 0.3, "farPlane": 1400.0,
     "orthographic": False, "priority": 0, "active": False},
]))

# -- sky / light settings ---------------------------------------------------
rendering = {
    "version": 1,
    # Modest ambient: the sky derivation tints it, the authored value sets its
    # magnitude, and this scene is lit by the sun -- not by fill.
    "ambient": {"color": [0.20, 0.26, 0.34], "intensity": 0.85},

    # GOD RAYS. The volumetric march is shadow-aware, so shafts appear wherever
    # the terrain occludes the low sun. Height fog pools it in the bay bowl.
    "fog": {"enabled": True, "mode": 2,
            "color": [0.70, 0.76, 0.84],
            # Density and falloff are a PAIR, and the falloff is what makes fog
            # feel like weather rather than like a post effect.
            #
            # 0.16 is an e-folding distance of 6 METRES: raise the camera ten
            # metres and the fog is simply gone, which reads as a bug because
            # nothing in the world changed.  0.018 e-folds over ~55 m, so the
            # bay bowl fills and the fog thins with height the way air does.
            #
            # Density is picked from a VISIBILITY target, not by eye.  Mode 2 is
            # exponential, so the fraction of a surface at distance d that is
            # fog is 1 - exp(-density*d).  The far shore of the bay is ~250 m
            # away and should read as atmospheric depth -- clearly visible,
            # tinted -- so 28% there gives density = -ln(0.72)/250 = 0.0013.
            #
            # The 0.0075 this replaces was an e-folding distance of 133 m: 53%
            # fog at ONE HUNDRED METRES, in a 400 m world.  That is a fog bank,
            # not air, and no amount of colour tuning rescues it -- the mistake
            # was the number, and picking it by eye is how it got there.
            "density": 0.0013,
            "heightFalloff": 0.018,
            # Sit the layer a little BELOW the waterline so the beach is inside
            # it -- anchoring at exactly sea level puts the densest fog on the
            # water surface and leaves the sand crisp, which is backwards.
            "heightOrigin": SEA_Y - 6.0,
            # Shorter than the view distance on purpose: past this the march
            # has saturated, and every extra metre only deepens the wall the
            # horizon already is.
            "start": 12.0, "end": 260.0},

    "shadows": {"distance": 260.0, "cascades": 4,
                "splitLambda": 0.3,   # NOT 0.72 -- see below
        # splitLambda weights the LOGARITHMIC term of the cascade partition,
        # and the engine fits that partition to [JCE_CSM_SHADOW_NEAR, far]
        # with a near of 0.1 m.  At 0.72 the split boundaries collapse toward
        # the camera: measured at the `slope` framing, 37/78/153/520, so
        # cascades 0-2 covered empty air -- the camera stands 95 m off and no
        # receiver is closer than ~100 m -- and every shadow in the frame came
        # from cascade 3 alone, at its coarsest.  Turning shadows off there
        # changed the picture by 0.001% of pixels: there were effectively none.
        #
        # Measured sweep (% of pixels shadowed by >24 levels, shadows on vs off):
        #     lambda   forest (close)   slope (standoff)
        #       0.72       0.593%            0.001%
        #       0.50       1.286%            0.252%
        #       0.30       1.351%            0.641%
        #       0.10       1.374%            0.768%
        # 0.72 is worse at BOTH ends, so there was no close-up cost to trade
        # away -- the expected trade-off simply is not there, because even the
        # close framing stands 70 m off.  0.10 measured best on both; 0.30 is
        # taken because it captures ~93% of the gain and no first-person
        # framing was measured, which is exactly where a high lambda pays.
        # Shadow-mask coherence was checked so this is not acne: 4-neighbour
        # agreement held at 95.6% (forest) and rose 89.3% -> 94.5% (slope).
                "resolution": 4096, "soft": 1},

    # A COHERENT HDR configuration. An HDR target must be tone mapped or
    # everything above 1.0 clips to white -- that combination is the
    # over-exposure this project already hit in street_demo and graveyard.
    "postfx": {"tonemap": True, "exposure": 0.85, "gamma": 2.2,
               "bloom": True, "bloomThreshold": 1.15, "bloomIntensity": 0.35,
               "fxaa": False, "vignette": True, "vignetteIntensity": 0.22,
               "vignetteSmoothness": 0.6, "chromatic": False,
               "grayscale": False},

    "environment": {
        "timeOfDay": {"enabled": False, "hour": 7.4, "speed": 1.0,
                      "latitude": 35.0, "dawnHour": 6.0, "duskHour": 18.0},
        "weather": {"type": 0, "intensity": 0.0},
        # The direction this cove's weather blows, at the level that now has an
        # authority for it. NOT a new content decision: the grass field and the
        # ocean both already say (0.2, 1.0) -- they always have -- and the
        # environment said (1, 0, 0) because that is what its struct default
        # left there and nothing ever wrote it. Three winds in one scene, and
        # the one nobody authored was the one the weather overlay and the cloud
        # drift were reading.
        "wind": {"directionX": 0.2, "directionZ": 1.0},
        # GATE: cloud keys do NOTHING unless mode == 4 (PHYSICAL), and coverage
        #       <= 0.54 renders no cloud at any density -- the noise field's
        #       global maximum is 0.4611 and the shader carves d-(1-coverage).
        "sky": {"mode": 4, "turbidity": 2.6,
                "cloudCoverage": 0.82,
                "cloudDensity": 1.15,
                "cloudBottomKm": 1.4,
                "cloudTopKm": 4.2},
        "floatingOrigin": {"enabled": False, "threshold": 4096.0},
    },

    # GATE: clarity / caustics / shoreFoam need the camera depth prepass.  That
    #       used to mean "only when SSAO or SSR is enabled", which is why these
    #       two flags are on here; it no longer does -- the renderer now
    #       requests the prepass itself for any visible water with clarity > 0,
    #       on every tier (see sr_water_needs_depth).  Left ON regardless,
    #       because SSAO also carries the contact-shadow (green) and
    #       cloud-shadow (blue) channels and every measurement archived against
    #       this scene was taken with both enabled: turning them off now would
    #       change the picture for reasons that have nothing to do with water.
    "ssaoEnabled": True,
    "ssaoRadius": 0.9,
    "ssaoIntensity": 1.1,
    "ssrEnabled": True,
    "iblEnabled": True,
}

scene = {"contract": {"name": "jce.scene", "major": 1, "minor": 0},
         "scene": {"version": 1,
                   "entities": entities,
                   "rendering": rendering,
                   "streaming": {"version": 1, "enabled": False, "mode": 0,
                                 "loadRadius": 150.0, "unloadRadius": 200.0,
                                 "maxPending": 4, "budgetMb": 256,
                                 "frameBudgetMs": 2.0, "chunks": []}}}

out = ASSETS / "scenes" / "hidden_cove.scene.json"
out.write_text(json.dumps(scene, indent=1, ensure_ascii=False) + "\n",
               encoding="utf-8", newline="\n")


# ── Test maps (plan section 52) ───────────────────────────────────────
#
# Variants of the scene above, differing in ONE environment field each, so a
# measurement can attribute what it sees. They are generated rather than
# hand-patched because a hand-patched variant goes stale the moment this
# script is re-run and says nothing about it -- which is how a measurement ends
# up describing a scene that no longer exists.
#
# Each exists because something could not otherwise be measured:
#
#   rain      -- weather at full intensity. Clear weather derives a fog
#                extinction below the authored one, so the max() in the
#                volumetric fog keeps the authored value and the whole
#                weather-to-fog path is invisible without this.
#   night     -- time of day frozen at midnight (speed 0). A day/night cycle
#                is right for WATCHING the sky and wrong for measuring it: the
#                frame a capture lands on then depends on startup timing, and
#                the first night measurement attempted this way fired after
#                dawn. A stopped clock is what makes the number reproducible.
#   daynight  -- the cycle itself, 4 h/s, a full day in six seconds. For
#                observing transitions, not for asserting on.
#
# Only `enabled`/`hour`/`speed`/`type`/`intensity` differ; everything else is
# byte-identical to the scene above, which is what lets a diff mean something.

def _variant(name, mutate):
    import copy
    v = copy.deepcopy(scene)
    mutate(v["scene"]["rendering"]["environment"])
    q = ASSETS / "scenes" / ("hidden_cove_%s.scene.json" % name)
    q.write_text(json.dumps(v, indent=1, ensure_ascii=False) + "\n",
                 encoding="utf-8", newline="\n")
    return q


def _rain(env):
    env["weather"] = {"type": 1, "intensity": 1.0}


def _snow(env):
    # Snow needs BOTH: falling snow and air cold enough for it to survive
    # landing. The environment melts snow_amount to zero on every frame above
    # 1 C, and nothing wrote temperature_c until 2026-08-15 -- so every scene
    # sat at the 15 C default and lying snow was unreachable by construction.
    # -5 C is a plain winter's day, well clear of the threshold, so this map
    # tests accumulation and not the edge of it.
    env["weather"] = {"type": 2, "intensity": 1.0}
    env["temperatureC"] = -5.0


def _night(env):
    env["timeOfDay"] = {"enabled": True, "hour": 0.0, "speed": 0.0,
                        "latitude": 35.0, "dawnHour": 6.0, "duskHour": 18.0}


def _daynight(env):
    env["timeOfDay"] = {"enabled": True, "hour": 0.0, "speed": 4.0,
                        "latitude": 35.0, "dawnHour": 6.0, "duskHour": 18.0}


_variants = [_variant("rain", _rain), _variant("snow", _snow),
             _variant("night", _night), _variant("daynight", _daynight)]

rs = ASSETS / "render_settings.json"
cur = {}
if rs.exists():
    try:
        cur = json.loads(rs.read_text(encoding="utf-8"))
    except Exception:
        cur = {}
cur["grassEnabled"] = 1
rs.write_text(json.dumps(cur, indent=1) + "\n", encoding="utf-8", newline="\n")

print("scene   : %s  (%d entities)" % (out, len(entities)))
for _v in _variants:
    print("test map: %s" % _v.name)
for k in sorted(counts):
    print("  %-14s %d" % (k, counts[k]))
print("  water : plane at y=%.2f, 300x300" % SEA_Y)
print("  sun   : elev %.1fdeg dir=(%.3f,%.3f,%.3f)"
      % (math.degrees(ELEV), SUN_DIR[0], SUN_DIR[1], SUN_DIR[2]))
print("grass gate: %s grassEnabled=1" % rs)
