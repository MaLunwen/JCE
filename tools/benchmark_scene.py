#!/usr/bin/env python3
"""Deterministic, minimal editor scene for JCE's five stress families.

This scene is a JCE-only fixture. Built-in cube/sphere topology is not the
canonical imported mesh required for a Unity/Unreal/Godot comparison.
"""
from __future__ import annotations

import json
import math
from pathlib import Path


def fixture_radius(kind, count, spread=0.0):
    if kind in ("instancing", "triangle"):
        radius = math.sqrt(count) * 1.25 + 8.0
    elif spread > 0:
        radius = spread * 0.65 + 8.0
    else:
        radius = math.ceil(count ** (1.0 / 3.0)) * 1.2 + 8.0
    return radius


def viewport_pose(kind, count, spread=0.0):
    """Fixed editor orbit camera, independent of saved per-user UI state."""
    return "45,-50,%.3f,0,0,0" % (fixture_radius(kind, count, spread) * 3.0)


def scene_document(kind, count, spread=0.0):
    radius = fixture_radius(kind, count, spread)
    height = max(100.0, radius * 2.5)
    return {
        "entities": [
            {
                "id": 1, "parentId": 0, "name": "BenchmarkCamera",
                "components": [
                    {"type": "Transform",
                     "posX": 0.0, "posY": height, "posZ": 0.0,
                     "rotX": -90.0, "rotY": 0.0, "rotZ": 0.0,
                     "scaleX": 1.0, "scaleY": 1.0, "scaleZ": 1.0},
                    {"type": "Camera",
                     "fov": 60.0, "nearClip": 0.1,
                     "farClip": max(1000.0, height * 4.0),
                     "primary": True, "orthographic": True,
                     "orthoSize": radius * 2.0, "clearMode": 1},
                ],
            },
            {
                "id": 2, "parentId": 0, "name": "BenchmarkSun",
                "components": [
                    {"type": "Transform",
                     "posX": 0.0, "posY": 0.0, "posZ": 0.0,
                     "rotX": -60.0, "rotY": 25.0, "rotZ": 0.0,
                     "scaleX": 1.0, "scaleY": 1.0, "scaleZ": 1.0},
                    {"type": "Light", "lightType": 0, "intensity": 1.1,
                     "colorR": 1.0, "colorG": 1.0, "colorB": 0.97},
                ],
            },
        ],
    }


def write_scene(path, kind, count, spread=0.0):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(scene_document(kind, count, spread),
                         sort_keys=True, separators=(",", ":")) + "\n"
    if path.exists():
        if path.read_text(encoding="utf-8") != payload:
            raise ValueError("benchmark fixture path has different content: %s" % path)
    else:
        path.write_text(payload, encoding="utf-8", newline="\n")
    return path
