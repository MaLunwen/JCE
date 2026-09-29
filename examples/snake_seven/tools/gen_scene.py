#!/usr/bin/env python3
"""gen_scene.py -- author snake.scene.json from the spec, not by hand.

The board is 30x20 (spec 11).  A generator rather than a literal file because
the body pool alone is 64 near-identical entities, and a hand-kept scene is
exactly where a board that says 30x20 in one place and 17x17 in another
comes from.

COORDINATES.  Cell (cx, cz) with cx in [0,COLS) and cz in [0,ROWS) maps to

    x = cx - (COLS-1)/2      z = cz - (ROWS-1)/2

so the board is centred on the origin and the camera needs no offset.

THE CAMERA LOOKS STRAIGHT DOWN, and the sign matters.  jce_scene_camera.c
takes forward as the NEGATED third column of the world matrix, so a rotation
of t about X gives forward = (0, sin t, -cos t) and up = (0, cos t, sin t).
At t = -90 that is forward (0,-1,0) and up (0,0,-1): straight down, with
SCREEN-UP = -Z.  Both are exact; up is read off the same matrix, so there is
no lookAt degeneracy to steer around and no reason to fudge the angle.

Screen-up being -Z is the entire reason UP must DECREASE cz in input.jcejs.
Getting this backwards is not visible in any symmetric test: the editor and
the runtime invert together, and two identical wrong pictures compare equal.
"""
import json
import pathlib

COLS, ROWS = 30, 20          # spec 11
BODY_POOL = 64               # far above any length this board will reach
PARKED_Y = -100.0            # where an unused body segment waits, off camera

MESH_CUBE = 2


def cell_to_world(cx, cz):
    return (cx - (COLS - 1) / 2.0, cz - (ROWS - 1) / 2.0)


def transform(pos=(0, 0, 0), rot=(0, 0, 0), scale=(1, 1, 1)):
    return {"type": "Transform",
            "posX": float(pos[0]), "posY": float(pos[1]), "posZ": float(pos[2]),
            "rotX": float(rot[0]), "rotY": float(rot[1]), "rotZ": float(rot[2]),
            "scaleX": float(scale[0]), "scaleY": float(scale[1]),
            "scaleZ": float(scale[2])}


def mesh(rgb, shape=MESH_CUBE):
    return {"type": "MeshRenderer", "meshShape": shape,
            "baseColorR": rgb[0], "baseColorG": rgb[1], "baseColorB": rgb[2],
            "baseColorA": 1.0, "castsShadow": False}


def script(path):
    """A Script component stores a PATH.

    For SOURCE forms (.lua, .py, .java, .jcejs) the bytes at that path are
    read.  For REFERENCE forms (.jcec, .jcecpp, .cs) they are NOT: the
    basename IS the class name, the class lives in a linked native module
    or a loaded assembly, and no such file needs to exist on disk.  So
    "scripts/SnakeBody.jcecpp" names the C++ class SnakeBody and nothing
    more -- do not go looking for the file, and do not create one.
    """
    return {"type": "Script", "scriptPath": path}


def text(txt, size, anchor, offset, size_wh, rgb, align=0):
    ax, ay = anchor
    # fontPath is deliberately EMPTY.  An empty path means "this machine's
    # UI font": the engine resolves the platform's own UI face and only
    # falls back to its bundled fonts/JCE.ttf if none is readable.  JCE.ttf
    # is a hand-drawn glyph set that ships with the sample content -- it is
    # artwork, not a UI face -- and naming it here is how a scene copied
    # from a sample inherits a decorative font for its HUD.
    return {"type": "UIText", "text": txt, "fontPath": "",
            "fontSize": size, "alignment": align,
            "colorR": rgb[0], "colorG": rgb[1], "colorB": rgb[2],
            "colorA": 1.0,
            "lineSpacing": 1.2, "richText": False, "mathText": False,
            "bestFit": False, "minSize": 10, "maxSize": size,
            "anchorMinX": ax, "anchorMinY": ay,
            "anchorMaxX": ax, "anchorMaxY": ay,
            "pivotX": ax, "pivotY": ay,
            "anchoredX": offset[0], "anchoredY": offset[1],
            "sizeW": size_wh[0], "sizeH": size_wh[1]}


def build():
    ents = []
    nid = [1]

    def add(name, comps, parent=0):
        eid = nid[0]
        nid[0] += 1
        ents.append({"id": eid, "parentId": parent, "name": name,
                     "components": comps})
        return eid

    # Orthographic, 22 world units tall for a 20-tall board: one cell of
    # margin above and below.  The WIDTH is deliberately not authored --
    # JceCameraComponent carries only a height and the viewport aspect
    # decides the rest, so the board frames the same in 16:9 and in 4:3.
    add("MainCamera",
        [transform(pos=(0, 20, 0), rot=(-90, 0, 0)),
         {"type": "Camera", "fov": 60.0, "nearClip": 0.1, "farClip": 100.0,
          "primary": True, "orthographic": True, "orthoSize": 22.0,
          "clearMode": 1}])

    add("Sun", [transform(rot=(-60, 25, 0)),
                {"type": "Light", "lightType": 0, "intensity": 1.1,
                 "colorR": 1.0, "colorG": 1.0, "colorB": 0.97}])

    add("Board", [transform(pos=(0, -0.55, 0), scale=(COLS, 0.1, ROWS)),
                  mesh((0.10, 0.12, 0.16))])

    # A visible border, so "the wall" is a thing on screen and not a rule.
    half_c, half_r = COLS / 2.0, ROWS / 2.0
    for nm, pos, sc in (
            ("WallTop", (0, -0.5, -(half_r + 0.25)), (COLS + 1, 0.4, 0.5)),
            ("WallBottom", (0, -0.5, (half_r + 0.25)), (COLS + 1, 0.4, 0.5)),
            ("WallLeft", (-(half_c + 0.25), -0.5, 0), (0.5, 0.4, ROWS + 1)),
            ("WallRight", ((half_c + 0.25), -0.5, 0), (0.5, 0.4, ROWS + 1))):
        add(nm, [transform(pos=pos, scale=sc), mesh((0.30, 0.34, 0.44))])

    # The blackboard.  No renderer: these exist so seven languages can agree
    # on state through the one surface every binding set has -- a transform.
    # Which field means what is written in BLACKBOARD.md and nowhere else,
    # so there is exactly one place to correct when it changes.
    #
    # POSITION ONLY, never scale or rotation.  Scale is not inert storage:
    # it goes through matrix compose/decompose, a zero component is a
    # degenerate basis, and anything that normalises a transform is free to
    # hand back a different number than was written.  Position is the one
    # channel that round-trips unexamined.  Five slots of three.
    add("GameState", [transform(), script("scripts/GameManager.jcec")])
    add("GameMeta", [transform()])
    add("Direction", [transform(), script("scripts/input.jcejs")])
    add("Pending", [transform()])
    add("Verdict", [transform(), script("scripts/GameBoard.java")])
    # PrevHead exists so the body module does not depend on running BEFORE
    # the snake module in a frame.  Script order within a frame is not
    # guaranteed; if C++ read the head directly it would push the old cell
    # when it happened to run first and the new one when it ran second, and
    # the body would be correct or one cell short depending on nothing the
    # code can see.  Lua writes the vacated cell here as part of the same
    # move, so there is one value with one meaning whenever it is read.
    add("PrevHead", [transform()])
    # CppDiag exists because the C++ script surface has no log binding: a
    # native module can only report by changing state.  x = how many of the
    # five entities it resolved by name, y = the last tick it processed,
    # z = how many history cells it holds.  Written by SnakeBody only.
    add("CppDiag", [transform()])

    hx, hz = cell_to_world(COLS // 2, ROWS // 2)
    head_id = add("SnakeHead",
                  [transform(pos=(hx, 0.0, hz), scale=(0.92, 0.92, 0.92)),
                   mesh((0.45, 1.00, 0.55)),
                   script("scripts/snake.lua")])

    # Eyes, parented to the head, so the head reads as a head (spec 9).
    add("EyeL", [transform(pos=(-0.20, 0.55, 0.18), scale=(0.22, 0.12, 0.22)),
                 mesh((0.05, 0.08, 0.10))], parent=head_id)
    add("EyeR", [transform(pos=(0.20, 0.55, 0.18), scale=(0.22, 0.12, 0.22)),
                 mesh((0.05, 0.08, 0.10))], parent=head_id)

    # The body pool.  Segment 0 carries the C++ module that drives all of
    # them; the rest are plain entities it moves.
    for i in range(BODY_POOL):
        comps = [transform(pos=(0.0, PARKED_Y, 0.0),
                           scale=(0.82, 0.82, 0.82)),
                 mesh((0.20, 0.70, 0.35))]
        if i == 0:
            comps.append(script("scripts/SnakeBody.jcecpp"))
        add("Body%02d" % i, comps)

    fx, fz = cell_to_world(COLS // 4, ROWS // 4)
    add("Food", [transform(pos=(fx, 0.0, fz), scale=(0.62, 0.62, 0.62)),
                 mesh((1.00, 0.35, 0.30)),
                 script("scripts/food.py")])

    canvas = add("Canvas", [transform(),
                            {"type": "Canvas", "renderMode": 0,
                             "sortOrder": 80, "refResX": 1920,
                             "refResY": 1080, "scaleFactor": 1,
                             "pixelPerfect": False},
                            script("scripts/Hud.cs")])

    # SCORE and BEST are on screen in EVERY state (spec 6).
    add("ScoreText", [transform(),
                      text("SCORE 0", 44, (0, 1), (40, -34), (520, 64),
                           (0.92, 0.97, 1.00))], parent=canvas)
    add("BestText", [transform(),
                     text("BEST 0", 36, (1, 1), (-40, -34), (520, 56),
                          (0.72, 0.80, 0.95), align=2)], parent=canvas)
    # Menu, paused and game over all speak through this one overlay pair.
    add("StateText", [transform(),
                      text("SNAKE", 96, (0.5, 0.5), (0, 60), (1400, 150),
                           (1.00, 0.96, 0.75), align=1)], parent=canvas)
    add("HintText", [transform(),
                     text("PRESS ENTER TO START", 40, (0.5, 0.5), (0, -60),
                          (1400, 90), (0.85, 0.90, 1.00), align=1)],
        parent=canvas)

    return {"entities": ents,
            "notes": "Generated by tools/gen_scene.py -- edit that, not this."}


if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parent.parent
    out = root / "resources" / "scenes" / "snake.scene.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    doc = build()
    out.write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n",
                   encoding="utf-8", newline="\n")
    print("wrote %s: %d entities" % (out, len(doc["entities"])))
