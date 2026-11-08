"""Audit pinned source balance and public requested authoring; no GPU execution."""
import hashlib
import json
import re

import verify_game as v
from td_render_parameter_audit import (
    ParameterAuditError,
    REFERENCE_COMMIT,
    RENDER_REFERENCE_PATHS,
    audit_requested_rendering,
    load_pinned_sources,
    parse_reference_rendering,
)

TOWER_NAMES = ("BasicTower", "AdvancedTower", "BadAssTower")
TOWER_PATHS = tuple("js/TowerDefense/Towers/TowerDefense." + name + ".js" for name in TOWER_NAMES)
BULLET_PATHS = ("js/TowerDefense/Towers/TowerDefense.Bullet.js",
                "js/TowerDefense/Towers/TowerDefense.RocketBullet.js")
ENEMY_PATHS = ("js/TowerDefense/Enemies/TowerDefense.Enemy.js",
               "js/TowerDefense/Enemies/TowerDefense.UfoEnemy.js")


def number(source, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*:\s*([.\d]+)", source)
    if not match:
        raise ParameterAuditError("Missing original balance field: " + name)
    return float(match[1])


def read_input(path, inputs):
    blob = path.read_bytes()
    inputs[str(path)] = {"sha256": hashlib.sha256(blob).hexdigest(), "bytes": len(blob)}
    return blob.decode("utf-8")


def build_report():
    reference_root = v.REPO / "_workspace/references/phase1_tower_defense"
    manifest = v.PROJECT / "build/reference/manifest.json"
    sources, identity = load_pinned_sources(
        reference_root, manifest,
        RENDER_REFERENCE_PATHS + TOWER_PATHS + BULLET_PATHS + ENEMY_PATHS,
    )
    inputs = {}
    code = read_input(v.PROJECT / "src/rules.c", inputs)
    rows = re.findall(r"\{([^{}]+)\}", code.split("const TdBalance td_balance[3] = {", 1)[1].split("};", 1)[0])
    port = [[float(x.strip().rstrip("f")) for x in row.split(",")] for row in rows]
    if len(port) != len(TOWER_NAMES):
        raise ParameterAuditError("Unexpected port tower balance table")
    report = {
        "reference_commit": REFERENCE_COMMIT,
        "reference_manifest": identity,
        "comparisons": [],
        "source_hashes": {row["path"]: row["sha256"] for row in identity["files"].values()},
        "port_input_hashes": inputs,
    }
    for index, name in enumerate(TOWER_NAMES):
        source = sources[TOWER_PATHS[index]]
        shots = 1 if index == 0 else int(re.search(r"i < (\d+)", source)[1])
        height = number(source.split("this.bulletOffset =", 1)[1], "z")
        expected = [number(source, "costs"), shots,
                    number(sources[BULLET_PATHS[0 if index < 2 else 1]], "damage"),
                    number(source, "speed") / 1000, number(source, "range"), height]
        if port[index] != expected:
            raise ParameterAuditError(f"Tower balance differs: {name}: {port[index]} != {expected}")
        for key, actual, value in zip(("cost", "shots", "damage", "interval_s", "range", "height"),
                                      port[index], expected):
            report["comparisons"].append({"entity": name, "parameter": key,
                                           "original": value, "native": actual, "pass": True})
    expected = [{key: number(sources[path], key) for key in ("hp", "resources", "score", "speed")}
                for path in ENEMY_PATHS]
    if expected != [{"hp": 2, "resources": 1, "score": 100, "speed": .025},
                    {"hp": 4, "resources": 3, "score": 300, "speed": .025}]:
        raise ParameterAuditError("Original enemy balance differs from the audited port")
    for required in ("e->hp = e->kind ? 4 : 2", "g->resources += e->kind ? 3 : 1",
                     "g->score += e->kind ? 300 : 100", "e->progress += 0.025f/(e->length+1)"):
        if required not in code:
            raise ParameterAuditError("Unrecognized port enemy balance expression: " + required)
    report["enemies"] = expected
    report["clock_contract"] = {
        "normal_game": "motion, shooting and waves share fixed 60 Hz accumulated runtime simulation dt; runtime pause and time scale remain authoritative",
        "hitch_policy": "max 0.25 s / 15 steps of supplied simulation dt per update; excess simulation time discarded and reported",
        "original_reference": "legacy per-render update behavior is explicit frozen fixture / diagnostic only",
        "verification": "Compiled rules and actual runtime/native C controller clock targets cover presentation rates, runtime pause/time-scale and host resume; see actual test logs for count and result",
    }
    scene = json.loads(read_input(v.PROJECT / "resources/scenes/td_lab.scene.json", inputs))
    settings = json.loads(read_input(v.PROJECT / ".jce/project-settings.json", inputs))
    rp = json.loads(read_input(v.PROJECT / "Settings/RenderPipeline.rp.json", inputs))
    rp_cooked = json.loads(read_input(v.PROJECT / "resources/Settings/RenderPipeline.rp.json", inputs))
    classic = read_input(v.REPO / "engine/shaders/pbr/fs_classic_lit.sc", inputs)
    report["rendering_requested"] = audit_requested_rendering(
        parse_reference_rendering(sources), scene, settings, rp, rp_cooked, classic,
    )
    report["status"] = "REQUESTED_PARAMETERS_MATCH_WITH_RECORDED_POLICY_DIFFERENCE"
    report["audit_scope"] = (
        "18 tower comparisons and 8 enemy constants; camera/lighting/shadow requests parsed from pinned "
        "original bytes and compared with public scene/settings/RP authoring. MID is JCE request policy, "
        "not an original quality enum. Shadow application differs explicitly; no complete game-state, "
        "effective hardware, submitted-uniform, GPU-output, performance or full-lighting parity certification."
    )
    report["gpu_verification"] = {
        "status": "NOT_RUN_BY_THIS_CPU_AUDIT",
        "scope": "Actual linked shader/uniform and fragment evidence is separately owned and verified.",
    }
    return report


def main():
    v.OUT.mkdir(parents=True, exist_ok=True)
    output = v.OUT / "parameter-audit.json"
    try:
        report = build_report()
    except Exception as error:
        # A failed invocation must not leave a stale PASS at the same output path.
        output.write_text(json.dumps({"status": "FAILED", "error": str(error),
                                      "audit_scope": "REQUESTED_AUTHORING_ONLY", "gpu_verification": "NOT_RUN"},
                                     indent=2) + "\n", encoding="utf-8")
        raise
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    requested = report["rendering_requested"]
    print("PARAMETER AUDIT REQUESTED MATCH", len(report["comparisons"]), "tower comparisons + 8 enemy constants +",
          len(requested["comparisons"]), "source-derived rendering requests +",
          len(requested["port_policy_checks"]), "JCE request-policy checks; shadow policy difference recorded; GPU NOT_RUN")


if __name__ == "__main__":
    main()
