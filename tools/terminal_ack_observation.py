"""Opt-in host-tail acknowledgement tied to owned stderr and CPU trace evidence.

This observes a host's bounded terminal claim after Engine cleanup returns. It
does not prove OS process teardown, GPU retirement, file durability or absence
of uninstrumented owners. The process sampler remains engine-neutral.
"""
from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path
import re

from tools import lifecycle_trace_report as lifecycle

PREFIX = b"JCE_TERMINAL_ACK "
TOKEN = re.compile(r"[0-9a-f]{32}\Z")
DECIMAL = re.compile(r"(?:0|[1-9][0-9]{0,19})\Z")
FIELDS = frozenset(("schema", "token", "host", "session", "cleanup_scope", "app_result",
    "engine_tail_complete", "trace_status", "trace_published", "trace_write_ok",
    "logger_shutdown_returned", "log_file_close"))
MAX_ACK_LINE = 4096
TRACE_BOOL_OPTIONS = frozenset(("resource_observations", "renderer_submissions", "gpu_view_observations",
    "screenshot_requests", "offscreen_configurations", "filesystem_read_details", "shader_provenance", "scope_witnesses",
    "render_pipeline_observations", "gpu_submission_joins"))
TRACE_NAME_OPTIONS = frozenset(("required", "require_wait", "require_counter_zero", "require_counter_last_zero"))


def terminal_trace_history_budget(value, *, trace_requested):
    """Resolve only an explicit trace-history budget; keep all other limits."""
    if value is None:
        return 200000
    if type(value) is not int or not 1 <= value <= 1000000:
        raise ValueError("terminal_trace_max_history must be an integer in 1..1000000")
    if not trace_requested:
        raise ValueError("terminal_trace_max_history requires terminal acknowledgement and a requested continuous trace")
    return value


def requested_trace_path(environment, cwd):
    """Validate explicit trace intent before a run can erase any old output."""
    stream = environment.get("JCE_TRACE_STREAM", "")
    trace = environment.get("JCE_TRACE", "")
    export = environment.get("JCE_TRACE_EXPORT", "")
    if any(not isinstance(value, str) for value in (stream, trace, export)):
        raise ValueError("terminal ack trace options must be strings")
    requested = bool(stream or export or (trace and not trace.startswith("0")))
    if not requested:
        return None
    if not stream:
        raise ValueError("terminal ack trace validation requires a fresh JCE_TRACE_STREAM path")
    path = Path(stream)
    if not path.is_absolute():
        path = Path(cwd) / path
    # A dangling link must not silently become an output reservation either.
    if path.exists() or path.is_symlink():
        raise ValueError("terminal ack trace path already exists; preserve prior evidence")
    return path.resolve()


def _decimal(value, name, *, positive=False, upper=(1 << 64) - 1):
    if not isinstance(value, str) or not DECIMAL.fullmatch(value):
        raise ValueError("terminal ack " + name + " must be a canonical decimal string")
    number = int(value)
    if number > upper or (positive and not number):
        raise ValueError("terminal ack " + name + " is out of range")
    return number


def _packet(raw, token):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("duplicate terminal ack JSON field")
            result[key] = value
        return result
    def constant(_value):
        raise ValueError("nonfinite terminal ack JSON value")
    try:
        packet = json.loads(raw.decode("utf-8", errors="strict"),
                            object_pairs_hook=unique, parse_constant=constant)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError("malformed terminal ack JSON") from error
    if not isinstance(packet, dict) or set(packet) != FIELDS:
        raise ValueError("terminal ack fields do not match schema 1")
    if type(packet["schema"]) is not int or packet["schema"] != 1:
        raise ValueError("unsupported terminal ack schema")
    if packet["token"] != token:
        raise ValueError("terminal ack token does not belong to this run")
    if packet["host"] not in ("sdl", "jni"):
        raise ValueError("unknown terminal ack host")
    _decimal(packet["session"], "session", positive=True, upper=(1 << 63) - 1)
    scope = _decimal(packet["cleanup_scope"], "cleanup_scope")
    if type(packet["app_result"]) is not int or packet["app_result"] != 1:
        raise ValueError("terminal ack application did not succeed")
    for name in ("engine_tail_complete", "logger_shutdown_returned"):
        if packet[name] is not True:
            raise ValueError("terminal ack " + name + " was not confirmed")
    for name in ("trace_published", "trace_write_ok"):
        if type(packet[name]) is not bool:
            raise ValueError("terminal ack trace status must use boolean fields")
    status = packet["trace_status"]
    if status == "complete":
        if not scope or packet["trace_published"] is not True or packet["trace_write_ok"] is not True:
            raise ValueError("terminal ack complete trace fields disagree")
    elif status == "unavailable":
        if packet["trace_published"] or packet["trace_write_ok"]:
            raise ValueError("terminal ack unavailable trace fields disagree")
    else:
        raise ValueError("terminal ack trace is incomplete or unsupported")
    if packet["log_file_close"] not in ("unobserved", "no_file", "api_ok"):
        raise ValueError("terminal ack log close failed or is unsupported")
    return packet


def _stderr_packet(path, token):
    """Bounded line reads; no merge, ANSI rewrite, deduplication or text repair."""
    packet, digest, size, line_number, witness_line = None, hashlib.sha256(), 0, 0, None
    with path.open("rb") as stream:
        initial = lifecycle._fingerprint(os.fstat(stream.fileno()))
        initial_path = lifecycle._fingerprint(path.stat())
        continuation = False
        for raw in iter(lambda: stream.readline(MAX_ACK_LINE + 1), b""):
            digest.update(raw); size += len(raw)
            if not continuation:
                line_number += 1
                if raw.startswith(PREFIX.rstrip()):
                    if not raw.startswith(PREFIX) or len(raw) > MAX_ACK_LINE or not raw.endswith(b"\n"):
                        raise ValueError("truncated or malformed terminal ack line")
                    if packet is not None:
                        raise ValueError("duplicate terminal ack line")
                    payload = raw[len(PREFIX):-1]
                    if payload.endswith(b"\r"):
                        payload = payload[:-1]
                    packet = _packet(payload, token)
                    witness_line = line_number
            continuation = not raw.endswith(b"\n")
        if (initial[:4] != initial_path[:4] or lifecycle._fingerprint(os.fstat(stream.fileno())) != initial or
                lifecycle._fingerprint(path.stat()) != initial_path):
            raise ValueError("terminal ack stderr changed during observation")
    if packet is None:
        raise ValueError("missing terminal ack in owned stderr")
    return packet, dict(path=str(path), sha256=digest.hexdigest(), bytes=size, line=witness_line)


def _analysis_options(options, *, trace_requested, return_trace_report):
    if type(return_trace_report) is not bool:
        raise ValueError("return_trace_report must be an explicit bool")
    if not trace_requested and (options is not None or return_trace_report):
        raise ValueError("trace_analysis_options/return_trace_report require a requested trace")
    if options is not None and type(options) is not dict:
        raise ValueError("trace_analysis_options must be an explicit options dict")
    result = {}
    for name, value in (options or {}).items():
        if name in TRACE_BOOL_OPTIONS:
            if type(value) is not bool:
                raise ValueError("trace_analysis_options " + name + " must be an explicit bool")
            result[name] = value
        elif name in TRACE_NAME_OPTIONS:
            if type(value) not in (list, tuple):
                raise ValueError("trace_analysis_options " + name + " must be an explicit names list/tuple")
            result[name] = lifecycle._names(value, "trace_analysis_options requires unique nonempty names")
        elif name == "scope_prefixes":
            if type(value) not in (list, tuple):
                raise ValueError("trace_analysis_options scope_prefixes must be an explicit list/tuple")
            result[name] = lifecycle._scope_prefixes(value)
        else:
            raise ValueError("unsupported trace_analysis_options field: " + str(name))
    required = result.setdefault("required", [])
    if "engine.log_quiesce" not in required:
        required.append("engine.log_quiesce")
    return result


def _trace_binding(path, packet, *, max_history=200000, analysis_options):
    # Exactly one canonical decode and its existing full second byte fingerprint.
    # No external report can bypass that owner, and no second event scan lives here.
    options = dict(analysis_options)
    required = options.pop("required")
    report = lifecycle.analyze_trace_file(path, required, cleanup_ownership=True,
        terminal_session_binding=True, max_history=max_history, **options)
    last = report["cleanup_ownership"]["last"]
    scope, session = int(packet["cleanup_scope"]), int(packet["session"])
    if int(last["scope_id"]) != scope:
        raise ValueError("terminal ack cleanup scope is not the final trace retirement")
    marker = report["cleanup_ownership"]["terminal_sessions"]["final"]
    if marker is None:
        raise ValueError("terminal ack final cleanup has no session counter")
    if int(marker["scope_id"]) != scope or int(marker["session"]) != session:
        raise ValueError("terminal session counter does not uniquely belong to final cleanup")
    # Preserve the post-validation identity check without decoding or hashing a
    # third pass. The canonical reader already compared both complete byte reads.
    if list(lifecycle._fingerprint(Path(path).stat())) != report["source_path_fingerprint"]:
        raise ValueError("terminal ack trace changed between canonical validation and terminal binding")
    binding = dict(path=str(path), sha256=report["sha256"], input_bytes=report["input_bytes"],
                cleanup_scope=packet["cleanup_scope"], session=packet["session"], counter=marker["counter"],
                cleanup_attempts=report["cleanup_ownership"]["attempts_count"],
                validation_limits=dict(report["validation_limits"]),
                scope="same complete CPU trace and final successful cleanup packet; not GPU retirement")
    if "history_storage" in report:
        binding["history_storage"] = report["history_storage"]
    return binding, report


def observe_terminal_ack(process, token, *, expected_stderr_path, trace_path=None,
                         terminal_trace_max_history=None, trace_analysis_options=None, return_trace_report=False,
                         trace_history_db_path=None, trace_disk_history_max_records=None, trace_disk_history_max_bytes=None):
    """Observe owned stderr and optionally return the same complete trace report.

    trace_analysis_options supports explicit required/require_wait/counter-name
    lists, bounded scope_prefixes and TRACE_BOOL_OPTIONS observational flags. Cleanup/session gates
    stay mandatory; history has its existing separate budget argument. No
    caller-provided report, alternate parser, path or limit override is accepted.
    """
    analysis_options = _analysis_options(trace_analysis_options,
        trace_requested=trace_path is not None, return_trace_report=return_trace_report)
    history_budget = terminal_trace_history_budget(terminal_trace_max_history,
                                                   trace_requested=trace_path is not None)
    if any(value is not None for value in (trace_history_db_path, trace_disk_history_max_records, trace_disk_history_max_bytes)):
        if trace_path is None:
            raise ValueError("disk history requires a requested terminal trace")
        if __package__:
            from .trace_history_storage import history_options
        else:
            from trace_history_storage import history_options
        history_options(trace_history_db_path, trace_disk_history_max_records, trace_disk_history_max_bytes)
        analysis_options.update(history_db_path=trace_history_db_path,
            disk_history_max_records=trace_disk_history_max_records, disk_history_max_bytes=trace_disk_history_max_bytes)
    if not isinstance(token, str) or not TOKEN.fullmatch(token):
        raise ValueError("terminal ack request token must be 32 lowercase hex characters")
    if process.get("timed_out") is not False or type(process.get("returncode")) is not int or process["returncode"] != 0:
        raise ValueError("terminal ack requires the owned child to exit successfully")
    identity = process.get("process_identity")
    if not isinstance(identity, dict) or type(identity.get("pid")) is not int or not 0 < identity["pid"] <= 0xffffffff:
        raise ValueError("terminal ack requires the observed child PID")
    birth, source = identity.get("creation_time_unix_s"), identity.get("creation_time_source")
    if not isinstance(source, str) or not source:
        raise ValueError("terminal ack requires explicit process birth availability")
    if birth is not None and (type(birth) not in (int, float) or not math.isfinite(birth) or birth <= 0):
        raise ValueError("terminal ack process birth is invalid")
    if (birth is not None) != (source == "psutil.create_time"):
        raise ValueError("terminal ack process birth source disagrees with availability")
    raw_path = process.get("stderr_path")
    if not isinstance(raw_path, str) or not raw_path:
        raise ValueError("terminal ack requires an owned stderr file")
    path = Path(raw_path).resolve()
    if path != Path(expected_stderr_path).resolve():
        raise ValueError("terminal ack stderr path does not belong to the launched child")
    packet, stderr = _stderr_packet(path, token)
    if trace_path is not None and packet["trace_status"] != "complete":
        raise ValueError("terminal ack requested trace was not completely published")
    binding, trace_report = (_trace_binding(trace_path, packet, max_history=history_budget,
        analysis_options=analysis_options) if trace_path is not None else (None, None))
    result = dict(schema=1, status="HOST_TERMINAL_ACK_OBSERVED", packet=packet,
                process_identity=dict(identity), stderr=stderr, trace=binding,
                trace_binding="validated" if binding else "not_requested",
                log_file_api_close_observed=packet["log_file_close"] == "api_ok",
                log_durability_observed=False,
                scope="owned child stderr token and host-returned Engine tail; process exit observed separately; "
                      "not OS resource reclamation, GPU completion, file durability or uninstrumented ownership")
    if return_trace_report:
        result["trace_report"] = trace_report
    return result
