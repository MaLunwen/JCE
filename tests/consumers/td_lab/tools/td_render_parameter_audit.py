"""Pinned original parsing and requested authoring audit; no engine/GPU execution."""
from __future__ import annotations

import colorsys
import hashlib
import json
import math
from pathlib import Path
import re

REFERENCE_COMMIT = "784129a685627a18f3d3f8fa594208000d9313e8"
LEVEL = "js/TowerDefense/Levels/TowerDefense.Level.js"
CORE = "js/TowerDefense/TowerDefense.js"
THREE = "js/lib/three.min.js"
RENDER_REFERENCE_PATHS = (LEVEL, CORE, THREE)
# The declared first-party policy difference was reviewed in this exact shader.
# Any source change requires re-audit before carrying that conclusion forward.
CLASSIC_POLICY_WITNESS_SHA256 = "8baa3ae159846d4ee064f0c15ab2d4ad756b59c8c04218b46705532eebac58e6"
NUM = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"


class ParameterAuditError(ValueError):
    """Missing, ambiguous, unpinned or mismatched audit input."""


def load_pinned_sources(reference_root, manifest_path, paths):
    """Bind the exact bytes used for parsing to the acquisition manifest."""
    manifest_path = Path(manifest_path)
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest.get("commit") != REFERENCE_COMMIT:
        raise ParameterAuditError("Reference manifest commit differs from pinned original")
    entries = {}
    for row in manifest.get("files", []):
        key = row.get("path")
        if key in entries:
            raise ParameterAuditError(f"Ambiguous manifest path: {key}")
        entries[key] = row
    sources, files = {}, {}
    root = Path(reference_root).resolve()
    for key in paths:
        path = (root / key).resolve()
        if not path.is_relative_to(root) or key not in entries:
            raise ParameterAuditError(f"Unpinned reference path: {key}")
        blob = path.read_bytes()
        sha = hashlib.sha256(blob).hexdigest()
        expected = entries[key]
        if sha != expected.get("sha256") or len(blob) != expected.get("bytes"):
            raise ParameterAuditError(f"Reference byte identity changed: {key}")
        sources[key] = blob.decode("utf-8")
        files[key] = {"path": str(path), "sha256": sha, "bytes": len(blob)}
    return sources, {"commit": REFERENCE_COMMIT, "manifest_path": str(manifest_path.resolve()),
                     "manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(), "files": files}


def _one(pattern, text, label):
    matches = list(re.finditer(pattern, text, flags=re.MULTILINE | re.DOTALL))
    if len(matches) != 1:
        raise ParameterAuditError(f"Expected one bounded original declaration: {label}")
    return matches[0]


def _number(field, text):
    return float(_one(re.escape(field) + r"\s*=\s*(" + NUM + r")\s*;", text, field)[1])


def _triplet(field, text):
    match = _one(re.escape(field) + r"\s*\(\s*(" + NUM + r")\s*,\s*(" + NUM
                 + r")\s*,\s*(" + NUM + r")\s*\)", text, field)
    return [float(match[n]) for n in (1, 2, 3)]


def _rgb_hex(value):
    return [(value >> shift & 255) / 255 for shift in (16, 8, 0)]


def _basis(value):
    return [value[0], value[2], -value[1]]


def _normalize(value):
    size = math.sqrt(sum(x * x for x in value))
    if not math.isfinite(size) or size <= 0:
        raise ParameterAuditError("Nonfinite or zero original light direction")
    return [x / size for x in value]


def parse_reference_rendering(sources):
    """A bounded parser for these pinned declarations, not a general JS interpreter."""
    level, core, three = (sources[k] for k in RENDER_REFERENCE_PATHS)
    camera = _one(r"new\s+THREE\.PerspectiveCamera\s*\(\s*(" + NUM
                  + r")\s*,\s*TowerDefense\.gameWidth\s*/\s*TowerDefense\.gameHeight\s*,\s*("
                  + NUM + r")\s*,\s*(" + NUM + r")\s*\)", level, "PerspectiveCamera")
    eye = [_number("TowerDefense.camera.position." + axis, level) for axis in "xyz"]
    hemi = _one(r"new\s+THREE\.HemisphereLight\s*\(\s*(0x[0-9a-fA-F]+)\s*,\s*"
                r"(0x[0-9a-fA-F]+)\s*,\s*(" + NUM + r")\s*\)", level, "HemisphereLight")
    sun = _one(r"new\s+THREE\.DirectionalLight\s*\(\s*(0x[0-9a-fA-F]+)\s*,\s*("
               + NUM + r")\s*\)", level, "DirectionalLight")
    h, s, lightness = _triplet("dirLight.color.setHSL", level)
    if not all(0 <= x <= 1 for x in (h, s, lightness)):
        raise ParameterAuditError("Original HSL is outside the supported finite domain")
    # Independent standard HSL conversion; no color value comes from the port.
    sun_rgb = list(colorsys.hls_to_rgb(h, lightness, s))
    pos = _triplet("dirLight.position.set", level)
    scale = float(_one(r"dirLight\.position\.multiplyScalar\s*\(\s*(" + NUM + r")\s*\)",
                       level, "sun position scale")[1])
    sun_pos = [x * scale for x in pos]
    shadow_enabled = _one(r"\badvancedLight\s*:\s*(true|false)\b", core, "advancedLight")[1] == "true"
    _one(r"dirLight\.castShadow\s*=\s*true\s*;", level, "directional shadow request")
    # r68 allocates using LIGHT dimensions, not similarly named renderer fields.
    _one(r"new\s+THREE\.WebGLRenderTarget\(\s*(\w+)\.shadowMapWidth\s*,\s*\1\.shadowMapHeight\b",
         three, "per-light shadow render-target allocation")
    chunk = _one(r'THREE\.ShaderChunk\.shadowmap_fragment=("(?:\\.|[^"\\])*")',
                 three, "shadowmap_fragment")
    shadow_fragment = json.loads(chunk[1])
    compact = re.sub(r"\s+", "", shadow_fragment)
    if ("gl_FragColor.xyz=gl_FragColor.xyz*shadowColor;" not in compact
            or "1.0-shadowDarkness[i]*shadow" not in compact):
        raise ParameterAuditError("Unrecognized pinned original shadow application")
    if "this.gammaOutput=this.gammaInput=!1" not in three:
        raise ParameterAuditError("Original default gamma flags were not independently found")
    darkness = _number("dirLight.shadowDarkness", level)
    return {
        "camera": {"fov": float(camera[1]), "near": float(camera[2]), "far": float(camera[3]),
                   "aspect_expression": "TowerDefense.gameWidth / TowerDefense.gameHeight",
                   "position_original": eye, "position_jce": _basis(eye)},
        "hemisphere": {"sky_rgb": _rgb_hex(int(hemi[1], 16)),
                       "ground_rgb": _rgb_hex(int(hemi[2], 16)), "intensity": float(hemi[3]),
                       "direction_jce": _basis(_normalize(_triplet("hemiLight.position.set", level)))},
        "sun": {"intensity": float(sun[2]), "hsl": [h, s, lightness], "color_rgb": sun_rgb,
                "position_original": sun_pos, "direction_jce": [-x for x in _normalize(_basis(sun_pos))]},
        "shadow": {"enabled": shadow_enabled,
                   "map_size": [_number("dirLight.shadowMapWidth", level), _number("dirLight.shadowMapHeight", level)],
                   "renderer_legacy_map_size": [_number("TowerDefense.renderer.shadowMapWidth", level),
                                                _number("TowerDefense.renderer.shadowMapHeight", level)],
                   "darkness": darkness, "light_camera_far": _number("dirLight.shadowCameraFar", level),
                   "declared_light_fov": _number("dirLight.shadowCameraFov", level),
                   "projection_note": "r68 DirectionalLight uses an orthographic shadow camera; declared light FOV is not its active projection",
                   "application": "whole_phong_result", "fully_occluded_factor": 1 - darkness,
                   "allocation_basis": "r68 ShadowMapPlugin uses light.shadowMapWidth/Height",
                   "fragment_sha256": hashlib.sha256(shadow_fragment.encode("utf-8")).hexdigest()},
        "gamma": {"input": False, "output": False},
        "coordinate_basis": "original (x,y,z) -> JCE (x,z,-y); directional API stores shine direction",
    }


def _component(scene, entity_name, component_type):
    entities = [e for e in scene.get("entities", []) if e.get("name") == entity_name]
    if len(entities) != 1:
        raise ParameterAuditError(f"Missing/ambiguous authored entity: {entity_name}")
    components = [c for c in entities[0].get("components", []) if c.get("type") == component_type]
    if len(components) != 1:
        raise ParameterAuditError(f"Missing/ambiguous authored component: {entity_name}.{component_type}")
    return components[0]


def _get(value, *path):
    for field in path:
        if not isinstance(value, dict) or field not in value:
            raise ParameterAuditError("Missing requested field: " + ".".join(path))
        value = value[field]
    return value


def _equal(actual, expected):
    if isinstance(expected, list):
        return isinstance(actual, list) and len(actual) == len(expected) and all(
            _equal(a, e) for a, e in zip(actual, expected))
    if isinstance(expected, bool):
        return type(actual) is bool and actual == expected
    if isinstance(expected, (int, float)):
        return type(actual) in (int, float) and math.isfinite(actual) and math.isclose(
            actual, expected, rel_tol=0, abs_tol=1e-12)
    return type(actual) is type(expected) and actual == expected


def audit_requested_rendering(reference, scene_doc, settings_doc, rp_doc, rp_cooked_doc, classic_source):
    """Certify authored requests only; shader policy differences stay non-PASS."""
    if scene_doc.get("contract", {}).get("name") != "jce.scene":
        raise ParameterAuditError("Not the public scene contract")
    scene = _get(scene_doc, "scene")
    camera = _component(scene, "MainCamera", "Camera")
    eye = _component(scene, "MainCamera", "Transform")
    sun = _component(scene, "Sun", "DirectionalLight")
    rendering = _get(scene, "rendering")
    rows, policy_checks = [], []

    def compare(field, actual, expected, basis):
        if not _equal(actual, expected):
            raise ParameterAuditError(f"Requested {field} differs: {actual!r} != {expected!r}")
        rows.append({"parameter": field, "original": expected, "requested": actual,
                     "basis": basis, "scope": "REQUESTED_AUTHORING_ONLY", "pass": True})

    for field, authored in (("fov", "fov"), ("near", "nearClip"), ("far", "farClip")):
        compare("camera." + field, _get(camera, authored), reference["camera"][field], LEVEL + ": PerspectiveCamera")
    compare("camera.perspective", _get(camera, "orthographic"), False, "Original PerspectiveCamera type")
    compare("camera.position", [_get(eye, "pos" + x) for x in "XYZ"],
            reference["camera"]["position_jce"], "Original camera eye and declared coordinate basis")
    compare("hemisphere.enabled", _get(rendering, "look", "hemisphere"), True, LEVEL + ": HemisphereLight declaration")
    compare("hemisphere.sky", _get(rendering, "ambient", "color"), reference["hemisphere"]["sky_rgb"], "Original sky hex channels / 255")
    compare("hemisphere.ground", _get(rendering, "look", "groundColor"), reference["hemisphere"]["ground_rgb"], "Original ground hex channels / 255")
    compare("hemisphere.intensity", _get(rendering, "ambient", "intensity"), reference["hemisphere"]["intensity"], "Original hemisphere intensity")
    compare("sun.color", [_get(sun, "color" + x) for x in "RGB"], reference["sun"]["color_rgb"], "Original HSL arguments; independent standard HSL conversion")
    compare("sun.intensity", _get(sun, "intensity"), reference["sun"]["intensity"], "Original DirectionalLight intensity")
    compare("sun.direction", [_get(sun, "dir" + x) for x in "XYZ"], reference["sun"]["direction_jce"], "Original position*scale, coordinate basis, normalized shine direction")
    compare("sun.cast_shadow", _get(sun, "castsShadow"), reference["shadow"]["enabled"], "Original advancedLight and per-light shadow request")
    compare("scene.shadow_map_size", [_get(rendering, "shadows", "resolution")] * 2,
            reference["shadow"]["map_size"], "Original per-light width/height allocation")
    compare("rp.shadow_map_size", [_get(rp_doc, "shadow_resolution")] * 2,
            reference["shadow"]["map_size"], "Original per-light width/height allocation")
    compare("rp.shadow_enabled", _get(rp_doc, "enable_csm"), reference["shadow"]["enabled"], "Original shadow request; no claim of identical shadow projection/filter")

    def require(field, actual, expected, basis):
        if not _equal(actual, expected):
            raise ParameterAuditError(f"Requested policy {field} differs: {actual!r} != {expected!r}")
        policy_checks.append({"parameter": field, "requested": actual, "required": expected,
                              "basis": basis, "scope": "JCE_REQUEST_POLICY_ONLY", "pass": True})

    require("rp.schema", _get(rp_doc, "$schema"), "jce.rp.v2", "Public JCE request document")
    require("rp.post_quality", _get(rp_doc, "post_quality"), "mid", "Explicit JCE material-look request, not an original Three quality enum")
    require("graphics.color_space", _get(settings_doc, "graphics", "color_space"), 0,
            "Pinned original gammaInput/gammaOutput false -> public JCE Gamma numeric mode")
    if rp_doc != rp_cooked_doc:
        raise ParameterAuditError("Requested host/cooked RP copies differ")
    level_index = _get(settings_doc, "quality", "current_level")
    levels = _get(settings_doc, "quality", "levels")
    if type(level_index) is not int or not isinstance(levels, list) or not 0 <= level_index < len(levels):
        raise ParameterAuditError("Requested quality level unavailable")
    distance = _get(rendering, "shadows", "distance")
    require("settings.shadow_distance_matches_scene", _get(levels[level_index], "shadow_distance"), distance,
            "Request-document consistency only; source light far and JCE camera-depth cutoff are distinct semantics")
    require("settings.shadow_cascades_matches_rp", _get(levels[level_index], "shadow_cascades"),
            _get(rp_doc, "csm_cascade_count"), "Request-document consistency only")
    classic_sha = hashlib.sha256(classic_source.encode("utf-8")).hexdigest()
    if classic_sha != CLASSIC_POLICY_WITNESS_SHA256:
        raise ParameterAuditError("Classic policy witness changed; re-audit the explicit shadow difference")
    if ("diffuse += radiance*ndl*shadow;" not in classic_source
            or "specular += radiance*ndl*highlight*fresnel*specMask*shadow;" not in classic_source):
        raise ParameterAuditError("Current classic shadow policy is unrecognized; re-audit the explicit difference")
    return {
        "scope": "REQUESTED_AUTHORING_ONLY",
        "status": "REQUESTED_PARAMETERS_MATCH_WITH_RECORDED_POLICY_DIFFERENCE",
        "comparisons": rows, "port_policy_checks": policy_checks,
        "reference_parameters": reference,
        "policy_differences": [{
            "name": "shadow_application", "status": "DOCUMENTED_DIFFERENCE_NOT_PARITY_PASS", "pass": None,
            "original": {"darkness": reference["shadow"]["darkness"], "application": "whole_phong_result",
                         "factor": "1 - light.shadowDarkness * occlusion"},
            "jce": {"application": "directional_diffuse_and_specular_only", "hemisphere": "not multiplied by CSM",
                    "reviewed_shader_sha256": classic_sha},
        }],
        "adaptations": [{"name": "shadow_distance", "source_light_projection_far": reference["shadow"]["light_camera_far"],
                         "requested_scene_camera_depth_cutoff": distance,
                         "pass": None, "scope": "DISTINCT_SEMANTICS_NOT_EXACT_PARAMETER_PARITY"}],
        "limits": [
            "Authored scene/settings/RP requests are not effective hardware, submitted uniforms, shader output or performance.",
            "Aspect remains the original window-size expression; directional shadow FOV declarations are not its active orthographic projection.",
            "Only the pinned declaration forms and named authoring entities are parsed; actual GPU evidence is separate.",
        ],
    }
