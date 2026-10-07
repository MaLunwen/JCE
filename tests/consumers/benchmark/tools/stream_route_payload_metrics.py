"""Actual CPU-submitted resource payload deltas; never PCIe/GPU-complete bytes."""
import re

CATEGORIES = ("vertex", "index", "texture_create", "texture_update")
_PATTERN = re.compile(
    r"benchmark-stream-payload: frame=(\d+) step=(\d+) phase=(begin|loaded|frozen) "
    r"valid=(\d+) generation=(\d+) renderer_frame=(\d+) vertex=(\d+) index=(\d+) "
    r"texture_create=(\d+) texture_update=(\d+) saturated=(\d+)")


def parse_payload_metrics(log, steps):
    keys = ("frame", "step", "phase", "valid", "generation", "renderer_frame") + CATEGORIES + ("saturated",)
    records = []
    for match in _PATTERN.findall(log):
        records.append({key: value if key == "phase" else int(value)
                        for key, value in zip(keys, match)})
    if len(records) != steps * 3:
        raise ValueError("missing or duplicate actual payload snapshots")
    phases = []
    previous = None
    for step in range(steps):
        current = records[step * 3:step * 3 + 3]
        for record, phase, offset in zip(current, ("begin", "loaded", "frozen"), (0 if step == 0 else 1, 190, 199)):
            if record["step"] != step or record["phase"] != phase or record["frame"] != step * 200 + offset:
                raise ValueError("payload snapshot phase is unaligned")
            if record["valid"] != 1 or not record["generation"] or record["saturated"] != 0:
                raise ValueError("payload snapshot invalid or saturated")
            if previous is not None:
                if record["generation"] != previous["generation"] or record["renderer_frame"] < previous["renderer_frame"]:
                    raise ValueError("payload context changed or frame regressed")
                if any(record[key] < previous[key] for key in CATEGORIES):
                    raise ValueError("payload cumulative counter regressed")
            previous = record
        begin, loaded, frozen = current
        delta = {key: loaded[key] - begin[key] for key in CATEGORIES}
        idle_delta = {key: frozen[key] - loaded[key] for key in CATEGORIES}
        if delta["vertex"] <= 0 or delta["index"] <= 0:
            raise ValueError("actual streamed geometry submission absent")
        if any(idle_delta.values()):
            raise ValueError("frozen phase submitted new resource payload")
        phases.append({"step": step, "load_delta_bytes": delta,
                       "frozen_delta_bytes": idle_delta})
    return {"status": "CPU_SUBMITTED_PAYLOAD_OBSERVATIONS", "records": records,
            "phases": phases,
            "scope": "Actual first-party static mesh/texture API payload; not PCIe traffic or GPU completion. Texture updates count submitted commands."}
