#!/usr/bin/env python3
"""Validate explicit CPU lifecycles with a complete bounded streaming CLI."""
from __future__ import annotations
import argparse
import codecs
import hashlib
import json
import math
import os
from pathlib import Path


class TraceLimitError(ValueError):
    """A declared validation state limit was exceeded; never a prefix PASS."""


class TraceFileError(ValueError):
    """Invalid validation with bounded, full-byte source identity evidence."""
    def __init__(self, error, evidence):
        super().__init__(str(error))
        self.evidence = evidence


def _number(value, name):
    try:
        valid = not isinstance(value, bool) and isinstance(value, (int, float)) and math.isfinite(value) and value >= 0
    except OverflowError:
        valid = False
    if not valid:
        raise ValueError("invalid " + name)
    return value


def _names(requested, message):
    if isinstance(requested, (str, bytes)):
        raise ValueError(message)
    values = list(requested)
    if any(not isinstance(name, str) or not name for name in values) or len(set(values)) != len(values):
        raise ValueError(message)
    return values


def _scope_prefixes(requested):
    """Bound explicit observation requests before opening inputs or outputs."""
    if isinstance(requested, (str, bytes, dict)):
        raise ValueError("scope prefixes must be unique bounded nonempty strings")
    try:
        iterator = iter(requested)
    except TypeError as error:
        raise ValueError("scope prefixes must be unique bounded nonempty strings") from error
    values = []
    for prefix in iterator:
        if len(values) >= 128:
            raise TraceLimitError("scope prefix request limit exceeded")
        if (type(prefix) is not str or not prefix or len(prefix) > 128 or
                prefix != prefix.strip() or not prefix.isprintable() or prefix in values):
            raise ValueError("scope prefixes must be unique bounded nonempty strings")
        values.append(prefix)
    return tuple(values)


def _writer_diagnostics(metadata):
    """Optional versioned payload-write observations, through the same parser.

    Older captures remain admissible without this block. The sample is elapsed
    monotonic wall time/requested bytes; it does not identify disk or GPU work.
    """
    if "writer_diagnostics" not in metadata:
        return
    diag = metadata["writer_diagnostics"]
    def u64(value):
        if type(value) is not int or not 0 <= value <= (1 << 64) - 1:
            raise ValueError("invalid writer diagnostic integer")
        return value
    if not isinstance(diag, dict) or type(diag.get("version")) is not int or diag["version"] != 1:
        raise ValueError("unknown writer diagnostics version")
    if metadata.get("mode") != "continuous" or diag.get("operation") != "payload-write":
        raise ValueError("invalid writer diagnostic operation")
    if "clock_domain" in diag and diag["clock_domain"] != "process-perf-ns":
        raise ValueError("unknown writer diagnostic clock domain")
    expected_delay = 16000000 if metadata.get("consumer_mode") == "threaded" else 0
    if u64(diag.get("flush_delay_ns")) != expected_delay:
        raise ValueError("writer flush policy disagrees with consumer")
    calls = u64(metadata.get("writer_append_calls"))
    payload = u64(metadata.get("writer_payload_bytes"))
    capacity = u64(metadata.get("writer_buffer_capacity"))
    limit = u64(metadata.get("writer_buffer_limit"))
    maximum = u64(metadata.get("writer_max_append_ns"))
    if not limit or capacity > limit:
        raise ValueError("invalid writer staging bound")
    sizes, walls = diag.get("size_counts"), diag.get("wall_ns_counts")
    for values, length in ((sizes, 6), (walls, 7)):
        if not isinstance(values, list) or len(values) != length or sum(u64(v) for v in values) != calls:
            raise ValueError("writer diagnostic bucket accounting disagrees with appends")
    started = u64(diag.get("slowest_started_ns"))
    finished = u64(diag.get("slowest_finished_ns"))
    requested = u64(diag.get("slowest_requested_bytes"))
    if finished < started or finished - started != maximum:
        raise ValueError("writer slowest sample disagrees with elapsed maximum")
    if not calls:
        if any((payload, maximum, started, finished, requested, metadata.get("captured", 0))):
            raise ValueError("writer zero-write sample contains payload")
        return
    if not 0 < requested <= payload or requested >= capacity:
        raise ValueError("writer requested bytes exceed accepted payload/staging")
    size_upper = (4096, 16384, 65536, 262144, 1048576)
    wall_upper = (100000, 1000000, 4000000, 16000000, 100000000, 1000000000)
    size_index = sum(requested > upper for upper in size_upper)
    wall_index = sum(maximum > upper for upper in wall_upper)
    if not sizes[size_index] or not walls[wall_index] or any(walls[wall_index + 1:]):
        raise ValueError("writer slowest sample is absent from its buckets")
    lows = (1,) + tuple(v + 1 for v in size_upper)
    highs = tuple(min(v, capacity - 1) for v in size_upper) + (capacity - 1,)
    if any(n and low > high for n, low, high in zip(sizes, lows, highs)):
        raise ValueError("writer size bucket exceeds retained staging capacity")
    if not sum(n * v for n, v in zip(sizes, lows)) <= payload <= sum(n * v for n, v in zip(sizes, highs)):
        raise ValueError("writer payload bytes disagree with size buckets")


def _metadata(metadata, count, zero_thread, positive_thread, zero_names):
    if not isinstance(metadata, dict):
        raise ValueError("capture metadata and traceEvents are required")
    if "reader_diagnostics" in metadata:
        if __package__:
            from .trace_reader_diagnostics import validate_reader_diagnostics
        else:
            from trace_reader_diagnostics import validate_reader_diagnostics
        validate_reader_diagnostics(metadata)
    if metadata.get("capture_generation_stable") is not True:
        raise ValueError("capture crossed a reset or has unknown generation")
    if "sequence_exhausted" in metadata and metadata["sequence_exhausted"] is not False:
        raise ValueError("capture reports sequence exhaustion or an invalid exhaustion state")
    for flag in ("write_ok", "capture_complete"):
        if flag in metadata and metadata[flag] is not True:
            raise ValueError("capture explicitly reports failed " + flag)
    mode = metadata.get("mode", "resident")
    if mode not in ("resident", "continuous"):
        raise ValueError("unknown capture mode")
    if mode == "continuous":
        consumer = metadata.get("consumer_mode")
        if consumer not in ("threaded", "cooperative"):
            raise ValueError("unknown continuous consumer mode")
        if metadata.get("worker_available") is not (consumer == "threaded"):
            raise ValueError("consumer mode disagrees with worker availability")
        if metadata.get("write_ok") is not True or metadata.get("capture_complete") is not True:
            raise ValueError("continuous capture write or completeness failed")
        for name in ("unexported", "active_threads", "active_work_items", "active_waits"):
            value = _number(metadata.get(name), name)
            if type(value) is not int or value != 0:
                raise ValueError("continuous capture has incomplete " + name)
    for name in ("recorded", "captured", "overwritten_before_capture", "cursor_lost", "lost"):
        if type(_number(metadata.get(name), name)) is not int:
            raise ValueError("capture counts must be integers")
    if metadata["lost"] != metadata["overwritten_before_capture"] + metadata["cursor_lost"]:
        raise ValueError("inconsistent loss accounting")
    if metadata["lost"] or metadata["recorded"] != metadata["captured"]:
        raise ValueError("trace capture is incomplete; retain as diagnostic only")
    if not count:
        raise ValueError("empty capture measured no lifecycle")
    if metadata["captured"] != count:
        raise ValueError("captured event count does not match payload")
    _writer_diagnostics(metadata)
    if zero_thread:
        if mode != "continuous" or metadata.get("consumer_mode") != "cooperative" or metadata.get("worker_available") is not False or positive_thread:
            raise ValueError("zero thread identity requires a single cooperative capture")
        if len(zero_names) != 1 or not next(iter(zero_names)).strip():
            raise ValueError("zero thread identity needs one unambiguous thread name")


def _accumulate_scope_timing(row, elapsed_ms, duration_ns, description):
    total = row["elapsed_ms_sum"] + elapsed_ms
    if not math.isfinite(total):
        raise ValueError(description + " elapsed aggregate is not finite")
    row["elapsed_ms_sum"] = total
    row["elapsed_ms_min"] = elapsed_ms if row["elapsed_ms_min"] is None else min(row["elapsed_ms_min"], elapsed_ms)
    row["elapsed_ms_max"] = elapsed_ms if row["elapsed_ms_max"] is None else max(row["elapsed_ms_max"], elapsed_ms)
    row["measured_ns_sum"] += duration_ns
    row["measured_ns_min"] = duration_ns if row["measured_ns_min"] is None else min(row["measured_ns_min"], duration_ns)
    row["measured_ns_max"] = duration_ns if row["measured_ns_max"] is None else max(row["measured_ns_max"], duration_ns)


class _TraceAnalysis:
    """One event/schema/pairing authority for the legacy API and streaming CLI.

    Streaming keeps every historical submitted identity, including completed
    services: eviction would accept replay. Explicit limits reject instead.
    Output witnesses are first/last per finite category, diagnostic (name, state)
    or requested counter; they are reporting samples, never a validation
    projection or event cutoff.
    """
    def __init__(self, required, all_zero, last_zero, *, require_wait=(), limits=None, resource_observations=False,
                 renderer_submissions=False, gpu_view_observations=False, gpu_submission_joins=False,
                 render_pipeline_observations=False, readback_operations=False, cleanup_ownership=False,
                 screenshot_requests=False, offscreen_configurations=False, filesystem_read_details=False,
                 shader_provenance=False, terminal_session_binding=False, history_owner=None,
                 scope_witnesses=False, scope_prefixes=()):
        self.history_owner = history_owner
        self.scope_prefixes = _scope_prefixes(scope_prefixes)
        self.scope_prefix_stats = {} if self.scope_prefixes else None
        self.scope_prefix_names = set() if self.scope_prefixes else None
        if type(scope_witnesses) is not bool:
            raise ValueError("scope_witnesses must be an explicit bool")
        self.scope_witnesses = scope_witnesses
        if type(shader_provenance) is not bool:
            raise ValueError("shader_provenance must be an explicit bool")
        self.shaders = None
        if shader_provenance:
            if __package__:
                from .shader_provenance_observation import ShaderProvenanceObservation
            else:
                from shader_provenance_observation import ShaderProvenanceObservation
            self.shaders = ShaderProvenanceObservation(limits["active"] if limits else 8192,
                limits["history"] if limits else 200000)
        if type(filesystem_read_details) is not bool:
            raise ValueError("filesystem_read_details must be an explicit bool")
        self.file_reads = None
        if filesystem_read_details:
            if __package__:
                from .filesystem_read_observation import FilesystemReadObservation
            else:
                from filesystem_read_observation import FilesystemReadObservation
            self.file_reads = FilesystemReadObservation(limits["active"] if limits else 8192,
                limits["history"] if limits else 200000)
        if type(offscreen_configurations) is not bool:
            raise ValueError("offscreen_configurations must be an explicit bool")
        self.offscreen = None
        if offscreen_configurations:
            if __package__:
                from .offscreen_configure_observation import OffscreenConfigureObservation
            else:
                from offscreen_configure_observation import OffscreenConfigureObservation
            self.offscreen = OffscreenConfigureObservation(limits["active"] if limits else 8192)
        if type(screenshot_requests) is not bool:
            raise ValueError("screenshot_requests must be an explicit bool")
        self.screenshots = None
        if screenshot_requests:
            if __package__:
                from .screenshot_request_observation import ScreenshotRequestObservation
            else:
                from screenshot_request_observation import ScreenshotRequestObservation
            self.screenshots = ScreenshotRequestObservation(limits["active"] if limits else 8192)
        if type(cleanup_ownership) is not bool:
            raise ValueError("cleanup_ownership must be an explicit bool")
        if type(terminal_session_binding) is not bool:
            raise ValueError("terminal_session_binding must be an explicit bool")
        if terminal_session_binding and not cleanup_ownership:
            raise ValueError("terminal_session_binding requires cleanup_ownership")
        self.cleanup = None
        if cleanup_ownership:
            if __package__:
                from .cleanup_ownership_observation import CleanupOwnershipObservation
            else:
                from cleanup_ownership_observation import CleanupOwnershipObservation
            self.cleanup = CleanupOwnershipObservation(limits["active"] if limits else 8192,
                terminal_session_binding=terminal_session_binding, max_history=limits["history"] if limits else 200000,
                terminal_sessions=history_owner.terminal_sessions if history_owner else None)
        if type(resource_observations) is not bool:
            raise ValueError("resource_observations must be an explicit bool")
        self.resources = None
        if resource_observations:
            if __package__:
                from .resource_trace_observation import ResourceTraceObservation
            else:
                from resource_trace_observation import ResourceTraceObservation
            self.resources = ResourceTraceObservation()
        if type(renderer_submissions) is not bool:
            raise ValueError("renderer_submissions must be an explicit bool")
        if type(gpu_submission_joins) is not bool:
            raise ValueError("gpu_submission_joins must be an explicit bool")
        self.report_submissions = renderer_submissions
        self.submissions = self.gpu_joins = None
        if renderer_submissions or gpu_submission_joins:
            if __package__:
                from .renderer_submission_observation import RendererSubmissionObservation
            else:
                from renderer_submission_observation import RendererSubmissionObservation
            self.submissions = RendererSubmissionObservation(limits["active"] if limits else 8192)
        if gpu_submission_joins:
            if __package__:
                from .gpu_submission_join import GpuSubmissionJoin
            else:
                from gpu_submission_join import GpuSubmissionJoin
            self.gpu_joins = GpuSubmissionJoin(limits["active"] if limits else 8192)
        if type(readback_operations) is not bool:
            raise ValueError("readback_operations must be an explicit bool")
        self.readbacks = None
        if readback_operations:
            if __package__:
                from .readback_operation_observation import ReadbackOperationObservation
            else:
                from readback_operation_observation import ReadbackOperationObservation
            self.readbacks = ReadbackOperationObservation(limits["active"] if limits else 8192)
        if type(gpu_view_observations) is not bool:
            raise ValueError("gpu_view_observations must be an explicit bool")
        self.gpu_views = None
        if gpu_view_observations:
            if __package__:
                from .gpu_view_observation import GpuViewObservation
            else:
                from gpu_view_observation import GpuViewObservation
            self.gpu_views = GpuViewObservation(limits["active"] if limits else 8192, limits["names"] if limits else 4096)
        if type(render_pipeline_observations) is not bool:
            raise ValueError("render_pipeline_observations must be an explicit bool")
        self.render_pipeline = None
        if render_pipeline_observations:
            if __package__:
                from .render_pipeline_observation import RenderPipelineObservation
            else:
                from render_pipeline_observation import RenderPipelineObservation
            self.render_pipeline = RenderPipelineObservation(limits["active"] if limits else 8192)
        self.required = _names(required, "unique explicit required phases are needed")
        if not self.required:
            raise ValueError("unique explicit required phases are needed")
        if scope_witnesses and len(self.required) > 128:
            raise TraceLimitError("required scope witness limit exceeded")
        self.required_wait = _names(require_wait, "wait names must be unique nonempty strings")
        self.counter_names = {"all_zero": _names(all_zero, "counter names must be unique nonempty strings"),
                              "last_zero": _names(last_zero, "counter names must be unique nonempty strings")}
        self.limits = limits
        if limits and (len(self.required) + len(self.required_wait) > 128 or sum(map(len, self.counter_names.values())) > 128):
            raise TraceLimitError("explicit requirement limit exceeded")
        self.active = {}
        self.tasks = history_owner.tasks if history_owner else {}
        self.completed_services = history_owner.completed_services if history_owner else set()
        self.names, self.complete, self.diagnostics = {}, [], []
        self.thread_timestamps = {}
        self.count = self.scope_count = self.failed = self.cancelled = 0
        self.zero_thread = self.positive_thread = False
        self.zero_names = set()
        self.required_stats = {name: {"name": name, "observed_count": 0, "failed": 0, "cancelled": 0,
            "elapsed_ms_sum": 0.0, "elapsed_ms_min": None, "elapsed_ms_max": None,
            "measured_ns_sum": 0, "measured_ns_min": None, "measured_ns_max": None,
            "timing_scope": "inclusive per-service wall intervals; nested services overlap, not OS CPU execution or GPU completion"}
            for name in self.required}
        self.required_wait_stats = {name: {"name": name, "category": "wait", "observed_count": 0,
            "scope": "complete paired CPU wait intervals; not GPU completion or successful target work"}
            for name in self.required_wait}
        self.category_stats, self.diagnostic_samples, self.diagnostic_stats = {}, {}, {}
        self.counters = {name: {"name": name, "observed_count": 0, "raw_indices": [], "values": []}
                         for name in set(self.counter_names["all_zero"] + self.counter_names["last_zero"])}

    def _intern(self, name):
        if not self.limits:
            return name
        if name not in self.names:
            if len(self.names) >= self.limits["names"]:
                raise TraceLimitError("distinct retained name limit exceeded")
            if len(name) > 4096:
                raise TraceLimitError("retained name length limit exceeded")
            self.names[name] = name
        return self.names[name]

    def _record_diagnostic(self, scope, index, timestamp, duration_ns):
        # Feed already interned the name and admitted state 1/2: at most two
        # rows per retained name, with only two witnesses, never an event list.
        key = (scope["name"], scope["state"])
        sample = dict(scope, end_raw_index=index, end_timestamp_us=timestamp,
                      measured_ns=duration_ns)
        row = self.diagnostic_stats.get(key)
        if row is None:
            row = {"name": scope["name"], "state": scope["state"],
                   "outcome": "failed" if scope["state"] == 1 else "cancelled",
                   "observed_count": 0, "first": sample,
                   "elapsed_ms_sum": 0.0, "elapsed_ms_min": None, "elapsed_ms_max": None,
                   "measured_ns_sum": 0, "measured_ns_min": None, "measured_ns_max": None,
                   "timing_scope": "inclusive paired CPU task wall intervals; nested scopes overlap, not OS CPU execution or GPU completion",
                   "witness_order": "first/last in exported event order, not timestamp order"}
            self.diagnostic_stats[key] = row
        row["observed_count"] += 1
        row["last"] = sample
        _accumulate_scope_timing(row, scope["elapsed_ms"], duration_ns, "diagnostic")

    def _record_scope_prefix(self, scope, start, index, timestamp, duration_ns):
        # The canonical owner has already validated this complete B/E pair.
        name = scope["name"]
        names_limit = self.limits["names"] if self.limits else 4096
        if len(name) > 4096:
            raise TraceLimitError("scope prefix retained name length limit exceeded")
        if name not in self.scope_prefix_names:
            if len(self.scope_prefix_names) >= names_limit:
                raise TraceLimitError("scope prefix retained name limit exceeded")
            self.scope_prefix_names.add(name)
        key = (name, scope["thread"])
        row = self.scope_prefix_stats.get(key)
        sample = dict(scope, begin_timestamp_us=start[1], end_timestamp_us=timestamp,
                      begin_raw_index=start[4], end_raw_index=index, measured_ns=duration_ns)
        if row is None:
            if len(self.scope_prefix_stats) >= (self.limits["history"] if self.limits else 200000):
                raise TraceLimitError("scope prefix aggregate limit exceeded")
            row = dict(name=name, thread=scope["thread"], observed_count=0, failed=0, cancelled=0,
                       elapsed_ms_sum=0.0, elapsed_ms_min=None, elapsed_ms_max=None,
                       measured_ns_sum=0, measured_ns_min=None, measured_ns_max=None, first=sample)
            self.scope_prefix_stats[key] = row
        row["observed_count"] += 1
        row["failed"] += scope["category"] == "task" and scope["state"] == 1
        row["cancelled"] += scope["category"] == "task" and scope["state"] == 2
        row["last"] = sample
        _accumulate_scope_timing(row, scope["elapsed_ms"], duration_ns, "scope prefix")

    def feed(self, event):
        index = self.count
        self.count += 1
        if not isinstance(event, dict):
            raise ValueError("invalid trace event")
        phase, category, tid = event.get("ph"), event.get("cat"), event.get("tid")
        if phase not in ("B", "E", "i", "C", "M"):
            raise ValueError("unknown event phase")
        if type(tid) is not int or tid < 0 or type(event.get("pid")) is not int or event["pid"] != 1:
            raise ValueError("missing or invalid process/thread identity")
        self.zero_thread |= tid == 0
        self.positive_thread |= tid > 0
        args, name = event.get("args"), event.get("name")
        if phase == "M":
            if name != "thread_name" or not isinstance(args, dict) or not isinstance(args.get("name"), str):
                raise ValueError("unknown metadata event")
            if tid == 0:
                self.zero_names.add(self._intern(args["name"]))
            return
        if category not in {"core", "frame", "thread", "task", "wait", "render", "gpu_scene"}:
            raise ValueError("unknown event category")
        timestamp = _number(event.get("ts"), "timestamp")
        if tid in self.thread_timestamps:
            if timestamp < self.thread_timestamps[tid]:
                raise ValueError("trace timestamp moved backwards within thread: " + str(tid))
        elif len(self.thread_timestamps) >= (self.limits["history"] if self.limits else 200000):
            raise TraceLimitError("retained thread clock identity limit exceeded")
        self.thread_timestamps[tid] = timestamp
        if not isinstance(args, dict):
            raise ValueError("event arguments are required")
        if self.file_reads:
            self.file_reads.event(event)
        if self.shaders:
            self.shaders.event(event)
        if self.screenshots:
            self.screenshots.event(event, index)
        if self.cleanup:
            self.cleanup.event(event)
        if self.offscreen:
            self.offscreen.event(event)
        if self.limits and (not isinstance(name, str) or not name):
            raise ValueError("missing event name")
        if phase == "C" and (name in self.counters or self.limits):
            value = args.get("value")
            try:
                valid = not isinstance(value, bool) and isinstance(value, (int, float)) and math.isfinite(value)
            except OverflowError:
                valid = False
            if not valid:
                raise ValueError("invalid counter value: " + str(name))
            if name in self.counters:
                row = self.counters[name]
                row["observed_count"] += 1
                row["last_value"], row["last_raw_index"] = value, index
                row["any_nonzero"] = row.get("any_nonzero", False) or value != 0
                if not self.limits or len(row["values"]) < 2:
                    row["raw_indices"].append(index); row["values"].append(value)
                else:
                    row["raw_indices"][-1] = index; row["values"][-1] = value
        if phase == "C" and self.file_reads:
            self.file_reads.counter(event, index, self.active)
        if phase == "C" and self.shaders:
            self.shaders.counter(event, index, self.active)
        if phase == "C" and self.resources:
            self.resources.counter(event, index)
        if phase == "C" and self.submissions:
            self.submissions.counter(event, index, self.active)
        if phase == "C" and self.gpu_views:
            self.gpu_views.counter(event, index, self.active)
        if phase == "C" and self.gpu_joins:
            self.gpu_joins.counter(event, index, self.active)
        if phase == "C" and self.render_pipeline:
            self.render_pipeline.counter(event, index, self.active)
        if phase == "C" and self.readbacks:
            self.readbacks.counter(event, index, self.active)
        if phase == "C" and self.offscreen:
            self.offscreen.counter(event, index, self.active)
        if phase == "C" and self.cleanup:
            self.cleanup.counter(event, index, self.active)
        if phase == "i" and category == "task":
            task = _number(args.get("task"), "submitted task")
            parent = _number(args.get("parent"), "task parent")
            kind = args.get("kind")
            if type(task) is not int or not task or task in self.tasks or type(parent) is not int or type(kind) is not int or kind not in (0, 1, 2):
                raise ValueError("invalid or repeated submitted task")
            if self.limits and self.history_owner is None and len(self.tasks) >= self.limits["history"]:
                raise TraceLimitError("historical task identity limit exceeded")
            self.tasks[task] = (kind, self._intern(name), timestamp, parent)
            if self.resources and name == "engine.app_resources":
                self.resources.submit(event, index)
        if phase not in ("B", "E"):
            return
        identity = _number(args.get("id"), "scope id")
        if type(identity) is not int or not identity:
            raise ValueError("invalid scope identity")
        parent = _number(args.get("parent"), "scope owner")
        if type(parent) is not int:
            raise ValueError("scope owner must be an integer")
        if not isinstance(name, str) or not name:
            raise ValueError("missing scope name")
        name = self._intern(name)
        key = (category, tid, identity)
        if phase == "B":
            if key in self.active:
                raise ValueError("duplicate active scope identity")
            owner = self.tasks.get(parent, (None, None, None, None))
            if category == "task" and identity == parent and owner[0] == 2 and (owner[1] != name or owner[2] > timestamp):
                raise ValueError("service submission must precede its scope")
            if self.limits and len(self.active) >= self.limits["active"]:
                raise TraceLimitError("active scope limit exceeded")
            self.active[key] = (name, timestamp, parent, owner)
            if self.scope_witnesses or self.scope_prefixes:
                self.active[key] += (index,)
            if self.shaders and name in self.shaders.stats:
                self.shaders.begin(event, index, self.active[key], self.tasks)
            if self.file_reads and name in ("fs.host.read_all", "fs.host.read_capped"):
                self.file_reads.begin(event, index, self.active[key])
            if self.cleanup and name in ("engine.cleanup_attempt", "engine.audio_master_retire"):
                self.cleanup.begin(event, index, self.active[key])
            if self.submissions and name in ("renderer.frame_advance_cpu", "renderer.submit_cpu",
                                              "engine.renderer_tail_cpu"):
                self.submissions.begin(event, index, self.active[key])
            if self.gpu_views and name == "renderer.submit_cpu":
                self.gpu_views.begin(event, index, self.active[key])
            if self.gpu_joins and name == "renderer.submit_cpu":
                self.gpu_joins.begin(event, index, self.active[key])
            if self.render_pipeline and name == "render_pipeline.publish":
                self.render_pipeline.begin(event, index, self.active[key])
            if self.readbacks and name == "readback.operation":
                self.readbacks.begin(event, index, self.active[key])
            if self.offscreen and name == "renderer.view_configure":
                self.offscreen.begin(event, index, self.active[key])
            if self.resources and name == "engine.app_resources":
                self.resources.begin(event, index)
            return
        start = self.active.pop(key, None)
        if start is None or start[0] != name or start[2] != parent or timestamp < start[1]:
            raise ValueError("unpaired or reversed scope: " + name)
        duration_ns = _number(args.get("ns"), "duration")
        if type(duration_ns) is not int:
            raise ValueError("duration must be integer nanoseconds")
        state = args.get("state")
        if category == "task" and (type(state) is not int or state not in (0, 1, 2)):
            raise ValueError("task outcome is missing or invalid")
        kind, task_name, _, submitted_parent = start[3]
        if category == "task" and kind == 2 and identity == parent:
            if parent in self.completed_services:
                raise ValueError("service identity completed more than once")
            self.completed_services.add(parent)
            if task_name != name:
                raise ValueError("service scope does not match submitted owner")
        scope = {"name": name, "thread": tid, "id": identity, "category": category,
                 "owner": parent, "task_kind": kind, "submitted_parent": submitted_parent,
                 "elapsed_ms": (timestamp - start[1]) / 1000,
                 "measured_ms": duration_ns / 1000000, "state": state}
        if self.scope_prefixes and name.startswith(self.scope_prefixes):
            self._record_scope_prefix(scope, start, index, timestamp, duration_ns)
        if self.file_reads and name in ("fs.host.read_all", "fs.host.read_capped"):
            self.file_reads.end(event, index, scope)
        if self.shaders and name in self.shaders.stats:
            self.shaders.end(event, index, scope)

        if self.cleanup and name in ("engine.cleanup_attempt", "engine.audio_master_retire"):
            self.cleanup.end(event, index, scope)
        if self.resources and name == "engine.app_resources":
            self.resources.end(event, index, scope)
        if self.submissions and name in ("renderer.frame_advance_cpu", "renderer.submit_cpu",
                                         "engine.renderer_tail_cpu"):
            submission = self.submissions.end(event, index, scope)
            if self.gpu_joins and submission is not None:
                self.gpu_joins.submission(submission, submitted_parent)
        if self.gpu_joins and name == "renderer.submit_cpu":
            self.gpu_joins.end(event, index, scope)
        if self.gpu_views and name == "renderer.submit_cpu":
            self.gpu_views.end(event, index, scope)
        if self.render_pipeline and name == "render_pipeline.publish":
            self.render_pipeline.end(event, index, scope)
        if self.readbacks and name == "readback.operation":
            self.readbacks.end(event, index, scope)
        if self.offscreen and name == "renderer.view_configure":
            self.offscreen.end(event, index, scope)
        self.scope_count += 1
        if category == "wait" and name in self.required_wait_stats:
            self.required_wait_stats[name]["observed_count"] += 1
        if category == "task" and kind == 2 and identity == parent and name in self.required_stats:
            row = self.required_stats[name]; row["observed_count"] += 1
            row["failed"] += state == 1; row["cancelled"] += state == 2
            _accumulate_scope_timing(row, scope["elapsed_ms"], duration_ns, "required phase")
            if self.scope_witnesses:
                witness = dict(scope_id=identity, thread=tid, owner=parent,
                    submitted_parent=submitted_parent, state=state,
                    begin_timestamp_us=start[1], end_timestamp_us=timestamp,
                    begin_raw_index=start[4], end_raw_index=index)
                retained = row.setdefault("_scope_witnesses", {"first": witness})
                retained["last"] = witness
        diagnostic = category == "task" and state in (1, 2)
        self.failed += diagnostic and state == 1; self.cancelled += diagnostic and state == 2
        if not self.limits:
            self.complete.append(scope)
            if diagnostic: self.diagnostics.append(scope)
        else:
            sample = dict(scope, end_raw_index=index)
            row = self.category_stats.setdefault(category, {"category": category, "observed_count": 0, "first": sample})
            row["observed_count"] += 1; row["last"] = sample
            if diagnostic:
                self.diagnostic_samples.setdefault(state, sample)
                self._record_diagnostic(scope, index, timestamp, duration_ns)

    def finish(self, metadata):
        _metadata(metadata, self.count, self.zero_thread, self.positive_thread, self.zero_names)
        if self.active:
            raise ValueError("unfinished scopes in capture")
        if not self.scope_count:
            raise ValueError("no completed lifecycle scopes")
        for name, row in self.required_stats.items():
            if not row["observed_count"] or row["failed"] or row["cancelled"]:
                raise ValueError("required phase missing or unsuccessful: " + name)
        for name, row in self.required_wait_stats.items():
            if not row["observed_count"]:
                raise ValueError("required wait missing: " + name)
        counters = {"all_zero": [], "last_zero": [], "scope": "CPU counter observations; last means exported stream order, not timestamp order or physical GPU completion"}
        for mode, names in self.counter_names.items():
            for name in names:
                row = self.counters[name]
                if not row["observed_count"]:
                    raise ValueError("required counter missing: " + name)
                if (mode == "all_zero" and row["any_nonzero"]) or (mode == "last_zero" and row["last_value"] != 0):
                    raise ValueError("required counter is not zero: " + name)
                row = {key: value for key, value in row.items() if key != "any_nonzero"}
                if self.limits:
                    row["raw_indices_samples"] = row.pop("raw_indices")
                    row["values_samples"] = row.pop("values")
                    row["samples_complete"] = row["observed_count"] <= 2
                counters[mode].append(row)
        result = {"status": "REQUIRED_CPU_LIFECYCLE_SCOPES_OBSERVED", "required": self.required,
                  "required_wait": self.required_wait, "required_wait_aggregates": list(self.required_wait_stats.values()),
                  "capture": metadata, "counter_controls": counters,
                  "writer_clock_domain": metadata.get("writer_diagnostics", {}).get("clock_domain", "unknown"),
                  "thread_identity_scope": "named_single_cooperative_thread" if self.zero_thread else "producer_positive_thread_ids",
                  "nonrequired_task_outcomes": {"failed": self.failed, "cancelled": self.cancelled},
                  "scope": "captured CPU trace only; not GPU completion, whole application coverage or leak certification"}
        if not self.limits:
            result["scopes"] = self.complete
            result["nonrequired_task_outcomes"]["scopes"] = self.diagnostics
        else:
            result["nonrequired_task_outcomes"]["aggregates"] = list(self.diagnostic_stats.values())
            result.update(validation_scope="all events and all lifecycle scope pairs; no validation projection",
                          events_validated=self.count, completed_scope_count=self.scope_count,
                          required_phase_aggregates=[{key: value for key, value in row.items()
                              if key != "_scope_witnesses"} for row in self.required_stats.values()],
                          scope_aggregates=list(self.category_stats.values()),
                          diagnostic_samples=list(self.diagnostic_samples.values()),
                          output_samples="first/last per finite category and task (name, state), first per outcome, first/last per requested counter; all matching observations validated",
                          validation_limits=self.limits, historical_task_count=len(self.tasks))
        if self.scope_witnesses:
            result["required_scope_witnesses"] = {
                "scope": "matched captured CPU service intervals; no OS/SDL/GPU clock alignment asserted",
                "timestamp_field": "traceEvents.ts", "timestamp_unit": "microseconds",
                "retention": "first/last in exported completion order; repeated envelopes are not a full timeline",
                "required": [dict(name=row["name"], observed_count=row["observed_count"],
                    samples_complete=row["observed_count"] <= 2, **row["_scope_witnesses"])
                    for row in self.required_stats.values()]}
        if self.scope_prefixes:
            prefixes = []
            for prefix in self.scope_prefixes:
                count = sum(row["observed_count"] for row in self.scope_prefix_stats.values()
                            if row["name"].startswith(prefix))
                prefixes.append(dict(prefix=prefix, observed_count=count,
                    status="CPU_SCOPE_ACTIVITY_OBSERVED" if count else "NO_ACTIVITY"))
            result["scope_prefix_observations"] = {
                "status": "CPU_SCOPE_ACTIVITY_OBSERVED" if self.scope_prefix_stats else "NO_ACTIVITY",
                "scope": "inclusive paired CPU intervals per (name, thread); overlapping/nested sums are not wall time, not OS CPU execution and not GPU duration or backend success",
                "retention": "first/last in exported completion order per (name, thread); not a full timeline; no duration sum across threads",
                "timestamp_field": "traceEvents.ts", "timestamp_unit": "microseconds",
                "prefixes": prefixes,
                "aggregates": [dict(row, samples_complete=row["observed_count"] <= 2)
                               for row in self.scope_prefix_stats.values()]}
        if self.file_reads:
            result["filesystem_read_details"] = self.file_reads.finish(self.tasks, self.completed_services)
        if self.shaders:
            result["shader_provenance"] = self.shaders.finish(self.tasks, self.completed_services)
        if self.resources:
            result["resource_observations"] = self.resources.finish()
        if self.submissions and self.report_submissions:
            result["renderer_submissions"] = self.submissions.finish()
        if self.gpu_joins:
            result["gpu_submission_joins"] = self.gpu_joins.finish()
        if self.gpu_views:
            result["gpu_view_observations"] = self.gpu_views.finish()
        if self.render_pipeline:
            result["render_pipeline_observations"] = self.render_pipeline.finish()
        if self.readbacks:
            result["readback_operations"] = self.readbacks.finish()
        if self.offscreen:
            result["offscreen_configurations"] = self.offscreen.finish(self.tasks, self.completed_services)
        if self.cleanup:
            result["cleanup_ownership"] = self.cleanup.finish(metadata, self.tasks, self.completed_services)
        if self.screenshots:
            result["screenshot_requests"] = self.screenshots.finish(metadata)
        return result


def analyze_trace(document, required, *, require_wait=(), require_counter_zero=(), require_counter_last_zero=(), resource_observations=False,
                  renderer_submissions=False, gpu_view_observations=False, gpu_submission_joins=False,
                  render_pipeline_observations=False, readback_operations=False, cleanup_ownership=False,
                  screenshot_requests=False, offscreen_configurations=False, filesystem_read_details=False,
                  shader_provenance=False, terminal_session_binding=False, scope_witnesses=False, scope_prefixes=()):
    """In-memory API: successful submitted services and explicit complete waits."""
    if not isinstance(document, dict) or not isinstance(document.get("traceEvents"), list):
        raise ValueError("capture metadata and traceEvents are required")
    analysis = _TraceAnalysis(required, require_counter_zero, require_counter_last_zero, require_wait=require_wait,
                              resource_observations=resource_observations, renderer_submissions=renderer_submissions,
                              gpu_view_observations=gpu_view_observations, gpu_submission_joins=gpu_submission_joins,
                              render_pipeline_observations=render_pipeline_observations,
                              readback_operations=readback_operations, cleanup_ownership=cleanup_ownership,
                              screenshot_requests=screenshot_requests, offscreen_configurations=offscreen_configurations,
                              filesystem_read_details=filesystem_read_details, shader_provenance=shader_provenance,
                              terminal_session_binding=terminal_session_binding, scope_witnesses=scope_witnesses,
                              scope_prefixes=scope_prefixes)
    for event in document["traceEvents"]: analysis.feed(event)
    return analysis.finish(document.get("jceTrace"))


def _json_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value: raise ValueError("duplicate JSON key: " + key)
        value[key] = item
    return value


def _invalid_constant(value):
    raise ValueError("nonfinite JSON constant: " + value)


class _JsonStream:
    """Bounded raw_decode over incremental strict UTF-8, one JSON value at a time."""
    def __init__(self, stream, chunk_bytes, max_value_chars):
        self.stream, self.chunk_bytes, self.max_value_chars = stream, chunk_bytes, max_value_chars
        self.decoder = codecs.getincrementaldecoder("utf-8")("strict")
        self.json_decoder = json.JSONDecoder(object_pairs_hook=_json_object, parse_constant=_invalid_constant)
        self.buffer, self.position, self.eof = "", 0, False
        self.digest, self.bytes_read = hashlib.sha256(), 0

    def more(self):
        if self.eof: return False
        self.buffer = self.buffer[self.position:]; self.position = 0
        raw = self.stream.read(self.chunk_bytes)
        self.digest.update(raw); self.bytes_read += len(raw)
        self.eof = not raw
        self.buffer += self.decoder.decode(raw, final=self.eof)
        return bool(raw)

    def peek(self):
        while True:
            while self.position < len(self.buffer) and self.buffer[self.position] in " \t\r\n": self.position += 1
            if self.position < len(self.buffer): return self.buffer[self.position]
            if not self.more(): return ""

    def take(self, character):
        if self.peek() != character: raise ValueError("invalid JSON envelope: expected " + character)
        self.position += 1

    def value(self):
        self.peek()
        while True:
            try:
                value, end = self.json_decoder.raw_decode(self.buffer, self.position)
                if end - self.position > self.max_value_chars:
                    raise TraceLimitError("JSON value size limit exceeded")
                # raw_decode may accept the prefix 1 of a split 1e+3. Require
                # a token boundary before accepting any scalar/value. Keys may
                # be followed by a colon; envelope/array owners check its use.
                boundary = end < len(self.buffer) and self.buffer[end] in " \t\r\n,]}:"
                if end == len(self.buffer) or not boundary:
                    if self.eof:
                        if end != len(self.buffer):
                            raise ValueError("invalid JSON value token boundary")
                    else:
                        if len(self.buffer) - self.position > self.max_value_chars:
                            raise TraceLimitError("JSON value size limit exceeded")
                        self.more(); continue
                self.position = end
                return value
            except json.JSONDecodeError as error:
                if self.eof:
                    raise ValueError("invalid or truncated JSON value") from error
                if len(self.buffer) - self.position > self.max_value_chars:
                    raise TraceLimitError("JSON value size limit exceeded") from error
                self.more()

    def read_envelope(self, accept_event):
        self.take("{"); keys = set(); metadata = None; has_events = False
        if self.peek() != "}":
            while True:
                key = self.value()
                if not isinstance(key, str) or key in keys: raise ValueError("invalid or duplicate envelope key")
                if len(keys) >= 128: raise TraceLimitError("JSON envelope key limit exceeded")
                keys.add(key); self.take(":")
                if key == "traceEvents":
                    has_events = True; self.take("[")
                    if self.peek() != "]":
                        while True:
                            accept_event(self.value())
                            if self.peek() == "]": break
                            self.take(",")
                    self.take("]")
                else:
                    value = self.value()
                    if key == "jceTrace": metadata = value
                if self.peek() == "}": break
                self.take(",")
        self.take("}")
        if self.peek(): raise ValueError("trailing JSON data")
        if not has_events: raise ValueError("capture metadata and traceEvents are required")
        return metadata

    def finish_hash(self):
        # Invalid input cannot pass. Still retain a full raw identity without
        # claiming that unread JSON/UTF-8 was validated after an early failure.
        while True:
            raw = self.stream.read(self.chunk_bytes)
            if not raw: break
            self.digest.update(raw); self.bytes_read += len(raw)
        return self.digest.hexdigest()


def _fingerprint(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def analyze_trace_file(path, required, *, require_wait=(), require_counter_zero=(), require_counter_last_zero=(),
                       chunk_bytes=65536, max_value_chars=65536, max_history=200000, max_active=8192, max_names=4096,
                       resource_observations=False, renderer_submissions=False, gpu_view_observations=False,
                       gpu_submission_joins=False, render_pipeline_observations=False, readback_operations=False,
                       cleanup_ownership=False, screenshot_requests=False, offscreen_configurations=False, filesystem_read_details=False, shader_provenance=False, terminal_session_binding=False,
                       history_db_path=None, disk_history_max_records=None, disk_history_max_bytes=None,
                       scope_witnesses=False, scope_prefixes=()):
    """Complete file validation and a second full byte fingerprint; bounded state.

    No phase/counter event is skipped. The historical identity cap is explicit:
    large unique task histories must be rejected, never evicted or prefix-PASSed.
    Explicit history_db_path plus both disk budgets moves exact task/completion/
    terminal-session history to one fresh disposable SQLite index. Other RAM
    limits and complete event/metadata/two-fingerprint validation stay unchanged.
    """
    if type(scope_witnesses) is not bool:
        raise ValueError("scope_witnesses must be an explicit bool")
    scope_prefixes = _scope_prefixes(scope_prefixes)
    history_config = None
    if any(value is not None for value in (history_db_path, disk_history_max_records, disk_history_max_bytes)):
        if __package__:
            from .trace_history_storage import history_options, SQLiteHistory
        else:
            from trace_history_storage import history_options, SQLiteHistory
        history_config = history_options(history_db_path, disk_history_max_records, disk_history_max_bytes)
    limits = {"history": max_history, "active": max_active, "names": max_names}
    for value in (chunk_bytes, max_value_chars, *limits.values()):
        if type(value) is not int or value <= 0: raise ValueError("validation limits must be positive integers")
    if chunk_bytes > 1048576 or max_value_chars > 1048576:
        raise TraceLimitError("JSON read/value limit exceeds 1 MiB ceiling")
    path = Path(path)
    with path.open("rb") as stream:
        initial = _fingerprint(os.fstat(stream.fileno()))
        initial_path = _fingerprint(path.stat())
        # Windows fstat/path.stat may expose different ctime semantics. Compare
        # each source with its own baseline, and common identity/size/mtime here.
        opened_path_matches = initial[:4] == initial_path[:4]
        reader = _JsonStream(stream, chunk_bytes, max_value_chars)
        validation_error = None
        syntax_complete = False
        report = None
        history_owner = None
        try:
            if history_config:
                history_owner = SQLiteHistory(**history_config)
            analysis = _TraceAnalysis(required, require_counter_zero, require_counter_last_zero,
                                      require_wait=require_wait, limits=limits, resource_observations=resource_observations,
                                      renderer_submissions=renderer_submissions, gpu_view_observations=gpu_view_observations,
                                      gpu_submission_joins=gpu_submission_joins,
                                      render_pipeline_observations=render_pipeline_observations, readback_operations=readback_operations,
                                      cleanup_ownership=cleanup_ownership, screenshot_requests=screenshot_requests,
                                      offscreen_configurations=offscreen_configurations,
                                      filesystem_read_details=filesystem_read_details, shader_provenance=shader_provenance,
                                      terminal_session_binding=terminal_session_binding, history_owner=history_owner,
                                      scope_witnesses=scope_witnesses, scope_prefixes=scope_prefixes)
            metadata = reader.read_envelope(analysis.feed)
            syntax_complete = True
            report = analysis.finish(metadata)
        except (ValueError, TypeError, KeyError, OverflowError, RecursionError) as error:
            validation_error = error
        finally:
            if history_owner is not None:
                try:
                    history_owner.close()
                except ValueError as error:
                    validation_error = validation_error or error
        # Even rejected validation retains the full raw fingerprint. Reading
        # the remainder is identity accounting, never post-error JSON proof.
        parsed_sha = reader.finish_hash()
        parsed_bytes = reader.bytes_read
        before_stable = (opened_path_matches and _fingerprint(os.fstat(stream.fileno())) == initial and
                         _fingerprint(path.stat()) == initial_path)
        stream.seek(0); after = hashlib.sha256(); size = 0
        for raw in iter(lambda: stream.read(chunk_bytes), b""):
            after.update(raw); size += len(raw)
        stable = (before_stable and after.hexdigest() == parsed_sha and size == parsed_bytes and
                  _fingerprint(os.fstat(stream.fileno())) == initial and _fingerprint(path.stat()) == initial_path)
        evidence = dict(reader_mode="bounded-stream", json_utf8_eof_validated=syntax_complete,
                        sha256=parsed_sha, source_sha256_after=after.hexdigest(), input_bytes=parsed_bytes,
                        source_fingerprint_stable=stable,
                        source_handle_fingerprint=list(initial), source_path_fingerprint=list(initial_path),
                        fingerprint_fields=["device", "inode", "size", "mtime_ns", "ctime_ns"],
                        fingerprint_scope="full first-read bytes vs full second read plus before/after file identity; not an atomic filesystem snapshot",
                        json_value_char_limit=max_value_chars, read_chunk_bytes=chunk_bytes)
        if history_owner is not None:
            evidence["history_storage"] = history_owner.evidence()
        elif getattr(validation_error, "history_evidence", None) is not None:
            evidence["history_storage"] = validation_error.history_evidence
        if not stable:
            raise TraceFileError("trace full source fingerprint changed during or after validation", evidence)
        if validation_error is not None:
            raise TraceFileError(validation_error, evidence) from validation_error
        report.update(evidence)
        report["validation_complete"] = True
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--require", action="append", required=True,
                        help="require a successful submitted service task (repeatable)")
    parser.add_argument("--require-wait", action="append", default=[],
                        help="require a complete CPU wait B/E pair (repeatable)")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--require-counter-zero", action="append", default=[])
    parser.add_argument("--require-counter-last-zero", action="append", default=[])
    parser.add_argument("--resource-observations", action="store_true",
                        help="require and validate bounded engine.app_resources schema-1 samples")
    parser.add_argument("--renderer-submissions", action="store_true",
                        help="require and validate typed renderer.frame_advance_cpu submission packets")
    parser.add_argument("--gpu-view-observations", action="store_true",
                        help="require typed latched GPU view packets within renderer.submit_cpu; no CPU/GPU frame join")
    parser.add_argument("--gpu-submission-joins", action="store_true",
                        help="require an exact sampled D3D11 GPU source-frame identity; no GPU completion or OS Present proof")
    parser.add_argument("--render-pipeline-observations", action="store_true",
                        help="require complete typed render-pipeline policy publications")
    parser.add_argument("--readback-operations", action="store_true",
                        help="observe per-capture readback CPU ownership/ready serials; not GPU completion")
    parser.add_argument("--offscreen-configurations", action="store_true",
                        help="require complete offscreen configure packets; empty shared view scopes stay unclassified")
    parser.add_argument("--filesystem-read-details", action="store_true",
                        help="observe bounded existing fs.read packets; optionality/errno remain unavailable")
    parser.add_argument("--shader-provenance", action="store_true",
                        help="observe typed eager-loader CPU buffer/create/link/creator-release receipts")
    parser.add_argument("--cleanup-ownership", action="store_true",
                        help="require paired cleanup outcomes and final captured success in a complete continuous CPU trace")
    parser.add_argument("--terminal-session-binding", action="store_true",
                        help="collect bounded exact terminal-session witnesses; requires --cleanup-ownership")
    parser.add_argument("--screenshot-requests", action="store_true",
                        help="validate screenshot admission/unique terminal ledger; no requests reports NO_ACTIVITY")
    parser.add_argument("--scope-witnesses", action="store_true",
                        help="retain bounded first/last absolute CPU intervals per required service; not a full timeline")
    parser.add_argument("--scope-prefix", action="append", default=[],
                        help="observe inclusive CPU scope timings per name/thread for a prefix (repeatable); no matches is NO_ACTIVITY")
    parser.add_argument("--history-db", type=Path, help="fresh absolute disposable exact-history database; requires both disk budgets")
    parser.add_argument("--disk-history-max-records", type=int)
    parser.add_argument("--disk-history-max-bytes", type=int)
    parser.add_argument("--max-history", type=int, default=200000)
    parser.add_argument("--max-active", type=int, default=8192)
    parser.add_argument("--max-names", type=int, default=4096)
    args = parser.parse_args()
    try:
        scope_prefixes = _scope_prefixes(args.scope_prefix)
    except ValueError as error:
        parser.error(str(error))
    try:
        report = analyze_trace_file(args.trace, args.require,
            require_wait=args.require_wait,
            require_counter_zero=args.require_counter_zero, require_counter_last_zero=args.require_counter_last_zero,
            max_history=args.max_history, max_active=args.max_active, max_names=args.max_names,
            resource_observations=args.resource_observations, renderer_submissions=args.renderer_submissions,
            gpu_view_observations=args.gpu_view_observations, gpu_submission_joins=args.gpu_submission_joins,
            render_pipeline_observations=args.render_pipeline_observations,
            readback_operations=args.readback_operations, cleanup_ownership=args.cleanup_ownership,
            screenshot_requests=args.screenshot_requests, offscreen_configurations=args.offscreen_configurations,
            filesystem_read_details=args.filesystem_read_details, shader_provenance=args.shader_provenance,
            terminal_session_binding=args.terminal_session_binding, history_db_path=args.history_db,
            disk_history_max_records=args.disk_history_max_records, disk_history_max_bytes=args.disk_history_max_bytes,
            scope_witnesses=args.scope_witnesses, scope_prefixes=scope_prefixes)
    except (ValueError, TypeError, KeyError, OverflowError, RecursionError, OSError) as error:
        report = {"status": "INVALID_OR_INCOMPLETE_CAPTURE", "error": str(error),
                  "reader_mode": "bounded-stream", "validation_complete": False}
        if isinstance(error, TraceFileError):
            report.update(error.evidence)
    report["trace"] = str(args.trace.resolve())
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8", newline="\n")
    print(report["status"])
    return 0 if report["status"] == "REQUIRED_CPU_LIFECYCLE_SCOPES_OBSERVED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
