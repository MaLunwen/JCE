"""Independent projected masks and paired pixels for the public Water fixture.

No reference screenshot or production render ordering is used as the oracle.
Synthetic images in the adjacent tests qualify this validator only, never GPU.
"""
import hashlib
import itertools
import math

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

SIZE = (1280, 720)
ORTHO_HEIGHT = 12.0
COMBINATIONS = ("B", "BW", "BF", "BWF")
CASES = ("empty", "W", "F") + tuple(
    name + suffix for name in COMBINATIONS for suffix in ("", "-repeat", "-order"))
# Built-in cube(1), matching the public procedural MeshRenderer shape contract.
BOXES = {"B": ((0, -1, 0), (10, .2, 10)),
         "F": ((0, 1, 0), (2.4, .2, 2.4))}
WATER_SIZE = 7.0


def transform(position, scale=(1, 1, 1), pitch=0):
    return dict(type="Transform", posX=position[0], posY=position[1],
                posZ=position[2], rotX=pitch, rotY=0, rotZ=0,
                scaleX=scale[0], scaleY=scale[1], scaleZ=scale[2])


def scene(case):
    if case not in CASES:
        raise ValueError("unknown depth-order case")
    kind = case.split("-")[0]
    entities = [dict(id=1, name="Camera", components=[
        transform((0, 10, 10), pitch=-45),
        dict(type="Camera", primary=True, orthographic=True,
             orthoSize=ORTHO_HEIGHT, fov=60, nearClip=.1, farClip=100,
             clearMode=1)])]
    for index, name in enumerate("BWF", 2):
        if name not in kind or kind == "empty":
            continue
        if name == "W":
            components = [transform((0, 0, 0)), dict(
                type="Water", sizeX=WATER_SIZE, sizeZ=WATER_SIZE,
                baseHeight=0, waveCount=0, waterMode=0, transparency=.5,
                shallowR=.02, shallowG=.08, shallowB=.8,
                deepR=.02, deepG=.08, deepB=.8,
                clarity=0, caustics=0, shoreFoam=0, shoreSurge=0,
                sunSpecular=0, shoreRipple=0, iceRatio=0, splashRatio=0,
                visible=True, depthWrite=False, planarReflection=False)]
        else:
            position, scale = BOXES[name]
            color = (.8, .03, .02) if name == "B" else (.8, .7, .02)
            components = [transform(position, scale), dict(
                type="MeshRenderer", meshShape=0, meshPath="", visible=True,
                baseColorR=color[0], baseColorG=color[1], baseColorB=color[2],
                baseColorA=1, emissiveR=color[0], emissiveG=color[1],
                emissiveB=color[2], metallic=0, roughness=1, normalScale=1,
                aoStrength=1, alphaMode=0, castsShadow=False,
                receivesShadow=False)]
        entities.append(dict(id=index, name=name, components=components))
    if case.endswith("-order"):
        entities = list(reversed(entities))
    return dict(entities=entities, rendering=dict(
        version=1, ambient=dict(color=[0, 0, 0], intensity=0), iblEnabled=False,
        fog=dict(enabled=False, mode=0, density=0),
        postfx=dict(tonemap=True, exposure=1, gamma=2.2, bloom=False,
                    fxaa=False, vignette=False, ssao=False, ssr=False),
        look=dict(autoExposure=False, dof=False, rimIntensity=0, lutStrength=0),
        environment=dict(timeOfDay=dict(enabled=False),
                         weather=dict(type=0, intensity=0))))


def project(point, size=SIZE):
    """Analytical -45deg orthographic projection; no Engine matrix/PNG fit."""
    width, height = size
    x, y, z = point
    return (width / 2 + x * height / ORTHO_HEIGHT,
            height / 2 - (y - z) / math.sqrt(2) * height / ORTHO_HEIGHT)


def hull(points):
    points = sorted(set(points))
    def cross(a, b, c):
        return (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])
    lower, upper = [], []
    for sequence, output in ((points, lower), (reversed(points), upper)):
        for p in sequence:
            while len(output) > 1 and cross(output[-2], output[-1], p) <= 0:
                output.pop()
            output.append(p)
    return lower[:-1] + upper[:-1]


def projected_mask(name, size=SIZE):
    if name == "W":
        half = WATER_SIZE / 2
        points = [(x, 0, z) for x, z in itertools.product((-half, half), repeat=2)]
    else:
        center, scale = BOXES[name]
        points = [tuple(c + s * side / 2 for c, s, side in zip(center, scale, signs))
                  for signs in itertools.product((-1, 1), repeat=3)]
    image = Image.new("L", size)
    ImageDraw.Draw(image).polygon(hull([project(p, size) for p in points]), fill=255)
    return np.asarray(image) != 0


def shrink(mask, radius=3):
    return np.asarray(Image.fromarray(mask.astype("uint8") * 255).filter(
        ImageFilter.MinFilter(radius * 2 + 1))) != 0


def difference(a, b):
    return np.abs(a.astype("int16") - b.astype("int16"))


def close_pixels(a, b, mask, label):
    if not mask.any():
        raise ValueError(label + ": empty comparison ROI")
    delta = difference(a, b)[mask]
    result = dict(pixels=int(mask.sum()), rgb_mae=float(delta.mean()),
                  max_error=int(delta.max()))
    if result["rgb_mae"] > .1 or result["max_error"] > 2:
        raise ValueError(label + ": paired pixels differ " + str(result))
    return result


def changed_pixels(a, b, mask, label):
    if not mask.any():
        raise ValueError(label + ": empty change ROI")
    delta = difference(a, b)[mask]
    result = dict(pixels=int(mask.sum()), rgb_mae=float(delta.mean()),
                  changed_fraction=float((delta.max(axis=1) > 8).mean()))
    if result["changed_fraction"] < .9 or result["rgb_mae"] < 8:
        raise ValueError(label + ": required visible contribution absent " + str(result))
    return result


def validate(images, size=SIZE):
    if set(images) != set(CASES):
        raise ValueError("missing or extra actual depth-order captures")
    width, height = size
    for name, value in images.items():
        if not isinstance(value, np.ndarray) or value.shape != (height, width, 3) or value.dtype != np.uint8:
            raise ValueError("invalid actual RGB shape/type: " + name)
    full = np.ones((height, width), dtype=bool)
    stable = {name + suffix: close_pixels(images[name], images[name + suffix], full,
              name + suffix) for name in COMBINATIONS for suffix in ("-repeat", "-order")}
    geometry, observed, masks, support = {}, {}, {}, {}
    minimum = max(64, width * height // 1000)
    for name in "BWF":
        geometry[name] = projected_mask(name, size)
        observed[name] = difference(images[name], images["empty"]).max(axis=2) > 8
        core = shrink(geometry[name])
        if int(core.sum()) < minimum:
            raise ValueError("projected object ROI too small: " + name)
        coverage = float(observed[name][core].mean())
        outside = ~np.asarray(Image.fromarray(geometry[name].astype("uint8") * 255).filter(
            ImageFilter.MaxFilter(7))).astype(bool)
        spill = int((observed[name] & outside).sum())
        if coverage < .95 or spill > max(8, width * height // 10000):
            raise ValueError("single-object pixels disagree with independent projection: " + name)
        masks[name] = core & shrink(observed[name])
        support[name] = dict(projected_core_pixels=int(core.sum()),
                             observed_core_fraction=coverage, outside_pixels=spill,
                             projected_sha256=hashlib.sha256(geometry[name].tobytes()).hexdigest(),
                             single_object_sha256=hashlib.sha256(observed[name].tobytes()).hexdigest())
    foreground = masks["F"] & masks["W"] & masks["B"]
    # Exclude the full projected foreground silhouette, including its sides.
    free_water = masks["W"] & masks["B"] & shrink(~geometry["F"])
    for name, roi in (("foreground", foreground), ("free_water", free_water)):
        if int(roi.sum()) < minimum:
            raise ValueError("insufficient independently supported ROI: " + name)
    checks = dict(
        water_survives_opaque=changed_pixels(images["BW"], images["B"], free_water,
                                             "water_survives_opaque"),
        background_through_water=changed_pixels(images["BW"], images["W"], free_water,
                                                 "background_through_water"),
        foreground_visible=changed_pixels(images["BWF"], images["BW"], foreground,
                                           "foreground_visible"),
        water_behind_foreground=close_pixels(images["BWF"], images["BF"], foreground,
                                              "water_behind_foreground"),
        water_outside_foreground=close_pixels(images["BWF"], images["BW"], free_water,
                                               "water_outside_foreground"))
    return dict(status="PAIRED_DEPTH_PIXELS_OBSERVED", size=list(size),
                masks=support, stability=stable, checks=checks,
                projection=dict(camera_position=[0, 10, 10], pitch_degrees=-45,
                                ortho_height=ORTHO_HEIGHT, built_in_cube_side=1,
                                boxes=BOXES, water_size=WATER_SIZE,
                                roi_margin_pixels=3),
                roi_sha256={name: hashlib.sha256(mask.tobytes()).hexdigest()
                            for name, mask in (("foreground", foreground), ("free_water", free_water))},
                scope="static flat Water/opaque depth subset; not GPU timing, presentation, grass or ES stylized water")
