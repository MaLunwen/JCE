"""Strict admission of actual linked WebGL source and submitted ground uniforms.

This validator reads referenced source bytes. It does not execute a GPU, certify
all fragment values, or treat these selected uniforms as the whole effective RP.
"""
from __future__ import annotations

from datetime import datetime
import hashlib
import math
import re
from urllib.parse import urlsplit

from web_evidence import referenced_bytes, require, sha, text

STATUS = "ACTUAL_WEB_GROUND_QUALITY_OBSERVED"
SCOPE = "Instrumented actual WebGL ground draw and linked shader/uniform state. No timing or every-fragment claim."
UNIFORMS = {"u_pbrParams", "u_baseColorFactor", "u_lookHemiGround", "u_ambientColor",
            "u_dirLights", "u_lightCounts", "u_csmParams", "u_shadowQuality"}
DIRECTION_LENGTH = math.hypot(66, 84, 60)
# Reviewed current ESSL lowering, not a portable every-driver shader allowlist.
# Actual captured source = 585-byte bgfx WebGL2 prefix + exact SDK ESSL payload.
# SDK binary: 7269e6f24cfa44e0b20574c2e28bb120335271e1fc6a81af5cc1c5c1e55b3e32
# Payload: c50a167a06d1fc26c1a7787395de51f42f55d264281c3f8ffa3b4c2ee199ecd6
# Unknown full-source identities require a fresh compiler/policy review.
REVIEWED_LINKED_SOURCE_SHA256 = frozenset({
    "35454539c715a07c03a3fd6731bb03afcc96284f7ca461e742ba9f3e30b57e13",
})
EXPECTED = {
    "u_pbrParams": [1 / 15, 1, 1, .5],
    "u_baseColorFactor": [1, 1, 1, 1],
    "u_lookHemiGround": [247 / 255, 1, 243 / 255, 1],
    "u_ambientColor": [243 / 255, 253 / 255, 1, .6],
    "u_dirLights": [66 / DIRECTION_LENGTH, -84 / DIRECTION_LENGTH, 60 / DIRECTION_LENGTH,
                    .7, 1, .96, .9, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    "u_lightCounts": [1, 0, 0, 1],
}


def _object(value, name):
    require(type(value) is dict, "Missing/malformed object: " + name)
    return value


def _finite(value, name, *, integer=False, minimum=None):
    require(type(value) in (int, float), "Nonnumeric " + name)
    try:
        valid = math.isfinite(value)
    except (OverflowError, ValueError):
        valid = False
    require(valid, "Nonfinite " + name)
    if integer:
        require(type(value) is int, "Noninteger " + name)
    if minimum is not None:
        require(value >= minimum, "Out-of-range " + name)
    return value


def _json_tree(value, name="quality", depth=0):
    # Unknown extension fields cannot hide NaN, Python objects or malformed keys.
    require(depth <= 64, "Quality JSON nesting exceeds bounded admission")
    if value is None or type(value) in (str, bool):
        return
    if type(value) in (int, float):
        _finite(value, name)
        return
    if type(value) is list:
        for index, item in enumerate(value):
            _json_tree(item, f"{name}[{index}]", depth + 1)
        return
    if type(value) is dict:
        for key, item in value.items():
            require(type(key) is str and bool(key), "Malformed JSON key: " + name)
            _json_tree(item, name + "." + key, depth + 1)
        return
    raise ValueError("Non-JSON value: " + name)


def _utc(value, name):
    value = text(value, name)
    require(re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?(?:Z|\+00:00)", value),
            "UTC timestamp required: " + name)
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise ValueError("Invalid UTC timestamp: " + name) from error


def _uniforms(value, name, *, ground, complete=True):
    value = _object(value, name)
    require(set(value) == UNIFORMS if complete else set(value) <= UNIFORMS,
            "Missing/unknown actual uniform set: " + name)
    for key, values in value.items():
        expected = EXPECTED.get(key)
        size = len(expected) if expected is not None else 4
        require(type(values) is list and len(values) == size, "Actual uniform shape differs: " + key)
        for index, actual in enumerate(values):
            _finite(actual, key + f"[{index}]")
            if ground and expected is not None:
                require(abs(actual - expected[index]) <= 1e-6, "Actual ground uniform differs: " + key)
    return value


def _js_units(value):
    return len(value.encode("utf-16-le", errors="surrogatepass")) // 2


def _js_prefix(value):
    return value.encode("utf-16-le", errors="surrogatepass")[:600].decode("utf-16-le", errors="surrogatepass")


def _power_witness(source):
    source_sha = hashlib.sha256(source.encode("utf-8")).hexdigest()
    require(source_sha in REVIEWED_LINKED_SOURCE_SHA256,
            "Unknown full linked-source identity; compiler/policy re-audit required")
    # No comments can serve as evidence of executable guard structure.
    code = re.sub(r"/\*[\s\S]*?\*/|//[^\n]*", "", source)
    compact = re.sub(r"\s+", "", code)
    direct = re.compile(
        r"floatclassic_lobe_power\(float(?P<base>[A-Za-z_]\w*),float(?P<exp>[A-Za-z_]\w*)\)\{"
        r"if\((?P=exp)==0\.0\)(?:\{return1\.0;\}|return1\.0;)"
        r"if\((?P=base)<=0\.0\)(?:\{return0\.0;\}|return0\.0;)"
        r"returnpow\((?P=base),(?P=exp)\);\}")
    definitions = list(direct.finditer(compact))
    calls = len(re.findall(r"classic_lobe_power\(", compact))
    if len(definitions) == 1 and calls == 4:
        return {"kind": "direct_classic_lobe_power_guard", "lobe_calls": 3}
    lowered = re.compile(
        r"if\(\((?P<exp>[A-Za-z_]\w*)==0\.0\)\)\{(?P<result>[A-Za-z_]\w*)=1\.0;\}else\{"
        r"if\(\((?P<base>[A-Za-z_]\w*)<=0\.0\)\)\{(?P=result)=0\.0;\}else\{"
        r"(?P=result)=pow\((?P=base),(?P=exp)\);\};\};")
    guards = list(lowered.finditer(compact))
    if calls == 0 and len(guards) == 3 and len({m["base"] for m in guards}) == 3 and len({m["result"] for m in guards}) == 3:
        return {"kind": "shaderc_inlined_three_finite_power_guards", "lobe_calls": 3}
    raise ValueError("Unknown finite-power linked-source lowering; re-audit required")


def validate_web_render_quality(data, artifact_sha256):
    """Reject incomplete/mismatched evidence; return its limited admission summary."""
    artifact_sha256 = sha(artifact_sha256, "current Web artifact")
    data = _object(data, "Web render quality")
    _json_tree(data)
    require(type(data.get("schema")) is int and data["schema"] == 1 and data.get("status") == STATUS,
            "Successful current Web ground-quality schema required")
    require(type(data.get("errors")) is list and not data["errors"], "Quality observation contains errors")
    require("partial_observation" not in data, "Partial/failed observation cannot be promoted")
    for key in ("artifact_sha256", "served_artifact_sha256", "executed_artifact_sha256"):
        require(sha(data.get(key), key) == artifact_sha256, "Web quality artifact identity differs: " + key)
    url = urlsplit(text(data.get("artifact_url"), "actual artifact URL"))
    require(url.scheme in ("http", "https") and bool(url.hostname) and not url.query and not url.fragment,
            "Unmodified HTTP(S) artifact URL required")
    require(re.fullmatch(r"[A-Za-z0-9_-]{1,64}", text(data.get("run_id"), "quality run_id")), "Invalid quality run_id")
    require(data.get("scope") == SCOPE, "Current producer observation scope required")
    text(data.get("executed_transport"), "actual executed transport")
    start, end = _utc(data.get("generated_utc"), "generated_utc"), _utc(data.get("completed_utc"), "completed_utc")
    require(start < end, "Quality observation UTC window is empty/reversed")
    duration_ms = (end - start).total_seconds() * 1000
    require(type(data.get("lost")) is int and data["lost"] == 0, "Bounded ground observation lost records")
    contexts = data.get("contexts")
    require(type(contexts) is list and len(contexts) == 1, "Actual single WebGL2 context required")
    context = _object(contexts[0], "actual WebGL context")
    require(context.get("kind") == "webgl2" and context.get("lost") is False,
            "Actual live WebGL2 context unavailable")
    require(text(context.get("version"), "actual WebGL version").startswith("WebGL 2.0") and
            text(context.get("shading_language"), "actual GLSL version").startswith("WebGL GLSL ES 3."),
            "Actual WebGL2/GLSL context version differs")
    text(context.get("vendor"), "actual GPU vendor")
    text(context.get("renderer"), "actual GPU renderer")
    draws = data.get("ground_draws")
    require(type(draws) is list and 1 <= len(draws) <= 2, "Actual bounded ground draws unavailable")
    draw_calls = _finite(data.get("draw_calls"), "draw_calls", integer=True, minimum=1)
    draws36 = _finite(data.get("draws36"), "draws36", integer=True, minimum=1)
    require(len(draws) <= draws36 <= draw_calls, "Actual draw counters inconsistent")
    programs = data.get("program_observations")
    require(type(programs) is list and 1 <= len(programs) <= draws36, "Actual linked program observations unavailable")
    for program in programs:
        program = _object(program, "actual observed program")
        names = program.get("names")
        require(type(names) is list and bool(names) and all(type(name) is str and name.strip() for name in names),
                "Actual active uniform names unavailable")
        # Shadow/depth programs can be reported before ground identification.
        _uniforms(program.get("uniforms"), "program uniforms", ground=False, complete=False)
        _finite(program.get("source_bytes"), "reported JS source code units", integer=True, minimum=1)
        text(program.get("source_start"), "actual source prefix")
        require(type(program.get("contains_finite_helper")) is bool, "Malformed helper-name observation")
    witnesses, previous_time = [], 0
    for draw in draws:
        draw = _object(draw, "actual ground draw")
        require(draw.get("method") in ("drawElements", "drawElementsInstanced"), "Unknown actual draw method")
        require(type(draw.get("count")) is int and draw["count"] == 36 and draw.get("linked") is True and
                draw.get("fragment_compiled") is True and type(draw.get("gl_error")) is int and draw["gl_error"] == 0,
                "Actual compiled/linked ground draw failed")
        timestamp = _finite(draw.get("timestamp_ms"), "actual ground timestamp", minimum=0)
        # Date.now is rounded to milliseconds; performance.now retains fractions.
        require(previous_time <= timestamp <= duration_ms + 1, "Actual ground timestamp outside ordered UTC observation window")
        previous_time = timestamp
        _uniforms(draw.get("uniforms"), "ground uniforms", ground=True)
        record = _object(draw.get("fragment_source"), "actual fragment source identity")
        try:
            raw = referenced_bytes(record.get("path"), record.get("sha256"))
        except OSError as error:
            raise ValueError("Actual linked fragment source file unavailable") from error
        require(type(record.get("bytes")) is int and record["bytes"] == len(raw) and bool(raw),
                "Actual fragment source byte count differs")
        try:
            source = raw.decode("utf-8")
        except UnicodeError as error:
            raise ValueError("Actual linked fragment source is not UTF-8") from error
        require(source.startswith("#version 300 es\n"), "Actual WebGL2 fragment source header differs")
        matching = [p for p in programs if p["source_bytes"] == _js_units(source) and
                    p["source_start"] == _js_prefix(source) and set(p["uniforms"]) == UNIFORMS]
        require(bool(matching), "Ground source metadata not corroborated by observed program diagnostics")
        require(any(p["contains_finite_helper"] == ("classic_lobe_power" in source) for p in matching),
                "Actual helper-name observation disagrees with captured source")
        witnesses.append({"fragment_sha256": record["sha256"], "fragment_bytes": len(raw), **_power_witness(source)})
    return {
        "status": STATUS, "artifact_sha256": artifact_sha256, "ground_draw_count": len(draws),
        "scope": "ACTUAL_LINKED_WEBGL_SOURCE_AND_SUBMITTED_GROUND_UNIFORMS_ONLY",
        "observed_window_ms": duration_ms, "finite_power_witnesses": witnesses,
        "limits": ["Reviewed full-source identity and guard structure are not numeric evidence for every GPU fragment.",
                   "Selected submitted uniforms do not certify the whole effective render pipeline.",
                   "This instrumented observation is not a performance timing window."],
    }
