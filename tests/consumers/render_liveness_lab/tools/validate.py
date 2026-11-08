"""Independent captured pixels and actual public trace-counter records."""
PHASES = (
    "colour_missing", "colour_restored_without_reset", "colour_recovered",
    "depth_missing", "depth_restored_without_reset", "depth_recovered",
    "visible_off", "visible_on", "cast_off", "cast_on_repeat",
    "scene_depth_on", "scene_depth_off", "shadow_mask_on", "shadow_mask_off",
    "shaded_repeat",
)
COLOUR_PHASES = {2, 3, 4, 5, 7, 8, 9, 10, 12, 13, 14}
SHADOW_PHASES = {5, 7, 9, 10, 12, 14}


def validate_records(records):
    if len(records) != len(PHASES):
        raise ValueError("incomplete actual phase records")
    owners = {row.get("owner") for row in records}
    entities = {row.get("entity") for row in records}
    if len(owners) != 1 or next(iter(owners)) in (None, 0) or len(entities) != 1 or next(iter(entities)) in (None, 0):
        raise ValueError("scene renderer or grass owner changed")
    for index, (name, row) in enumerate(zip(PHASES, records)):
        if row.get("index") != index or row.get("name") != name or row.get("lost") != 0:
            raise ValueError("incorrect phase identity or lost production trace")
        for key in ("accepted", "authored", "generation", "shadow_admissions", "shadow_scopes",
                    "colour_failures", "depth_failures", "colour_program", "depth_program"):
            if type(row.get(key)) is not int or row[key] < 0:
                raise ValueError("missing actual counter: " + name + "/" + key)
        if (row["accepted"] > 0) != (index in COLOUR_PHASES):
            raise ValueError("actual colour submission mismatch: " + name)
        if index in COLOUR_PHASES and (row["authored"] == 0 or row["colour_program"] == 65535):
            raise ValueError("nonzero colour admission unavailable: " + name)
        expected_colour = index in (0, 1)
        expected_depth = index in (3, 4)
        if (row["colour_failures"] > 0) != expected_colour or (row["depth_failures"] > 0) != expected_depth:
            raise ValueError("expected actual missing/recovery reason unavailable: " + name)
        if index in SHADOW_PHASES and (not row["shadow_admissions"] or not row["shadow_scopes"] or row["depth_program"] == 65535):
            raise ValueError("actual shadow source admission unavailable: " + name)
    if records[0]["generation"] != records[1]["generation"] or records[2]["generation"] <= records[1]["generation"]:
        raise ValueError("colour recovery did not follow reset generation")
    if records[3]["generation"] != records[4]["generation"] or records[5]["generation"] <= records[4]["generation"]:
        raise ValueError("depth recovery did not follow reset generation")
    if records[2]["colour_program"] != records[5]["colour_program"]:
        raise ValueError("successful colour program unnecessarily replaced during depth recovery")
    return {"owner": records[0]["owner"], "entity": records[0]["entity"],
            "colour_generations": [records[i]["generation"] for i in (0, 1, 2)],
            "depth_generations": [records[i]["generation"] for i in (3, 4, 5)],
            "accepted_max": max(row["accepted"] for row in records),
            "shadow_submission_scope": "public CPU scope/admission; pixels independently required"}


def rgb(image):
    width, height, channels, data = image
    if (width, height) != (960, 540) or channels not in (3, 4) or len(data) != width * height * channels:
        raise ValueError("unexpected actual image shape")
    return bytes(value for offset in range(0, len(data), channels) for value in data[offset:offset + 3])


def changed(first, second):
    return bytearray(max(abs(first[i+j] - second[i+j]) for j in range(3)) > 2
                     for i in range(0, len(first), 3))


def validate_pixels(images):
    if set(images) != set(PHASES):
        raise ValueError("incomplete actual image controls")
    raw = {name: rgb(value) for name, value in images.items()}
    count = 960 * 540
    def amount(a, b):
        return sum(changed(raw[a], raw[b])) / count
    stable_pairs = (
        ("colour_missing", "colour_restored_without_reset"),
        ("colour_missing", "visible_off"),
        ("colour_recovered", "cast_off"),
        ("depth_missing", "cast_off"),
        ("depth_missing", "depth_restored_without_reset"),
        ("depth_recovered", "visible_on"),
        ("depth_recovered", "cast_on_repeat"),
        ("depth_recovered", "shaded_repeat"),
    )
    repeated = {a + "/" + b: amount(a, b) for a, b in stable_pairs}
    if any(value > .0001 for value in repeated.values()):
        raise ValueError("fixed/repeated/failed-cache image controls disagree")
    geometry = changed(raw["colour_recovered"], raw["visible_off"])
    geometry_count = sum(geometry)
    if geometry_count < 100:
        raise ValueError("grass colour pixels unavailable")
    shadow = changed(raw["cast_off"], raw["depth_recovered"])
    mask_shadow = changed(raw["shadow_mask_off"], raw["shadow_mask_on"])
    off, on = raw["cast_off"], raw["depth_recovered"]
    receiver_darkened = sum(
        shadow[p] and not geometry[p] and sum(off[p*3+j] - on[p*3+j] for j in range(3)) > 6
        for p in range(count))
    mask_receiver = sum(mask_shadow[p] and not geometry[p] for p in range(count))
    depth_count = sum(changed(raw["scene_depth_on"], raw["scene_depth_off"]))
    if receiver_darkened < 10 or mask_receiver < 10 or depth_count < 100:
        raise ValueError("grass receiver shadow/depth pixels unavailable")
    return {"grass_geometry_changed_pixels": geometry_count,
            "receiver_darkened_outside_geometry_pixels": receiver_darkened,
            "shadow_mask_changed_outside_geometry_pixels": mask_receiver,
            "scene_depth_changed_pixels": depth_count, "stable_pairs": repeated,
            "scope": "grass colour/main-depth/receiver shadow liveness; no independent cascade silhouette or GPU timing"}
