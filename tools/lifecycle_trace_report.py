#!/usr/bin/env python3
"""Validate explicit service phases in the resident JCE Chrome trace capture."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path


def _number(value, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
        raise ValueError("invalid " + name)
    return value


def analyze_trace(document, required):
    """CPU scope evidence; never infer GPU completion or missing stages."""
    if not isinstance(document, dict):
        raise ValueError("trace must be an object")
    metadata, events = document.get("jceTrace"), document.get("traceEvents")
    if not isinstance(metadata, dict) or not isinstance(events, list):
        raise ValueError("capture metadata and traceEvents are required")
    if metadata.get("capture_generation_stable") is not True:
        raise ValueError("capture crossed a reset or has unknown generation")
    for name in ("recorded", "captured", "overwritten_before_capture", "cursor_lost", "lost"):
        value = _number(metadata.get(name), name)
        if not isinstance(value, int):
            raise ValueError("capture counts must be integers")
    if metadata["lost"] != metadata["overwritten_before_capture"] + metadata["cursor_lost"]:
        raise ValueError("inconsistent loss accounting")
    if metadata["lost"] or metadata["recorded"] != metadata["captured"]:
        raise ValueError("resident capture is incomplete; retain as diagnostic only")
    if not events:
        raise ValueError("empty capture measured no lifecycle")
    if metadata["captured"] != len(events):
        raise ValueError("captured event count does not match payload")
    active, complete, tasks, completed_services = {}, [], {}, set()
    for event in events:
        if not isinstance(event, dict):
            raise ValueError("invalid trace event")
        phase = event.get("ph")
        if phase not in ("B", "E", "i", "C", "M"):
            raise ValueError("unknown event phase")
        if type(event.get("tid")) is not int or event["tid"] <= 0 or type(event.get("pid")) is not int or event["pid"] != 1:
            raise ValueError("missing or invalid process/thread identity")
        if phase == "M":
            if event.get("name") != "thread_name" or not isinstance(event.get("args"), dict) or not isinstance(event["args"].get("name"), str):
                raise ValueError("unknown metadata event")
            continue
        if event.get("cat") not in {"core", "frame", "thread", "task", "wait", "render", "gpu_scene"}:
            raise ValueError("unknown event category")
        _number(event.get("ts"), "timestamp")
        args = event.get("args")
        if not isinstance(args, dict):
            raise ValueError("event arguments are required")
        if phase == "i" and event["cat"] == "task":
            task = _number(args.get("task"), "submitted task")
            parent = _number(args.get("parent"), "task parent")
            kind = args.get("kind")
            if type(task) is not int or not task or task in tasks or type(parent) is not int or type(kind) is not int or kind not in (0, 1, 2):
                raise ValueError("invalid or repeated submitted task")
            tasks[task] = (kind, event.get("name"), event["ts"])
        if phase not in ("B", "E"):
            continue
        timestamp = _number(event.get("ts"), "timestamp")
        args = event.get("args")
        if not isinstance(args, dict):
            raise ValueError("scope arguments are required")
        identity = _number(args.get("id"), "scope id")
        if not isinstance(identity, int) or not identity:
            raise ValueError("invalid scope identity")
        parent = _number(args.get("parent"), "scope owner")
        if type(parent) is not int:
            raise ValueError("scope owner must be an integer")
        key = (event.get("cat"), event.get("tid"), identity)
        name = event.get("name")
        if not isinstance(name, str) or not name:
            raise ValueError("missing scope name")
        if event["ph"] == "B":
            if key in active:
                raise ValueError("duplicate active scope identity")
            owner = tasks.get(parent, (None, None, None))
            if owner[0] == 2 and (owner[1] != name or owner[2] > timestamp):
                raise ValueError("service submission must precede its scope")
            active[key] = (event, owner)
        else:
            entry = active.pop(key, None)
            start = entry[0] if entry else None
            if start is None or start["name"] != name or start["args"]["parent"] != parent or timestamp < start["ts"]:
                raise ValueError("unpaired or reversed scope: " + name)
            duration_ns = _number(args.get("ns"), "duration")
            if type(duration_ns) is not int:
                raise ValueError("duration must be integer nanoseconds")
            state = args.get("state")
            if event.get("cat") == "task" and (type(state) is not int or state not in (0, 1, 2)):
                raise ValueError("task outcome is missing or invalid")
            kind, task_name, submitted_at = entry[1]
            if kind == 2 and identity == parent:
                if parent in completed_services:
                    raise ValueError("service identity completed more than once")
                completed_services.add(parent)
            if kind == 2 and task_name != name:
                raise ValueError("service scope does not match submitted owner")
            complete.append({"name": name, "thread": event.get("tid"), "id": identity,
                "category": event["cat"], "owner": parent, "task_kind": kind,
                "elapsed_ms": (timestamp - start["ts"]) / 1000,
                "measured_ms": duration_ns / 1000000, "state": state})
    if active:
        raise ValueError("unfinished scopes in capture")
    if not complete:
        raise ValueError("no completed lifecycle scopes")
    required = list(required)
    if not required or len(set(required)) != len(required) or any(not isinstance(x, str) or not x for x in required):
        raise ValueError("unique explicit required phases are needed")
    for name in required:
        matching = [scope for scope in complete if scope["name"] == name and
                    scope["category"] == "task" and scope["task_kind"] == 2 and
                    scope["id"] == scope["owner"]]
        if not matching or any(scope["state"] != 0 for scope in matching):
            raise ValueError("required phase missing or unsuccessful: " + name)
    return {"status": "REQUIRED_CPU_LIFECYCLE_SCOPES_OBSERVED", "required": required,
        "capture": metadata, "scopes": complete,
        "scope": "resident CPU trace only; not GPU completion, whole application coverage or leak certification"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--require", action="append", required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    raw = args.trace.read_bytes()
    try:
        report = analyze_trace(json.loads(raw), args.require)
    except (ValueError, TypeError, KeyError) as error:
        report = {"status": "INVALID_OR_INCOMPLETE_CAPTURE", "error": str(error)}
    report.update(trace=str(args.trace.resolve()), sha256=hashlib.sha256(raw).hexdigest())
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(report["status"])
    return 0 if report["status"] == "REQUIRED_CPU_LIFECYCLE_SCOPES_OBSERVED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
