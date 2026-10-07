#!/usr/bin/env python3
"""Actual editor mixed full/scoped Undo/Redo; stable tokens, no name identity binding."""
import argparse
import json
import re
from pathlib import Path
from editor_hierarchy_suite import run, scene, sha256_file, assert_backend, validate_display_name
from PIL import Image

def validate(text, operation, count):
    rows = re.findall(r"history-entity-restore: token=(\d+) source_id=(\d+) current_id=(\d+) reason=(undo|redo) name=(\S+) parent=(\d+)", text)
    if len(rows) != 2 or rows[0][3:] != ("undo", "Node0", "0") or rows[1][3:] != ("redo", "Renamed", "0"):
        raise ValueError("actual scoped history results differ")
    if rows[0][0] != rows[1][0] or int(rows[0][0]) == 0:
        raise ValueError("logical object token changed")
    full = re.findall(r"history-scene-restore: reason=(undo|redo) focused=(\d+) token=(\d+) name=(\S+) parent_name=(\S+) population=(\d+)", text)
    if len(full) != 2 or full[0][0] != "undo" or full[1][0] != "redo":
        raise ValueError("actual full-scene traversal missing")
    if full[0][2] != rows[0][0] or full[0][3:] != ("Renamed", "ROOT", str(count)):
        raise ValueError("full undo did not rebind intended object")
    if operation == "reparent" and (full[1][2] != rows[0][0] or full[1][3:] != ("Renamed", "Node1", str(count))):
        raise ValueError("full redo parent/identity differs")
    if operation == "delete" and int(full[1][5]) != count - 1:
        raise ValueError("delete redo population differs")
    return dict(scoped=rows, full=full)

def validate_duplicate_state(before, after):
    def duplicates(document):
        entities = document.get("scene", document)["entities"]
        names = {int(row["id"]): row.get("name", "") for row in entities}
        result = []
        for row in entities:
            if row.get("name") != "Duplicate": continue
            components = row.get("components", [])
            transform = next((part for part in components if part.get("type") == "Transform"), None)
            if transform is None: raise ValueError("actual duplicate transform missing")
            parent = int(row.get("parentId", 0))
            if parent and parent not in names: raise ValueError("actual duplicate parent missing")
            result.append(dict(name="Duplicate", parent=names[parent] if parent else "ROOT", transform=transform))
        return sorted(result, key=lambda row: json.dumps(row, sort_keys=True))
    left, right = duplicates(before), duplicates(after)
    if len(left) != 2 or left != right or left[0] == left[1]:
        raise ValueError("actual duplicate objects/parents/transforms changed or missing")
    return left

def validate_play_stop(text, count):
    if len(re.findall(r"autoplay: entered Play at frame 20", text)) != 1 or len(re.findall(r"play mode stopped", text)) != 1:
        raise ValueError("actual Play/Stop lifecycle missing")
    full = re.findall(r"history-scene-restore: reason=play-stop-restore focused=(\d+) token=(\d+) name=(\S+) parent_name=(\S+) population=(\d+)", text)
    if len(full) != 1 or full[0][2:] != ("Node0", "ROOT", str(count)):
        raise ValueError("PlayStop did not restore original scene")
    rows = re.findall(r"history-entity-restore: token=(\d+) source_id=(\d+) current_id=(\d+) reason=(undo|redo) name=(\S+) parent=(\d+)", text)
    if len(rows) != 2 or rows[0][3:] != ("undo", "Node0", "0") or rows[1][3:] != ("redo", "Renamed", "0"):
        raise ValueError("post-Play scoped history state differs")
    if int(rows[0][0]) == 0 or rows[0][0] != rows[1][0] or rows[0][2] != rows[1][2] or rows[0][2] != full[0][0]:
        raise ValueError("post-Play token or actual identity changed")
    content = re.findall(r"inspector-content-observation: frame=190 entity=(\d+) valid=1 display_name=Renamed", text)
    if content != [rows[0][2]]:
        raise ValueError("post-Play actual Inspector content missing")
    return dict(play_stop=full, scoped=rows, inspector_content_entity=content[0])

def validate_full_first(text, count):
    scoped = re.findall(r"history-entity-restore: token=(\d+) source_id=(\d+) current_id=(\d+) reason=(undo|redo) name=(\S+) parent=(\d+)", text)
    full = re.findall(r"history-scene-restore: reason=(undo|redo) focused=(\d+) token=(\d+) name=(\S+) parent_name=(\S+) population=(\d+)", text)
    if len(scoped) != 2 or len(full) != 2:
        raise ValueError("full-first traversal observations missing")
    if scoped[0][3:5] != ("undo", "Node0") or scoped[1][3:5] != ("redo", "Renamed") or any(int(row[5]) == 0 for row in scoped):
        raise ValueError("scoped traversal lost reparented state")
    token = scoped[0][0]
    if int(token) == 0 or scoped[1][0] != token:
        raise ValueError("scoped logical identity differs")
    if full[0][0] != "undo" or full[0][2:] != ("0", "Node0", "ROOT", str(count)):
        raise ValueError("older untracked full snapshot did not restore")
    if full[1][0] != "redo" or full[1][2:] != (token, "Node0", "Node1", str(count)):
        raise ValueError("inverse full snapshot did not reintroduce scoped identity")
    validate_display_name(text, "Renamed", 180)
    return dict(scoped=scoped, full=full)

def validate_budget_refusal(text, count):
    rows = re.findall(r"history-budget-refusal: unchanged=(\d+) selected_unchanged=(\d+) focused_unchanged=(\d+) population=(\d+) before_hash=(\d+) after_hash=(\d+)", text)
    if len(rows) != 2:
        raise ValueError("expected exactly two pre-clear budget refusals")
    for row in rows:
        if row[:4] != ("1", "1", "1", str(count)) or row[4] != row[5] or int(row[4]) == 0:
            raise ValueError("budget refusal changed actual live state")
    errors = [line for line in text.splitlines() if "] ERROR -" in line]
    allowed = "history identity preparation refused before scene clear"
    if len(errors) != 2 or any(allowed not in line for line in errors):
        raise ValueError("unexpected error in budget negative control")
    commands = re.findall(r"editor-command: operation=reparent entity=(\d+) frame=100 correct=1 parent=(\d+) name=Renamed", text)
    if len(commands) != 1 or int(commands[0][1]) == 0:
        raise ValueError("actual reparent state missing")
    if "history-scene-restore: reason=undo" in text or "history-entity-restore:" in text:
        raise ValueError("refused undo unexpectedly applied history")
    content = re.findall(r"inspector-content-observation: frame=190 entity=(\d+) valid=1 display_name=Renamed", text)
    if len(content) != 1 or content[0] != commands[0][0]:
        raise ValueError("post-refusal actual Inspector content missing or differs")
    return dict(refusals=rows, actual_reparent=commands, inspector_content_entity=content[0])

def validate_process(process, text, backend, capture):
    state = process["process"]
    if state.get("returncode") != 0 or state.get("timed_out"):
        raise ValueError("editor did not exit normally")
    stdout = Path(state["stdout_path"]).read_text(encoding="utf-8", errors="replace")
    actual = assert_backend(text + "\n" + stdout, backend)
    if actual != process.get("backend_actual"):
        raise ValueError("actual backend differs")
    if not capture.is_file():
        raise ValueError("editor capture missing")
    with Image.open(capture) as image:
        if image.format != "PNG" or min(image.size) <= 0:
            raise ValueError("editor capture is not a valid PNG")
        image.verify()
    with Image.open(capture) as image:
        image.load()
    return actual

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--count", type=int, default=10000)
    parser.add_argument("--operation", choices=("reparent", "delete"), required=True)
    parser.add_argument("--backend", default="d3d11")
    parser.add_argument("--duplicate-names", action="store_true", help="actual serialized unrelated duplicate-name state control")
    parser.add_argument("--play-stop", action="store_true", help="Play20/Stop~120 then scoped rename Undo/Redo")
    parser.add_argument("--full-first", action="store_true", help="full reparent before the first scoped rename")
    parser.add_argument("--identity-budget", type=int, help="explicit expected-refusal negative control, 64..67108864 bytes")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.count < 3: parser.error("at least three actual nodes required")
    if args.play_stop and (args.full_first or args.identity_budget is not None):
        parser.error("play-stop cannot combine full-first or budget negative mode")
    if args.full_first and (args.operation != "reparent" or args.identity_budget is not None):
        parser.error("full-first requires ordinary reparent mode")
    if args.identity_budget is not None and (args.operation != "reparent" or not 64 <= args.identity_budget <= 67108864):
        parser.error("budget refusal requires reparent and bounded identity budget")
    out = args.out.resolve(); out.mkdir(parents=True, exist_ok=False)
    source = out / "scene.json"
    document = scene(args.count)
    if args.duplicate_names:
        if args.count < 5: parser.error("duplicate names require at least five nodes")
        for offset, parent in ((-2, 2), (-1, 3)):
            entity = document["entities"][offset]
            entity["name"] = "Duplicate"; entity["parentId"] = parent
            entity["components"][0]["posX"] = 17 if offset == -2 else 29
    source.write_text(json.dumps(document)+"\n", encoding="utf-8", newline="\n")
    env = dict(JCE_SCENE=str(source), JCE_BACKEND=args.backend, JCE_MULTI_INSTANCE="1",
               JCE_DBG_SCENE_LOAD_SYNC="1", JCE_EDITOR_PROFILE_DIR=str(out/"profile"),
               JCE_EDITOR_OPERATION_METRICS="1", JCE_DBG_REPARENT_BATCH="1", JCE_DBG_SELECT="Node0@60",
               JCE_DBG_RENAME="Node0:Renamed@80", JCE_DBG_UNDO="120,140", JCE_DBG_REDO="160,180",
               JCE_WINCAP_FRAME="190", JCE_WINCAP_PATH=str(out/"editor.png"))
    env["JCE_DBG_REPARENT" if args.operation == "reparent" else "JCE_DBG_DELETE"] = "Renamed:Node1@100" if args.operation == "reparent" else "Renamed@100"
    if args.full_first:
        env["JCE_DBG_REPARENT"] = "Node0:Node1@80"
        env["JCE_DBG_RENAME"] = "Node0:Renamed@100"
    if args.identity_budget is not None:
        env["JCE_EDITOR_HISTORY_IDENTITY_BUDGET_BYTES"] = str(args.identity_budget)
        env["JCE_DBG_INSPECTOR_OBSERVE_AT"] = "190"
        env.pop("JCE_DBG_REDO")
    if args.play_stop:
        env.pop("JCE_DBG_REPARENT", None); env.pop("JCE_DBG_DELETE", None)
        env.update(JCE_DBG_SELECT="Node0@10", JCE_DBG_AUTOPLAY="20", JCE_DBG_PLAY_STOP_AT="100",
                   JCE_DBG_RENAME="Node0:Renamed@140", JCE_DBG_UNDO="150", JCE_DBG_REDO="170",
                   JCE_DBG_INSPECTOR_OBSERVE_AT="190")
    if args.duplicate_names:
        env["JCE_DBG_SCENE_OBSERVE_DIR"] = str(out)
        env["JCE_DBG_SCENE_OBSERVE_AT"] = "70,190"
    digest, fixture_hash = sha256_file(args.exe), sha256_file(source)
    process = run(env, out/"editor.log", frames=200, timeout=600, exe=args.exe.resolve(), samples_path=out/"frames.csv")
    text = (out/"editor.log").read_text(encoding="utf-8", errors="replace")
    if args.identity_budget is None and ("] ERROR -" in text or "failed to capture current state" in text or "restore failed" in text):
        raise ValueError("actual history execution failed")
    actual_backend = validate_process(process, text, args.backend, out/"editor.png")
    observed = (validate_play_stop(text, args.count) if args.play_stop else
                validate_budget_refusal(text, args.count) if args.identity_budget is not None else
                validate_full_first(text, args.count) if args.full_first else validate(text, args.operation, args.count))
    if args.duplicate_names:
        for frame in (70, 190):
            if len(re.findall(r"scene-content-observation: frame=" + str(frame) + r" ok=1 bytes=\d+", text)) != 1:
                raise ValueError("actual duplicate scene observation missing")
        observed["duplicate_objects"] = validate_duplicate_state(
            json.loads((out/"scene-70.json").read_text(encoding="utf-8")),
            json.loads((out/"scene-190.json").read_text(encoding="utf-8")))
    if sha256_file(args.exe)!=digest or sha256_file(source)!=fixture_hash: raise ValueError("input changed")
    result = dict(status="EXPECTED_BUDGET_REFUSAL_OBSERVED" if args.identity_budget is not None else "MIXED_HISTORY_OBSERVATIONS", scope="pre-clear refusal preserves live state; not general Undo PASS" if args.identity_budget is not None else __doc__, environment=env,
                  executable_sha256=digest, scene_sha256=fixture_hash, backend_actual=actual_backend, process=process, observed=observed,
                  raw_sha256={str(p.relative_to(out)):sha256_file(p) for p in out.rglob("*") if p.is_file()})
    (out/"result.json").write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8",newline="\n")
if __name__ == "__main__": main()
