"""Typed observations of a nonce-bound Windows owned-child launch report.

The native helper requests a desktop and owns one held child handle. Neither
that request nor its terminal receipt certifies pixels or process-tree cleanup.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import re

from tools.build import xp_native_probe as evidence

SCHEMA = "jce.windows.owned-launch.v1"
DESKTOP = "WinSta0\\Default"
MAX_REPORT_BYTES = 64 * 1024
TOP_FIELDS = frozenset(("schema", "nonce", "target_path", "working_dir", "requested_desktop",
                        "controller_error", "child", "streams", "context"))
CHILD_BOOL = frozenset(("created", "birth_known", "birth_stable", "exit_known", "timed_out",
                        "terminate_attempted", "terminate_ok", "reaped", "thread_closed", "process_closed"))
CHILD_UINT = frozenset(("pid", "birth_low", "birth_high", "first_wait", "final_wait", "exit_code"))
STREAM_BOOL = frozenset(("stdout_closed", "stderr_closed", "output_limit_exceeded"))
STREAM_UINT = frozenset(("stdout_bytes", "stderr_bytes", "output_limit_bytes", "output_poll_ms"))
STREAM_FIELDS = STREAM_BOOL | STREAM_UINT | {"stdout_name", "stderr_name", "byte_limit_policy"}


def output_names(nonce):
    if not isinstance(nonce, str) or not re.fullmatch(r"[0-9a-f]{32}", nonce):
        raise ValueError("owned launch requires a bounded lowercase hex nonce")
    return dict(report="jce-launch-" + nonce + ".json",
                stdout="jce-child-" + nonce + ".stdout.bin",
                stderr="jce-child-" + nonce + ".stderr.bin")


def arguments(target, directory, nonce, timeout):
    output_names(nonce)
    return ("--target", target, "--workdir", directory, "--nonce", nonce,
            "--timeout-ms", str(int(timeout * 1000)), "--desktop", DESKTOP)


def _uint(value, name, upper=0xffffffff):
    if type(value) is not int or not 0 <= value <= upper:
        raise ValueError("owned launch " + name + " is not a bounded unsigned integer")


def _entry(collection, name):
    entry = next((item for item in collection.get("files", []) if item.get("name") == name), None)
    if (entry is None or entry.get("verified") is not True
            or entry.get("credential_redacted") is not False
            or entry.get("source_sha256") != entry.get("sha256")
            or entry.get("local_relative") != "collected/" + name):
        raise ValueError("owned launch file is not an exact verified collection")
    return entry


def _stream(out, collection, name, limit):
    entry = _entry(collection, name)
    with (Path(out) / "collected" / name).open("rb") as stream:
        raw = stream.read(limit + 1)
    if (len(raw) > limit or len(raw) != entry["bytes_read"]
            or hashlib.sha256(raw).hexdigest() != entry["source_sha256"]):
        raise ValueError("owned child stream differs from its collected source")
    return raw


def observe(out, collection, *, nonce, target, directory, max_output_bytes):
    """Validate only this held child's report and byte-identical retained streams."""
    names = output_names(nonce)
    report_entry = _entry(collection, names["report"])
    path = Path(out) / "collected" / names["report"]
    raw, _identity = evidence._read(path, MAX_REPORT_BYTES)
    if (len(raw) != report_entry["bytes_read"]
            or hashlib.sha256(raw).hexdigest() != report_entry["source_sha256"]):
        raise ValueError("owned launch report differs from its collected source")
    report = evidence._json(raw)
    if (set(report) != TOP_FIELDS or report["schema"] != SCHEMA or report["nonce"] != nonce
            or report["target_path"] != target or report["working_dir"] != directory
            or report["requested_desktop"] != DESKTOP):
        raise ValueError("owned launch report does not bind this request")
    _uint(report["controller_error"], "controller_error")
    context = report["context"]
    if type(context) is not dict or set(context) != {"user_sid", "token_type", "session_id", "query_error"}:
        raise ValueError("owned launch context fields do not match schema")
    _uint(context["query_error"], "query_error")
    for key in ("token_type", "session_id"):
        if context[key] is not None:
            _uint(context[key], key)
    if context["user_sid"] is not None and (not isinstance(context["user_sid"], str)
            or len(context["user_sid"]) > 256 or not re.fullmatch(r"S-[0-9]+(?:-[0-9]+)+", context["user_sid"])):
        raise ValueError("owned launch user SID is malformed")
    child, streams = report["child"], report["streams"]
    if type(child) is not dict or set(child) != CHILD_BOOL | CHILD_UINT:
        raise ValueError("owned launch child fields do not match schema")
    if type(streams) is not dict or set(streams) != STREAM_FIELDS:
        raise ValueError("owned launch stream fields do not match schema")
    for fields, values in ((CHILD_BOOL, child), (STREAM_BOOL, streams)):
        if any(type(values[key]) is not bool for key in fields):
            raise ValueError("owned launch flags must be booleans")
    for key in CHILD_UINT:
        _uint(child[key], key)
    for key in STREAM_UINT:
        _uint(streams[key], key, (1 << 63) - 1 if key.endswith("_bytes") else 0xffffffff)
    if (streams["stdout_name"] != names["stdout"] or streams["stderr_name"] != names["stderr"]
            or streams["output_limit_bytes"] != 33554432 or streams["output_poll_ms"] != 25
            or streams["byte_limit_policy"] != "observed-periodic-file-size; overshoot possible"):
        raise ValueError("owned launch stream identity or observation policy mismatch")
    birth = (child["birth_high"] << 32) | child["birth_low"]
    identity = dict(pid=child["pid"], creation_time_unix_s=None,
                    creation_time_source="unavailable_remote_psutil",
                    creation_time_windows_filetime=str(birth),
                    creation_time_windows_filetime_low=child["birth_low"],
                    creation_time_windows_filetime_high=child["birth_high"])
    state = "EXITED"
    if not child["created"]:
        state = "OWNED_CHILD_NOT_CREATED"
    elif child["timed_out"]:
        state = "TIMEOUT"
    elif child["exit_known"] and child["exit_code"] != 0:
        state = "NONZERO_EXIT"
    elif streams["output_limit_exceeded"] or streams["stdout_bytes"] + streams["stderr_bytes"] > max_output_bytes:
        state = "OUTPUT_LIMIT"
    elif (not child["pid"] or not birth or not child["birth_known"] or not child["birth_stable"]):
        state = "OWNED_CHILD_IDENTITY_UNVERIFIED"
    elif (child["first_wait"] != 0 or child["final_wait"] != 0 or not child["reaped"]
            or not child["exit_known"] or child["terminate_attempted"] or child["terminate_ok"]):
        state = "OWNED_CHILD_WAIT_UNVERIFIED"
    elif not child["thread_closed"] or not child["process_closed"]:
        state = "OWNED_CHILD_CLEANUP_UNVERIFIED"
    elif not streams["stdout_closed"] or not streams["stderr_closed"]:
        state = "OWNED_CHILD_STREAMS_UNVERIFIED"
    elif report["controller_error"]:
        state = "OWNED_LAUNCH_CONTROLLER_FAILED"
    stdout = stderr = b""
    streams_verified = False
    stream_state = "UNAVAILABLE"
    try:
        stdout_entry, stderr_entry = _entry(collection, names["stdout"]), _entry(collection, names["stderr"])
        if (streams["stdout_bytes"] != stdout_entry["bytes_read"]
                or streams["stderr_bytes"] != stderr_entry["bytes_read"]):
            raise ValueError("owned launch stream sizes disagree with collection")
        if streams["stdout_bytes"] + streams["stderr_bytes"] <= max_output_bytes and not streams["output_limit_exceeded"]:
            stdout = _stream(out, collection, names["stdout"], max_output_bytes)
            stderr = _stream(out, collection, names["stderr"], max_output_bytes)
            streams_verified, stream_state = True, "EXACT_COLLECTED_BYTES"
        else:
            stream_state = "OUTPUT_LIMIT"
    except (ValueError, OSError):
        pass
    if not streams_verified and state == "EXITED":
        state = "OUTPUT_LIMIT" if stream_state == "OUTPUT_LIMIT" else "OWNED_CHILD_STREAMS_UNVERIFIED"
    result = dict(state=state, exit_code=child["exit_code"] if child["exit_known"] else None,
                  stdout=stdout, stderr=stderr, output_truncated=streams["output_limit_exceeded"],
                  process_identity=identity, stderr_path=str(Path(out) / "collected" / names["stderr"]))
    observation = dict(report=report, report_sha256=hashlib.sha256(raw).hexdigest(),
                       report_bytes=len(raw), child_state=state,
                       streams_verified=streams_verified, stream_state=stream_state,
                       child_cleanup_observed=child["created"] and child["reaped"] and child["thread_closed"] and child["process_closed"],
                       actual_child_desktop="UNOBSERVED", process_tree_cleanup="UNKNOWN")
    return result, observation
