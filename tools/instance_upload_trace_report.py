#!/usr/bin/env python3
"""Validate the captured common renderer CPU instance-owner resource subset."""
from __future__ import annotations
import argparse
import hashlib
import json
from collections import defaultdict
from pathlib import Path
try:
    from .lifecycle_trace_report import analyze_trace
except ImportError:
    from lifecycle_trace_report import analyze_trace

FIELDS = ("owner_observed owner_generation owner_epoch owner_epoch_serial token_generation serial "
          "allocation_serial epoch epoch_serial handle offset requested count actual capacity stride "
          "route_dynamic fallback_reason transient_offset transient_size transient_num transient_stride "
          "encoder_opaque is_main").split()
IMMUTABLE = FIELDS[4:13] + FIELDS[14:22]
ZERO = ("staging_trace_unavailable staging_unavailable_total staging_state_oom_total staging_atomic_oom_total "
        "staging_mutex_oom_total staging_pool_full_total staging_provider_gap_total staging_origin_capture_gap").split()
METRICS = set(("resource_reason resource_capacity_bytes attempted_actual discard_pending staging_transfer_bytes "
               "staging_submit_bytes staging_release_bytes staging_cancel_bytes staging_free_bytes staging_was_transferred "
               "origin_span origin_capture_generation current_capture_generation origin_same_capture "
               "traced_backend_held_bytes staging_lease_count staging_lease_peak backend_held_lease_count "
               "backend_held_lease_peak staging_slot_capacity staging_instrumented_scope generation logical_owned_bytes").split() + ZERO)


def need(condition, message):
    if not condition:
        raise ValueError(message)


def integer(value, name, signed=False):
    need(type(value) is int and (-(1 << 63) <= value < (1 << 64) if signed else 0 <= value < (1 << 64)), "invalid integer " + name)
    return value


def metric(op, name, serial=None, unique=False):
    values = [v for key, value_id, v in op["metrics"] if key == name and (serial is None or serial == value_id)]
    need(bool(values) and (not unique or len(values) == 1), "missing/repeated " + name + " in " + op["name"])
    return values[-1]


def identity_equal(a, b):
    need(all(a[name] == b[name] for name in IMMUTABLE), "immutable token/allocation identity mismatch")


def token_key(packet):
    return packet["token_generation"], packet["serial"]


def allocation_key(packet):
    return packet["token_generation"], packet["allocation_serial"]


def project(document, lifecycle):
    scopes = {s["id"]: dict(s, metrics=[], packets=[]) for s in lifecycle["scopes"]
              if s["name"].startswith("renderer.instance.")}
    need(bool(scopes), "instance-owner path was not observed")
    allowed = {"init", "retire", "destroy", "trim", "reserve", "upload", "bind", "discard", "implicit_cancel", "staging_cpu_free",
               "candidate_create", "retire_enqueue", "handle_destroy", "staging_transfer", "staging_release", "staging_cancel", "staging_summary", "staging_detach"}
    need(all(op["name"].removeprefix("renderer.instance.") in allowed for op in scopes.values()), "unknown instance operation")
    counters = defaultdict(list)
    for index, event in enumerate(document["traceEvents"]):
        if event["ph"] in ("B", "E") and event["args"]["id"] in scopes:
            scopes[event["args"]["id"]][event["ph"]] = index
        if event["ph"] != "C" or not event["name"].startswith("instance."):
            continue
        args = event["args"]
        serial, parent = integer(args.get("id"), "counter id"), integer(args.get("parent"), "counter parent")
        need(parent in scopes, "instance counter has no operation span")
        op = scopes[parent]
        need(event["cat"] == "render" and event["tid"] == op["thread"], "counter thread/category mismatch")
        value = integer(args.get("value"), event["name"], True)
        counters[parent].append((index, event["name"][9:], serial, value))
    records = []
    for scope, op in scopes.items():
        need(op["category"] == "task" and op["task_kind"] == 2 and scope == op["owner"], "instance operation is not a SERVICE span")
        values, pos = counters[scope], 0
        while pos < len(values):
            index, name, serial, value = values[pos]
            need(op["B"] < index < op["E"], "counter outside its active operation")
            if name != "owner_observed":
                need(name not in FIELDS or name in ("requested", "stride", "route_dynamic", "epoch_serial"), "orphan identity field " + name)
                if name in METRICS:
                    need(value >= 0, "negative resource metric " + name)
                    op["metrics"].append((name, serial, value))
                pos += 1
                continue
            packet = {}
            for field in FIELDS:
                need(pos < len(values), "truncated identity counters")
                current, field_name, field_id, field_value = values[pos]
                need(field_name == field and field_id == serial and op["B"] < current < op["E"], "missing/reordered identity " + field)
                packet[field] = integer(field_value, field, field == "encoder_opaque")
                wide = field in ("owner_generation", "owner_epoch_serial", "token_generation", "serial", "allocation_serial", "epoch_serial", "encoder_opaque")
                limit = 65535 if field in ("handle", "stride", "transient_stride") else (1 << 64) - 1 if wide else (1 << 32) - 1
                need(packet[field] <= limit, "out-of-range identity " + field)
                pos += 1
            need(packet["serial"] == serial and packet["owner_observed"] in (0, 1) and packet["is_main"] in (0, 1)
                 and packet["route_dynamic"] in (0, 1), "invalid identity flags/serial")
            record = dict(op=op, packet=packet, index=index)
            op["packets"].append(record); records.append(record)
        for name, _, value in op["metrics"]:
            need(name not in ZERO or value == 0, "resource capture gap: " + name)
        if op["name"] in ("renderer.instance.reserve", "renderer.instance.upload", "renderer.instance.bind") and not op["packets"]:
            need(op["name"].endswith("upload") and op["state"] == 2 and any(s["submitted_parent"] == scope and s["name"].endswith("discard") for s in scopes.values()), "missing operation identity")
    return scopes, sorted(records, key=lambda r: r["index"])


def valid_origin(op, serial, commits):
    origin = metric(op, "origin_span", serial, True)
    need(origin in commits and op["submitted_parent"] == origin, "staging has wrong commit origin")
    need(metric(op, "origin_same_capture", serial, True) == 1 and metric(op, "origin_capture_generation", serial, True)
         == metric(op, "current_capture_generation", serial, True), "staging crossed capture")
    return origin


def staging_snapshot(op, serial):
    names = ("traced_backend_held_bytes staging_lease_count staging_lease_peak backend_held_lease_count "
             "backend_held_lease_peak staging_slot_capacity staging_instrumented_scope").split()
    values = {name: metric(op, name, serial) for name in names}
    for name in ZERO[1:7]: metric(op, name, serial)
    need(values["staging_instrumented_scope"] == 1 and values["staging_slot_capacity"] == 256
         and values["backend_held_lease_count"] <= values["staging_lease_count"] <= values["staging_lease_peak"] <= 256
         and values["backend_held_lease_count"] <= values["backend_held_lease_peak"] <= values["staging_lease_peak"], "invalid bounded staging snapshot")
    return values


def analyze_instance_upload_trace(document, require_dynamic=False):
    lifecycle = analyze_trace(document, ["renderer.instance.destroy"])
    scopes, records = project(document, lifecycle)
    tokens, resources, stages, failed_creates, frees = {}, {}, {}, {}, defaultdict(int)
    current_handles, summaries, destroyed_generations = {}, defaultdict(list), set()
    logical_bytes, peak_logical_bytes = defaultdict(int), 0
    intervals = defaultdict(list)
    binds = cancelled = diagnostics = retired = destroyed = 0
    commits = {op["id"]: op for op in scopes.values() if op["name"].endswith(".upload")}
    for op in scopes.values():
        if op["name"].endswith(".destroy"):
            generation = metric(op, "generation")
            need(generation > 0 and metric(op, "logical_owned_bytes") == 0, "owner teardown lacks logical closure")
            destroyed_generations.add(generation)
    # Resource/staging events may precede the enclosing successful commit packet.
    accepted = {token_key(r["packet"]): r for r in records if r["op"]["name"].endswith(".upload") and r["op"]["state"] == 0}
    for record in records:
        op, packet, index = record["op"], record["packet"], record["index"]
        name, state, key = op["name"].removeprefix("renderer.instance."), op["state"], token_key(packet)
        if name in ("staging_summary", "staging_detach"):
            need(packet["serial"] == 0 and metric(op, "staging_unavailable_total", 0) == 0
                 and metric(op, "staging_instrumented_scope", 0) == 1, "invalid staging summary")
            for counter in ZERO[1:7]: metric(op, counter, 0)
            if name == "staging_detach" or any(n == "origin_span" for n, _, _ in op["metrics"]): valid_origin(op, 0, commits)
            if name == "staging_summary": summaries[key[0]].append(record)
            continue
        if name in ("reserve", "upload", "bind") and state != 0:
            need(state == 1 or name == "upload", "invalid rejected operation outcome")
            if name == "upload" and state == 1:
                attempted = metric(op, "attempted_actual", packet["serial"])
                token = tokens.get(key)
                if token and not token["committed"] and not token["cancelled"] and packet["route_dynamic"] and 0 < attempted <= packet["count"]:
                    need(key in stages and stages[key].get("cancelled") and stages[key]["origin"] == op["id"], "failed update lacks metadata cancellation")
            diagnostics += 1
            continue
        if name in ("reserve", "upload", "bind", "candidate_create", "retire_enqueue", "handle_destroy", "staging_transfer", "staging_release", "staging_cancel", "staging_cpu_free", "implicit_cancel", "discard") and packet["serial"]:
            need(packet["token_generation"] > 0 and packet["allocation_serial"] > 0 and packet["stride"] in (32, 64, 80), "invalid resource identity")
        if name == "reserve":
            need(key[0] > 0 and key[1] > 0 and packet["allocation_serial"] > 0 and key not in tokens and packet["actual"] == 0 and 0 < packet["count"] <= packet["requested"], "invalid/repeated reserve")
            need(packet["handle"] < 65535 and packet["offset"] + packet["count"] <= packet["capacity"], "reservation capacity/handle mismatch")
            need(packet["owner_observed"] == 1 and packet["owner_generation"] == key[0]
                 and packet["owner_epoch"] == packet["epoch"] and packet["owner_epoch_serial"] == packet["epoch_serial"], "stale reserve identity")
            if packet["route_dynamic"]: need(current_handles.get((key[0], packet["handle"])) == allocation_key(packet), "reserve references absent/destroyed allocation")
            else: need(packet["count"] <= packet["transient_num"] and packet["count"] * packet["stride"] <= packet["transient_size"] and packet["transient_stride"] == packet["stride"], "invalid transient slice")
            tokens[key] = dict(record=record, committed=None, cancelled=False, binds=0)
        elif name in ("upload", "bind"):
            need(key in tokens, "commit/bind has no prior reserve")
            token = tokens[key]; identity_equal(token["record"]["packet"], packet)
            if packet["route_dynamic"]: need(current_handles.get((key[0], packet["handle"])) == allocation_key(packet), "upload/bind references destroyed allocation")
            need(not token["cancelled"] and packet["owner_observed"] == 1 and packet["owner_generation"] == key[0]
                 and packet["owner_epoch"] == packet["epoch"] and packet["owner_epoch_serial"] == packet["epoch_serial"], "stale/cancelled admitted token")
            if name == "upload":
                need(token["committed"] is None and 0 < packet["actual"] <= packet["count"], "invalid/duplicate commit")
                token["committed"] = record
                if packet["route_dynamic"]: intervals[allocation_key(packet) + (packet["epoch_serial"],)].append((packet["offset"], packet["offset"] + packet["actual"]))
            else:
                need(token["committed"] is not None and 0 < packet["actual"] <= token["committed"]["packet"]["actual"], "bind precedes/exceeds commit")
                token["binds"] += 1; binds += 1
        elif name in ("discard", "implicit_cancel"):
            if name == "discard" and any(n == "discard_pending" and v == 0 for n, _, v in op["metrics"]):
                need(state == 2, "invalid alias discard outcome")
                diagnostics += 1; continue
            need(state == 2 and packet["actual"] == 0 and key in tokens and tokens[key]["committed"] is None and not tokens[key]["cancelled"], "orphan/duplicate token cancellation")
            identity_equal(tokens[key]["record"]["packet"], packet)
            tokens[key]["cancelled"] = True; cancelled += 1
            tokens[key]["cancel_scope"] = op["id"]
            tokens[key]["cancel_reason"] = metric(op, "resource_reason", packet["serial"], True) if name == "implicit_cancel" else 4
            need(tokens[key]["cancel_reason"] in (1, 3, 4), "invalid pending cancellation reason")
        elif name in ("candidate_create", "retire_enqueue", "handle_destroy"):
            akey = allocation_key(packet)
            need(key[0] > 0 and key[1] > 0 and packet["allocation_serial"] > 0 and packet["route_dynamic"] == 1 and metric(op, "resource_capacity_bytes", packet["serial"], True) == packet["capacity"] * packet["stride"], "invalid dynamic capacity evidence")
            reason = metric(op, "resource_reason", packet["serial"], True)
            if name == "candidate_create" and state == 1:
                need(reason == 6 and packet["handle"] == 65535 and akey not in resources, "failed create published resource")
                failed_creates[key] = record; diagnostics += 1; continue
            need(state == 0 and packet["handle"] < 65535, "unsuccessful resource operation")
            hkey = key[0], packet["handle"]
            if name == "candidate_create":
                need(reason == 0, "invalid creation reason")
                need(akey not in resources and hkey not in current_handles, "duplicate live allocation/handle")
                resources[akey] = dict(record=record, retired=False, destroy=None)
                current_handles[hkey] = akey
                logical_bytes[key[0]] += packet["capacity"] * packet["stride"]
                need(0 < logical_bytes[key[0]] <= 128 * 1048576, "shared logical dynamic capacity exceeded")
                peak_logical_bytes = max(peak_logical_bytes, logical_bytes[key[0]])
            else:
                need(akey in resources and resources[akey]["destroy"] is None, "unknown/double resource destruction")
                resource = resources[akey]; identity_equal(resource["record"]["packet"], packet)
                if name == "retire_enqueue":
                    need(reason == 0 and not resource["retired"], "double/invalid retirement")
                    resource["retired"] = True; retired += 1
                else:
                    need(reason in (1, 2, 3, 4), "unknown destruction reason")
                    if reason == 1 and not resource["retired"]:
                        pending = tokens.get(token_key(resource["record"]["packet"]))
                        need(pending and pending["cancelled"] and pending["committed"] is None and pending["cancel_reason"] == 1
                             and pending["cancel_scope"] == op["submitted_parent"], "unretired allocation lacks matching pending advance cancellation")
                    resource["destroy"] = index; del current_handles[hkey]; destroyed += 1
                    logical_bytes[key[0]] -= packet["capacity"] * packet["stride"]
        elif name in ("staging_transfer", "staging_release", "staging_cancel"):
            need(key in tokens and packet["route_dynamic"] == 1 and 0 < packet["actual"] <= packet["count"], "orphan/invalid staging event")
            identity_equal(tokens[key]["record"]["packet"], packet)
            origin = valid_origin(op, packet["serial"], commits)
            kind = name.removeprefix("staging_"); byte_count = metric(op, "staging_" + kind + "_bytes", packet["serial"], True)
            need(byte_count == packet["count"] * packet["stride"], "retained staging bytes mismatch")
            staging_snapshot(op, packet["serial"])
            if kind == "transfer":
                need(state == 0 and key not in stages and key in accepted and accepted[key]["op"]["id"] == origin, "orphan/duplicate staging transfer")
                need(packet["actual"] == accepted[key]["packet"]["actual"] and metric(op, "staging_submit_bytes", packet["serial"], True) == packet["actual"] * packet["stride"], "submitted staging bytes/count mismatch")
                stages[key] = dict(bytes=byte_count, index=index, closed=None, packet=packet, origin=origin)
            elif kind == "release":
                need(state == 0 and key in stages and stages[key]["closed"] is None and stages[key]["index"] < index
                     and metric(op, "staging_was_transferred", packet["serial"], True) == 1, "orphan/double/reversed backend release")
                need(packet["actual"] == stages[key]["packet"]["actual"] and stages[key]["origin"] == origin, "release actual/origin mismatch")
                stages[key]["closed"] = index
            else:
                need(state == 2 and key not in stages and commits[origin]["state"] == 1
                     and metric(op, "staging_was_transferred", packet["serial"], True) == 0
                     and metric(commits[origin], "attempted_actual", packet["serial"]) == packet["actual"], "invalid staging rollback")
                stages[key] = dict(bytes=byte_count, index=index, closed=index, cancelled=True, origin=origin)
        elif name == "staging_cpu_free":
            expected = failed_creates.get(key) or (tokens.get(key) or {}).get("record")
            need(state == 2 and expected is not None and packet["route_dynamic"] == 1, "orphan/invalid CPU staging free")
            identity_equal(expected["packet"], packet)
            need(metric(op, "staging_free_bytes", packet["serial"], True) == packet["count"] * packet["stride"], "CPU free bytes mismatch")
            frees[key] += 1; need(frees[key] == 1, "double CPU staging free")
    for spans in intervals.values():
        ordered = sorted(spans)
        need(all(left[1] <= right[0] for left, right in zip(ordered, ordered[1:])), "overlapping committed instance intervals")
    dynamic = 0
    for key, token in tokens.items():
        p, committed = token["record"]["packet"], token["committed"]
        need(committed is not None or token["cancelled"], "unclosed reservation")
        if p["route_dynamic"]:
            resource = resources.get(allocation_key(p))
            need(resource is not None and resource["record"]["index"] < token["record"]["index"], "dynamic token lacks allocation")
            if committed:
                dynamic += 1; need(key in stages and "cancelled" not in stages[key] and stages[key]["closed"] is not None and frees[key] == 0, "dynamic commit lacks closed backend staging")
            else: need(frees[key] == 1, "cancelled dynamic reserve lacks CPU free")
    for key in failed_creates: need(frees[key] == 1, "failed creation lacks staging disposal")
    for resource in resources.values():
        creator = resource["record"]["packet"]
        need(token_key(creator) in tokens, "allocation has no successful reserve")
        identity_equal(creator, tokens[token_key(creator)]["record"]["packet"])
        need(resource["destroy"] is not None, "resource missing destroy request")
    for generation in {key[0] for key in tokens} | {key[0] for key in resources} | destroyed_generations:
        need(generation in destroyed_generations and len(summaries[generation]) == 1, "missing/duplicate owner summary")
        if any(k[0] == generation for k in stages):
            summary = summaries[generation][0]
            staging_snapshot(summary["op"], 0)
            for name in ("traced_backend_held_bytes", "backend_held_lease_count", "staging_lease_count"):
                need(metric(summary["op"], name, 0, True) == 0, "nonzero final " + name)
            need(metric(summary["op"], "origin_same_capture", 0, True) == 1 and summary["index"] > max(s["closed"] for k, s in stages.items() if k[0] == generation), "premature/cross-capture terminal summary")
    need(not require_dynamic or dynamic > 0, "dynamic uploads NOT_EXERCISED")
    return {"status": "COMPLETE_CAPTURED_INSTANCE_OWNER_SUBSET" if tokens else "INSTANCE_OWNER_UPLOADS_NOT_EXERCISED", "dynamic": "EXERCISED_CLOSED" if dynamic else "NOT_EXERCISED",
            "tokens": {"reserved": len(tokens), "committed": sum(t["committed"] is not None for t in tokens.values()), "cancelled": cancelled,
                       "binds": binds, "committed_unbound": sum(t["committed"] is not None and not t["binds"] for t in tokens.values())},
            "resources": {"created": len(resources), "retired": retired, "destroy_requested": destroyed, "peak_logical_capacity_bytes": peak_logical_bytes},
            "staging": {"transferred": sum("cancelled" not in s for s in stages.values()), "backend_released": sum(s["closed"] is not None and "cancelled" not in s for s in stages.values()),
                        "metadata_cancelled": sum("cancelled" in s for s in stages.values()),
                        "cpu_freed": sum(frees.values()), "retained_transfer_bytes": sum(s["bytes"] for s in stages.values() if "cancelled" not in s)},
            "diagnostic_attempts": diagnostics, "capture": lifecycle["capture"],
            "scope": "common renderer CPU instance owner only; excludes raw worker transient routes, GPU physical memory and GPU completion"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path); parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--require-dynamic", action="store_true"); args = parser.parse_args(); raw = None
    try:
        raw = args.trace.read_bytes()
        report = analyze_instance_upload_trace(json.loads(raw), args.require_dynamic)
    except (ValueError, TypeError, KeyError, OverflowError, OSError) as error:
        report = {"status": "INVALID_OR_INCOMPLETE_INSTANCE_CAPTURE", "error": str(error)}
    report.update(trace=str(args.trace.resolve()), sha256=hashlib.sha256(raw).hexdigest() if raw is not None else None)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(report["status"])
    return 0 if report["status"] == "COMPLETE_CAPTURED_INSTANCE_OWNER_SUBSET" else 2 if report["status"] == "INSTANCE_OWNER_UPLOADS_NOT_EXERCISED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
