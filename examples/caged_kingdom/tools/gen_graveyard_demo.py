#!/usr/bin/env python3
"""One-shot generator for the Hollow's End graveyard demo scene.

Places individual Halloween props with auto ground-seating (from each mesh's
measured pivot) and auto BoxColliders (sized from measured bounds), so collision
is correct per-prop instead of one blob around a pack. Re-run to regenerate;
the editor will re-normalise on save.
"""
import json, math, pathlib, warnings, logging
warnings.filterwarnings("ignore"); logging.getLogger("trimesh").setLevel(logging.ERROR)
import trimesh

ROOT = pathlib.Path(__file__).resolve().parents[1] / "resources/assets"
OUT = ROOT / "scenes" / "graveyard.scene.json"
HALL_ALB = "textures/halloween_albedo.png"
HALL_EMIT = "textures/halloween_emit.png"
WHITE = "textures/white.png"

_bcache = {}
def bounds(mesh_rel):
    if mesh_rel not in _bcache:
        s = trimesh.load(str(ROOT / mesh_rel), force="scene")
        _bcache[mesh_rel] = (list(map(float, s.bounds[0])), list(map(float, s.bounds[1])))
    return _bcache[mesh_rel]

ents = []
_id = [0]
def nid():
    _id[0] += 1
    return _id[0]

def T(pos, rotY=0.0, scale=(1, 1, 1)):
    return {"type": "Transform", "posX": pos[0], "posY": pos[1], "posZ": pos[2],
            "rotX": 0, "rotY": rotY, "rotZ": 0,
            "scaleX": scale[0], "scaleY": scale[1], "scaleZ": scale[2]}

def MR(mesh="", albedo=WHITE, shape=0, color=(1, 1, 1, 1), metallic=0.0, rough=0.9,
       emissive=(0, 0, 0), emit_tex=None):
    color = tuple(color) + (1.0,) * (4 - len(color))
    d = {"type": "MeshRenderer", "meshPath": mesh, "materialPath": "", "albedoTex": albedo,
         "meshShape": shape, "baseColorR": color[0], "baseColorG": color[1],
         "baseColorB": color[2], "baseColorA": color[3], "metallic": metallic,
         "roughness": rough, "emissiveR": emissive[0], "emissiveG": emissive[1],
         "emissiveB": emissive[2]}
    if emit_tex:
        d["emissiveTex"] = emit_tex
    return d

def RB(mass=0.0, grav=False):
    # Higher damping + friction so a shoved crate stops soon instead of sliding
    # far (the dynamic-capsule player drives velocity, so it pushes firmly).
    return {"type": "Rigidbody", "mass": mass, "drag": 0.6, "angularDrag": 0.6,
            "useGravity": grav, "isKinematic": False, "friction": 1.0}

def BOX(center, size, trigger=False):
    return {"type": "BoxCollider", "centerX": center[0], "centerY": center[1],
            "centerZ": center[2], "sizeX": size[0], "sizeY": size[1], "sizeZ": size[2],
            "isTrigger": trigger}

# Absolute model root: the CompoundCollider runtime path does NOT use the editor
# asset resolver, so it needs an absolute (or cwd-relative) path. Cooked .jcol
# blobs sit beside each model so the runtime loads the exact trimesh instead of
# live V-HACD-cooking on every scene load.
ABS_ROOT = ROOT.as_posix()
def COMPOUND(mesh_rel):
    # mode 0 = AUTO (static -> exact triangle mesh); hugs irregular geometry.
    return {"type": "CompoundCollider", "modelPath": f"{ABS_ROOT}/{mesh_rel}",
            "mode": 0, "split": 0, "isStatic": True, "isTrigger": False,
            "detectNaming": False, "friction": 0.7, "restitution": 0.0}

def add(name, comps, parent=0):
    eid = nid()
    ents.append({"name": name, "id": eid, "parent_id": parent, "components": comps})
    return eid

def prop(name, mesh, pos_xz, rotY=0.0, scale=1.0, collider=True, color=(1, 1, 1, 1),
         emissive=(0, 0, 0), emit_tex=None, trunk=None, rough=0.85):
    mn, mx = bounds(mesh)
    posY = -mn[1] * scale
    comps = [T((pos_xz[0], posY, pos_xz[1]), rotY, (scale, scale, scale)),
             MR(mesh=mesh, albedo=HALL_ALB, color=color, rough=rough,
                emissive=emissive, emit_tex=emit_tex)]
    if collider:
        # Exact trimesh that hugs the irregular mesh (cooked .jcol blob), instead
        # of a loose AABB box. The cooked collider is transformed by the entity
        # TRS at spawn, so it tracks the placed/scaled prop. (trunk arg unused now.)
        comps.append(COMPOUND(mesh))
    return add(name, comps)

def prim(name, pos, scale, shape, color, rough=0.9, collider=False, dynamic=False,
         mass=0.0, emissive=(0, 0, 0)):
    comps = [T(pos, 0.0, scale), MR(albedo=WHITE, shape=shape, color=color, rough=rough,
                                    emissive=emissive)]
    if collider:
        # A plane (shape 2) is flat: a unit box collider would be 1*scaleY tall and
        # stick up half a unit above the surface (objects then rest in mid-air). Use
        # a thin slab whose TOP is flush with the plane at local y=0. Cubes/cylinders
        # keep the unit box (it matches the rendered primitive).
        if shape == 2:
            # Thick slab: TOP flush at local y=0 but extends ~2 units DOWN, so a
            # kinematic character (with downward velocity at spawn) can't tunnel
            # straight through a thin floor and sink below the world.
            comps += [RB(mass=mass, grav=dynamic), BOX((0, -1.0, 0), (1, 2.0, 1))]
        else:
            comps += [RB(mass=mass, grav=dynamic), BOX((0, 0, 0), (1, 1, 1))]
    return add(name, comps)

def point_light(name, pos, color, intensity, radius):
    add(name, [T(pos), {"type": "Light", "lightType": 1, "colorR": color[0],
                        "colorG": color[1], "colorB": color[2], "intensity": intensity,
                        "radius": radius, "castsShadow": False}])

H = lambda n: f"models/halloween/{n}.obj"

# ── lighting ────────────────────────────────────────────────────────────
add("Moonlight", [T((0, 24, 0)), {"type": "Light", "lightType": 0,
    "dirX": -0.32, "dirY": -0.86, "dirZ": -0.40, "colorR": 0.55, "colorG": 0.66,
    "colorB": 0.98, "intensity": 2.1, "castsShadow": True}])

# ── ground + perimeter walls (south gap for the gate) ───────────────────
prim("Ground", (0, 0, 0), (150, 1, 150), 2, (0.13, 0.16, 0.12), rough=0.97, collider=True)
prim("Wall_North", (0, 1.5, -29), (60, 3, 1), 0, (0.26, 0.26, 0.30), collider=True)
prim("Wall_East", (29, 1.5, 0), (1, 3, 60), 0, (0.26, 0.26, 0.30), collider=True)
prim("Wall_West", (-29, 1.5, 0), (1, 3, 60), 0, (0.26, 0.26, 0.30), collider=True)
prim("Wall_South_L", (-16, 1.5, 29), (26, 3, 1), 0, (0.26, 0.26, 0.30), collider=True)
prim("Wall_South_R", (16, 1.5, 29), (26, 3, 1), 0, (0.26, 0.26, 0.30), collider=True)

# ── backdrop packs beyond the walls (no collider; unreachable) ──────────
add("BackdropForest", [T((0, 0.32, -45)), MR(mesh="models/TreesPack.glb",
    albedo="textures/trees_palette.png", rough=0.95)])
add("BackdropRocks", [T((46, 0.88, 6), 0.6), MR(mesh="models/RocksPack.glb",
    albedo="textures/rocks_palette.png", rough=0.95)])

# ── grave fields (2 cols x 5 rows per side) ─────────────────────────────
GRAVES = ["SM_Gravestone_Old_Cross", "SM_Gravestone_Old_Classic_Cross",
          "SM_Gravestone_Old_Small_A", "SM_Gravestone_Old_Small_B",
          "SM_Gravestone_Old_Small_C", "SM_Gravestone_Old_Tower",
          "SM_Gravestone_Old_Classic_Broken"]
gi = 0
for side in (-1, 1):
    for xx in (11, 20):
        for ri, zz in enumerate((-22, -13, -4, 6, 15)):
            g = GRAVES[gi % len(GRAVES)]
            gi += 1
            rot = 0.25 * (((gi * 7) % 5) - 2)
            prop(f"Grave_{gi:02d}", H(g), (side * xx + 0.4 * ((gi % 3) - 1), zz), rotY=rot)

# ── fresh-grave vignette: open hole + coffin + shovel + dirt ────────────
prop("OpenGrave", H("SM_Hole_Graveyard"), (10, 1), rotY=0.2, collider=False)
prop("Coffin_Open", H("SM_Coffin_Old"), (12.4, 1.5), rotY=1.4)
prop("Shovel", H("SM_Shovel_Gravedigger"), (9.0, 3.0), rotY=2.4, collider=False)
prop("Coffin_Leaning", H("SM_Coffin_New"), (-12, -2), rotY=0.6)

# ── centerpiece: gibbet + the Bag Man boss ──────────────────────────────
prop("Gibbet", H("SM_Gibbet"), (0, -17), rotY=0.0)
prop("Scarecrow_Boss", H("SM_Scarecrow_B"), (2.4, -15), rotY=math.pi)

# ── witch corner: cauldron + scarecrows + green glow ────────────────────
prop("Cauldron", H("SM_Cauldron_Potion_Full"), (-21, -21), rotY=0.5,
     emissive=(0.05, 0.35, 0.1), emit_tex=HALL_EMIT)
point_light("CauldronGlow", (-21, 0.7, -21), (0.30, 1.0, 0.45), 7.0, 6.0)
prop("Scarecrow_A", H("SM_Scarecrow_A"), (-18, -23), rotY=0.4)
prop("Scarecrow_B", H("SM_Scarecrow_B"), (17, -22), rotY=-0.5)

# ── dead trees at the corners (collide with the trunk only) ─────────────
for i, (x, z) in enumerate([(-24, -24), (24, -24), (-24, 20), (24, 18)]):
    prop(f"DeadTree_{i}", H("SM_Tree_Scary_Dead"), (x, z), rotY=0.7 * i,
         scale=0.8, trunk=0.6)

# ── street lanterns flanking the path (+ warm point lights) ─────────────
lantern_lights = 0
for zz in (16, 2, -12):
    for sx in (-4.5, 4.5):
        prop(f"Lantern_{zz}_{int(sx)}", H("SM_Lantern_Street_Fixed_Halloween"),
             (sx, zz), rotY=(0 if sx < 0 else math.pi))
        if lantern_lights < 4:
            point_light(f"LanternGlow_{zz}_{int(sx)}", (sx, 3.0, zz),
                        (1.0, 0.72, 0.32), 6.0, 8.0)
            lantern_lights += 1

# ── carved jack-o-lanterns (emissive + a few point lights) ──────────────
PUMP = ["SM_Pumpkin_Carved_Lit_A", "SM_Pumpkin_Carved_Lit_B", "SM_Pumpkin_Carved_Lit_C"]
pump_spots = [(-2.5, 19), (3, 9), (-3, -2), (2.5, -10), (-14, -16), (15, 7)]
for i, (x, z) in enumerate(pump_spots):
    prop(f"Pumpkin_{i}", H(PUMP[i % 3]), (x, z), rotY=0.5 * i, collider=False,
         emissive=(1.0, 0.45, 0.1), emit_tex=HALL_EMIT)
    if i < 2:   # keep total point lights <= JCE_MAX_POINT_LIGHTS (8)
        point_light(f"PumpkinGlow_{i}", (x, 0.5, z), (1.0, 0.5, 0.15), 5.0, 4.5)

# ── wandering ghosts + a fox ─────────────────────────────────────────────
add("Ghost_A", [T((-7, 0, -6), 0.7), MR(mesh="models/CesiumMan.glb",
    albedo="textures/CesiumMan_tex0.jpg", rough=0.85)])
add("Ghost_B", [T((8, 0, -9), -1.0), MR(mesh="models/CesiumMan.glb",
    albedo="textures/CesiumMan_tex0.jpg", rough=0.85)])
add("Fox", [T((6, 0, 8), -1.2, (0.008, 0.008, 0.008)),
            MR(mesh="models/Fox.glb", albedo="textures/Fox_tex0.png", rough=0.9)])

# ── dynamic physics props near the entrance (real Bullet rigidbodies) ───
# Fully DYNAMIC (pushable / knock-overable). They no longer jitter when the
# player stands on them: the runtime presses the character's weight onto the
# body it stands on (rt_snap_character_to_ground), seating it on the floor so
# the kinematic controller's penetration recovery can't kick it into a bounce.
prim("Crate_A", (1.6, 0.5, 20), (0.8, 0.8, 0.8), 0, (0.52, 0.38, 0.22),
     rough=0.8, collider=True, dynamic=True, mass=3.0)
prim("Crate_B", (2.4, 0.5, 20.5), (0.8, 0.8, 0.8), 0, (0.50, 0.36, 0.20),
     rough=0.8, collider=True, dynamic=True, mass=3.0)
prim("Crate_C", (2.0, 1.4, 20.2), (0.8, 0.8, 0.8), 0, (0.54, 0.40, 0.24),
     rough=0.8, collider=True, dynamic=True, mass=3.0)
prim("Barrel", (-2.2, 0.6, 20.2), (0.55, 0.9, 0.55), 4, (0.30, 0.42, 0.30),
     rough=0.7, collider=True, dynamic=True, mass=4.0)

# ── objective beacon ────────────────────────────────────────────────────
prim("Beacon", (0, 1.2, -22), (0.5, 0.5, 0.5), 1, (1.0, 0.62, 0.18), rough=0.3,
     emissive=(1.0, 0.5, 0.1))
point_light("BeaconGlow", (0, 1.4, -22), (1.0, 0.55, 0.15), 6.0, 7.0)

# ── ambient audio ────────────────────────────────────────────────────────
add("Ambience", [T((0, 1, 0)), {"type": "AudioSource",
    "clipPath": "musics/AlkaKrab Music/ogg/Ambient 3.ogg", "loop": True,
    "playOnAwake": True, "volume": 0.45, "pitch": 1.0, "spatialBlend": 0.0}])

# ── player = animated Bag Man (third-person: press V in play) ────────────
# The play harness switches the clip by name (idle/walk/run) from the movement
# input. No state machine bound here on purpose (name-driven clips work + editor
# preview stays usable). To drive it by a conditional SM instead, set the
# Skeletal Animator's "State Machine" field in the Inspector.
pid = add("Player", [
    T((0, 0.0, 24), 0.0, (0.008, 0.008, 0.008)),
    MR(mesh="models/Fox.glb", albedo="textures/Fox_tex0.png", rough=0.9),
    # GENERIC-ENGINE LOCOMOTION = a 1D BLEND TREE (not a discrete state machine):
    # Idle(Survey)->Walk->Run cross-blend SMOOTHLY by movement speed, so there is
    # NO discrete-state flicker / twitch. The engine auto-feeds blend_param from
    # the entity's movement speed in Play (clipNames[i] sits at blendThresholds[i]).
    # Tune the thresholds using the "anim_drive: Speed=..." lines in the Console.
    {"type": "SkeletalAnimator", "skeletonPath": "models/Fox.glb",
     "clipNames": ["Survey", "Walk", "Run"],
     "useBlendTree": True,
     "autoSpeed": True,   # opt in to engine-driven locomotion (Speed/blend by movement)
     # Matched to the real speeds from the anim_drive log: standing=0, normal
     # move ~4 (Walk), Shift-sprint ~7 (Run).
     "blendThresholds": [0.0, 4.0, 7.0],
     "loop": True, "playing": True, "speed": 1.0},
    {"type": "CharacterController", "height": 0.55, "radius": 0.2,
     "stepOffset": 0.2, "slopeLimit": 50.0}])
add("PlayerCamera", [T((0, 0.5, 0)), {"type": "Camera", "fov": 72,
    "nearClip": 0.05, "farClip": 400.0, "primary": True, "orthographic": False}],
    parent=pid)

doc = {"format_version": 1,
       "_design_note": "Hollow's End demo — generated by examples/caged_kingdom/tools/gen_graveyard_demo.py. "
                       "Individual Halloween props at native scale, auto-seated by pivot, "
                       "with per-prop BoxColliders sized from measured bounds.",
       "entities": ents}
OUT.write_bytes(json.dumps(doc, indent=2).encode("utf-8"))
print(f"wrote {OUT} — {len(ents)} entities")
