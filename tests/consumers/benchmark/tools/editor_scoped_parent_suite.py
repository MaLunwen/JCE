#!/usr/bin/env python3
"""Actual scoped parent Undo/Redo state; no stable sibling-order claim."""
import argparse
import json
import re
from pathlib import Path
from editor_hierarchy_suite import scene, run, sha256_file, hierarchy_cache_environment, validate_cache_observations, compare_cache_controls
from editor_mixed_history_suite import validate_process, validate_duplicate_state


def validate_states(documents, count):
    if len(documents) != 4: raise ValueError("four actual state snapshots required")
    def extract(document):
        rows = document.get("scene", document)["entities"]
        if len(rows) != count: raise ValueError("actual scene population differs")
        names = {int(row["id"]): row["name"] for row in rows}
        if len(names) != count or any(identity <= 0 for identity in names):
            raise ValueError("actual entity identities duplicate or invalid")
        def entity(name):
            matches = [row for row in rows if row["name"] == name]
            if len(matches) != 1: raise ValueError("actual target identity missing")
            row = matches[0]
            transform = next((part for part in row["components"] if part["type"] == "Transform"), None)
            if transform is None: raise ValueError("actual local Transform missing")
            parent = int(row.get("parentId", 0))
            if parent and parent not in names: raise ValueError("actual parent identity missing")
            return dict(id=int(row["id"]), parent=names[parent] if parent else "ROOT", parent_id=parent, transform=transform)
        return {name: entity(name) for name in ("Node0", "Node1", "Node2")}
    states = [extract(document) for document in documents]
    if [state["Node0"]["parent"] for state in states] != ["ROOT", "Node1", "ROOT", "Node1"]:
        raise ValueError("actual parent Undo/Redo sequence differs")
    if len({state["Node0"]["id"] for state in states}) != 1:
        raise ValueError("scoped parent operation recreated target identity")
    if states[0]["Node0"]["transform"] != states[2]["Node0"]["transform"] or states[1]["Node0"]["transform"] != states[3]["Node0"]["transform"]:
        raise ValueError("actual local Transform round trip differs")
    for state in states[1:]:
        if state["Node1"] != states[0]["Node1"] or state["Node2"] != states[0]["Node2"]:
            raise ValueError("parent/descendant local state or identity changed")
    original = {int(row["id"]): row for row in documents[0].get("scene", documents[0])["entities"]}
    target_id = states[0]["Node0"]["id"]
    for document in documents[1:]:
        validate_duplicate_state(documents[0], document)
        current = {int(row["id"]): row for row in document.get("scene", document)["entities"]}
        if current.keys() != original.keys(): raise ValueError("unrelated entity identity changed")
        if any(current[identity] != row for identity, row in original.items() if identity != target_id):
            raise ValueError("unrelated serialized entity state changed")
    return states


def validate_events(text):
    commands = re.findall(r"editor-command: operation=reparent entity=(\d+) frame=80 correct=1 parent=(\d+) name=Node0", text)
    if len(commands) != 1 or int(commands[0][1]) == 0: raise ValueError("actual reparent command missing")
    entity, parent = commands[0]
    rows = re.findall(r"history-entity-restore: token=(\d+) source_id=(\d+) current_id=(\d+) reason=(undo|redo) name=Node0 parent=(\d+)",text)
    if len(rows) != 2 or rows[0][3:] != ("undo", "0") or rows[1][3:] != ("redo", parent):
        raise ValueError("actual scoped history relation sequence missing")
    if rows[0][0] != rows[1][0] or int(rows[0][0]) == 0 or any(row[1:3] != (entity, entity) for row in rows):
        raise ValueError("actual scoped history identity sequence differs")
    content = re.findall(r"inspector-content-observation: frame=150 entity=(\d+) valid=1 display_name=Node0", text)
    if content != [entity]: raise ValueError("actual Inspector content missing or repeated")
    return dict(entity=int(entity), parent=int(parent), scoped=[list(row) for row in rows])

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--count", type=int, default=10000)
    parser.add_argument("--backend", default="d3d11")
    parser.add_argument("--disable-hierarchy-cache", action="store_true")
    parser.add_argument("--compare-with", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.count < 5: parser.error("at least five nodes required")
    args.out = args.out.resolve(); args.out.mkdir(parents=True, exist_ok=False)
    fixture = scene(args.count)
    fixture["entities"][1]["components"][0].update(posX=10, posY=5, posZ=2, scaleX=2, scaleY=2, scaleZ=2)
    fixture["entities"][2]["parentId"] = 1
    fixture["entities"][2]["components"][0]["posY"] = 1
    for offset, x in ((-2, 17), (-1, 29)):
        fixture["entities"][offset]["name"] = "Duplicate"
        fixture["entities"][offset]["components"][0]["posX"] = x
    source = args.out/"scene.json"; source.write_text(json.dumps(fixture)+"\n",encoding="utf-8",newline="\n")
    digest, source_hash = sha256_file(args.exe), sha256_file(source)
    env = dict(JCE_SCENE=str(source), JCE_BACKEND=args.backend, JCE_MULTI_INSTANCE="1",
        JCE_DBG_SCENE_LOAD_SYNC="1", JCE_EDITOR_PROFILE_DIR=str(args.out/"profile"),
        JCE_EDITOR_OPERATION_METRICS="1", JCE_DBG_REPARENT_BATCH="0", JCE_DBG_SELECT="Node0@60", JCE_DBG_REPARENT="Node0:Node1@80",
        JCE_DBG_UNDO="100", JCE_DBG_REDO="120", JCE_DBG_SCENE_OBSERVE_DIR=str(args.out),
        JCE_DBG_SCENE_OBSERVE_AT="70,90,110,130", JCE_DBG_INSPECTOR_OBSERVE_AT="150",
        JCE_WINCAP_FRAME="150", JCE_WINCAP_PATH=str(args.out/"editor.png"))
    env.update(hierarchy_cache_environment(args.disable_hierarchy_cache))
    process = run(env,args.out/"editor.log",frames=200,timeout=600,exe=args.exe.resolve(),samples_path=args.out/"frames.csv")
    text = (args.out/"editor.log").read_text(encoding="utf-8",errors="replace")
    backend = validate_process(process,text,args.backend,args.out/"editor.png")
    if "] ERROR -" in text or "history-scene-restore:" in text: raise ValueError("actual scoped history failed or used full restore")
    cache_observations = validate_cache_observations(text, args.disable_hierarchy_cache)
    events = validate_events(text)
    for frame in (70,90,110,130):
        if len(re.findall(r"scene-content-observation: frame="+str(frame)+r" ok=1 bytes=\d+",text))!=1: raise ValueError("actual serialized state observation missing")
    observations = validate_states([json.loads((args.out/("scene-%u.json"%frame)).read_text()) for frame in (70,90,110,130)],args.count)
    # Serializer retains full ECS handles; the editor public ID facade uses the low32 index.
    if (observations[0]["Node0"]["id"] & 0xffffffff) != events["entity"] or (observations[1]["Node1"]["id"] & 0xffffffff) != events["parent"]:
        raise ValueError("serialized target/parent differs from actual command")
    if sha256_file(args.exe)!=digest or sha256_file(source)!=source_hash: raise ValueError("input changed")
    result=dict(status="SCOPED_PARENT_STATE_OBSERVATIONS",scope=__doc__,environment=env,backend_actual=backend,
        executable_sha256=digest,scene_sha256=source_hash,process=process,observations=observations,events=events,hierarchy_cache=cache_observations,
        world_transform_scope="native production-helper regression; serializer observer proves local components/relations only",
        raw_sha256={str(path.relative_to(args.out)):sha256_file(path) for path in args.out.rglob("*") if path.is_file()})
    if args.compare_with:
        result["cache_comparison"] = compare_cache_controls(result,args.compare_with.resolve())
        for frame in (70,90,110,130):
            previous = json.loads((args.compare_with.resolve().parent/("scene-%u.json"%frame)).read_text())
            current = json.loads((args.out/("scene-%u.json"%frame)).read_text())
            if current != previous: raise ValueError("cache changes actual complete scene snapshot")
    (args.out/"result.json").write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8",newline="\n")

if __name__ == "__main__": main()
