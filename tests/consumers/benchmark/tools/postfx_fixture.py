"""First-party controlled PBR room for stock SDK post-processing witnesses."""
import argparse
import json
from pathlib import Path

EFFECTS = ("base", "taa", "ssao", "ssr", "bloom", "fog")


def fixture(effect="base", *, empty=False):
    if effect not in EFFECTS:
        raise ValueError("unknown effect")
    entities = []

    def entity(name, position, scale, component, rotation=(0, 0, 0)):
        entities.append({"id": len(entities) + 1, "parentId": 0, "name": name,
            "components": [{"type": "Transform",
                **dict(zip(("posX", "posY", "posZ"), position)),
                **dict(zip(("scaleX", "scaleY", "scaleZ"), scale)),
                **dict(zip(("rotX", "rotY", "rotZ"), rotation))}, component]})

    entity("Camera", (0, 3, 8), (1, 1, 1),
           {"type": "Camera", "primary": True, "orthographic": False,
            "fov": 55, "nearClip": 0.1, "farClip": 80, "clearMode": 2},
           (-14, 0, 0))
    entity("Sun", (0, 0, 0), (1, 1, 1),
           {"type": "DirectionalLight", "dirX": -0.4, "dirY": -1,
            "dirZ": -0.3, "intensity": 2, "castsShadow": True})
    if not empty:
        objects = [
            ("Floor", (0, -0.2, -2), (12, 0.4, 16), (0.38, 0.4, 0.42), 0.95, 0.12, 0),
            ("Back", (0, 3, -9), (12, 6, 0.4), (0.3, 0.33, 0.4), 0, 0.8, 0),
            ("Left", (-6, 3, -2), (0.4, 6, 14), (0.4, 0.3, 0.25), 0, 0.8, 0),
            ("Right", (6, 3, -2), (0.4, 6, 14), (0.25, 0.35, 0.4), 0, 0.8, 0),
        ]
        for i in range(5):
            objects.append(("Column%u" % i, (-4 + i * 2, 1.5, -5),
                            (0.65, 3, 0.65), (0.5, 0.45, 0.35), 0.1, 0.45, 0))
        for i, color in enumerate(((1, 0.05, 0.01), (0.02, 0.7, 1), (0.9, 0.6, 0.05))):
            objects.append(("Emitter%u" % i, (-2.4 + i * 2.4, 0.7, -1),
                            (0.9, 1.4, 0.9), color, 0.2, 0.2, 12))
        for name, pos, scale, color, metal, rough, emission in objects:
            entity(name, pos, scale, {"type": "MeshRenderer", "meshShape": 0,
                "visible": True, "baseColorR": color[0], "baseColorG": color[1],
                "baseColorB": color[2], "baseColorA": 1, "metallic": metal,
                "roughness": rough, "emissiveR": emission * color[0],
                "emissiveG": emission * color[1], "emissiveB": emission * color[2]})
    return {"entities": entities, "rendering": {
        "ambient": {"color": [0.15, 0.17, 0.2], "intensity": 0.3},
        "iblEnabled": False, "dynamicGI": 0,
        "fog": {"enabled": effect == "fog", "mode": 1,
                "color": [0.35, 0.45, 0.6], "density": 0.08,
                "start": 0, "end": 45, "heightFalloff": 0.1, "heightOrigin": 0},
        "shadows": {"distance": 40, "cascades": 2, "resolution": 1024, "soft": 1},
        "postfx": {"tonemap": True, "bloom": effect == "bloom", "fxaa": False,
                   "vignette": False, "chromatic": False, "grayscale": False,
                   "autoExposure": False, "exposure": 1, "gamma": 2.2,
                   "bloomThreshold": 1, "bloomIntensity": 0.7,
                   "ssao": True, "ssaoRadius": 0.8, "ssaoIntensity": 2,
                   "ssr": True, "ssrIntensity": 1}}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for effect in EFFECTS:
        path = args.out / (effect + ".scene.json")
        path.write_text(json.dumps(fixture(effect), indent=2) + "\n",
                        encoding="utf-8", newline="\n")
    (args.out / "empty.scene.json").write_text(
        json.dumps(fixture(empty=True), indent=2) + "\n",
        encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
