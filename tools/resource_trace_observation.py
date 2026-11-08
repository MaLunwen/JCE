"""Bounded resource-schema consumer; receives canonical parser events, never JSON.

All memory/async values are sampled owner-thread snapshots, not complete allocator
coverage, OS thread rates or lifetime maxima. GPU values are latched observations,
not joined CPU/GPU frames, presentation FPS, fences or GPU completion.
"""
from __future__ import annotations
import math

SERVICE = "engine.app_resources"
PREFIX = "resource."
FLAGS = ("host_headless pid_available host_role_available clock_rollback memory.copied "
         "memory.rss_available memory.commit_available tagged.partial_scope async.available "
         "gpu.query_allowed gpu.copied gpu.context_available gpu.backend_available "
         "gpu.timer_available gpu.reason_available").split()
SCALARS = ("schema async.mode async.worker_count async.live_tasks async.queued_tasks "
           "async.running_tasks async.pending_completions gpu.reason gpu.read_host_frame gpu.source_frame").split()
WIDE = ("sample_ns owner_thread_id memory.rss_bytes memory.commit_bytes tagged.current_bytes "
        "tagged.peak_bytes async.submitted_tasks async.failed_tasks async.queue_time_ns async.run_time_ns "
        "gpu.context_generation gpu.frequency gpu.raw_begin gpu.raw_end").split()
FIELDS = frozenset(FLAGS + SCALARS + [name + half for name in WIDE for half in (".hi32", ".lo32")])
MAX_ACTIVE = 8


def _uint(value, bits=32):
    if type(value) is not int or not 0 <= value < 1 << bits:
        raise ValueError("resource counter/identity is not an exact uint" + str(bits))
    return value


def _signed(value):
    return value - (1 << 64) if value >= 1 << 63 else value


def _exact(value):
    # JSON numbers wider than JS's exact integer range are decimal strings.
    if type(value) is int and abs(value) > (1 << 53) - 1:
        return str(value)
    if isinstance(value, dict):
        return {key: _exact(item) for key, item in value.items()}
    return value


class ResourceTraceObservation:
    """Only fixed schema fields, <=8 active records, two witnesses and one GPU tuple."""
    def __init__(self):
        self.active = {}
        self.count = self.repeats = 0
        self.first = self.last = self.gpu_tuple = None
        self.rows = {name: {"known_count": 0, "unknown_count": 0, "sampled_min": None, "sampled_max": None}
                     for name in FLAGS + SCALARS + WIDE + ["gpu.duration_ms"]}

    def submit(self, event, index):
        args = event["args"]
        identity = _uint(args["task"], 64)
        tid = _uint(event["tid"], 64)
        # The canonical report's finish validates the complete named, single
        # cooperative zero-thread authority before publishing this consumer.
        if not identity or args["kind"] != 2 or event["cat"] != "task":
            raise ValueError("resource observation requires a paired service owner")
        if identity in self.active or len(self.active) >= MAX_ACTIVE:
            raise ValueError("resource active sample limit or duplicate submission")
        self.active[identity] = dict(tid=tid, parent=_uint(args["parent"], 64),
                                     submitted_index=index, begin_index=None, values={})

    def begin(self, event, index):
        args = event["args"]
        identity = _uint(args["id"], 64)
        sample = self.active.get(identity)
        if (sample is None or sample["begin_index"] is not None or event["cat"] != "task" or
            args["parent"] != identity or event["tid"] != sample["tid"]):
            raise ValueError("resource begin has the wrong submitted owner/thread")
        sample["begin_index"] = index

    def counter(self, event, index):
        name = event.get("name")
        if not isinstance(name, str) or not name.startswith(PREFIX):
            return
        args = event["args"]
        sample = self.active.get(_uint(args.get("id"), 64))
        key = name[len(PREFIX):]
        if (event["cat"] != "core" or sample is None or sample["begin_index"] is None or
            event["tid"] != sample["tid"] or _uint(args.get("parent"), 64) != sample["parent"]):
            raise ValueError("resource counter has the wrong active owner/parent/thread")
        if key not in FIELDS or key in sample["values"]:
            raise ValueError("resource counter schema has an unknown or repeated field")
        sample["values"][key] = _uint(args.get("value"))

    def end(self, event, index, scope):
        identity = event["args"]["id"]
        sample = self.active.pop(identity, None)
        if (sample is None or sample["begin_index"] is None or scope["category"] != "task" or
            scope["task_kind"] != 2 or scope["owner"] != identity or scope["state"] != 0 or
            scope["thread"] != sample["tid"] or scope["submitted_parent"] != sample["parent"]):
            raise ValueError("resource scope is not a successful paired self-owned service")
        raw = sample["values"]
        if set(raw) != FIELDS:
            raise ValueError("resource sample is missing fixed schema counter fields")
        values = {name: raw[name] for name in FLAGS + SCALARS}
        values.update({name: (raw[name + ".hi32"] << 32) | raw[name + ".lo32"] for name in WIDE})
        for name in ("gpu.frequency", "gpu.raw_begin", "gpu.raw_end"):
            values[name] = _signed(values[name])
        if values["schema"] != 1 or any(values[name] not in (0, 1) for name in FLAGS):
            raise ValueError("resource schema version or availability flag is invalid")
        if values["owner_thread_id"] != sample["tid"] or not values["tagged.partial_scope"]:
            raise ValueError("resource owner thread or tagged partial scope is invalid")
        if values["pid_available"] or values["host_role_available"]:
            raise ValueError("resource schema 1 has no authoritative PID/host-role observation")
        for name in ("rss", "commit"):
            available = values["memory." + name + "_available"]
            value = values["memory." + name + "_bytes"]
            if available != int(bool(values["memory.copied"] and value > 0)) or (not available and value):
                raise ValueError("resource memory availability disagrees with its payload")
        if values["async.mode"] not in (0, 1, 2):
            raise ValueError("resource async mode is invalid")
        if not values["async.available"] and any(values[name] for name in values
                if name.startswith("async.") and name != "async.available"):
            raise ValueError("resource unavailable async snapshot contains counters")
        copied, context, backend, timer = [values["gpu." + field] for field in
                                          ("copied", "context_available", "backend_available", "timer_available")]
        if copied > values["gpu.query_allowed"] or context > copied or backend > context or timer > backend:
            raise ValueError("resource GPU availability hierarchy is invalid")
        if values["gpu.reason_available"] != copied or values["gpu.reason"] not in range(5):
            raise ValueError("resource GPU reason availability is invalid")
        if not copied and values["gpu.reason"]:
            raise ValueError("resource unknown GPU reason is not zero")
        if not context and any(values[name] for name in ("gpu.context_generation", "gpu.read_host_frame")):
            raise ValueError("resource unavailable context contains identity")
        if context and not values["gpu.context_generation"]:
            raise ValueError("resource available context has no generation")
        if not backend and any(values[name] for name in ("gpu.source_frame", "gpu.frequency", "gpu.raw_begin", "gpu.raw_end")):
            raise ValueError("resource unavailable backend contains raw timing")
        freq, begin, end = [values["gpu." + field] for field in ("frequency", "raw_begin", "raw_end")]
        interval = freq > 0 and begin >= 0 and end > begin
        reason = 1 if not context else 2 if not backend else 3 if freq <= 0 else 4 if not interval else 0
        if copied and (values["gpu.reason"] != reason or timer != int(reason == 0)):
            raise ValueError("resource available GPU frequency/interval or unavailable reason is invalid")
        values["gpu.duration_ms"] = (end - begin) * 1000.0 / freq if timer else None
        if timer and (not math.isfinite(values["gpu.duration_ms"]) or values["gpu.duration_ms"] <= 0):
            raise ValueError("resource GPU duration is invalid")
        known = {name: True for name in values}
        known["memory.rss_bytes"] = bool(values["memory.rss_available"])
        known["memory.commit_bytes"] = bool(values["memory.commit_available"])
        for name in values:
            if name.startswith("async.") and name != "async.available": known[name] = bool(values["async.available"])
        for name in ("gpu.context_generation", "gpu.read_host_frame"): known[name] = bool(context)
        for name in ("gpu.source_frame", "gpu.frequency", "gpu.raw_begin", "gpu.raw_end"): known[name] = bool(backend)
        known["gpu.reason"] = bool(copied)
        known["gpu.duration_ms"] = bool(timer)
        known["owner_thread_id"] = True
        for name, value in values.items():
            row = self.rows[name]
            row["known_count" if known[name] else "unknown_count"] += 1
            if known[name]:
                row["sampled_min"] = value if row["sampled_min"] is None else min(row["sampled_min"], value)
                row["sampled_max"] = value if row["sampled_max"] is None else max(row["sampled_max"], value)
        repeated = False
        if timer:
            key = tuple(values[name] for name in ("gpu.context_generation", "gpu.source_frame", "gpu.frequency", "gpu.raw_begin", "gpu.raw_end"))
            repeated = key == self.gpu_tuple
            self.repeats += repeated
            self.gpu_tuple = key
        witness = dict(submitted_raw_index=sample["submitted_index"], begin_raw_index=sample["begin_index"],
                       end_raw_index=index, id=identity, submitted_parent=sample["parent"], thread=sample["tid"],
                       end_timestamp_us=event["ts"], repeated_previous_available_gpu_tuple=repeated,
                       values={name: value if known[name] else None for name, value in values.items()})
        if self.first is None: self.first = witness
        self.last = witness
        self.count += 1

    def finish(self):
        if self.active: raise ValueError("resource samples have unfinished submission/scope pairs")
        if not self.count: raise ValueError("resource observations requested but no resource samples were captured")
        return _exact(dict(schema=1, samples_count=self.count, first=self.first, last=self.last,
            sampled_fields=self.rows, repeated_previous_available_gpu_tuple_count=self.repeats,
            active_sample_cap=MAX_ACTIVE, wide_integer_encoding="decimal strings beyond JS exact integer range",
            scope="sampled RSS/commit vs partial tagged allocations; async snapshots, not OS thread rates; latched GPU raw intervals, no CPU/GPU join, presentation FPS, fence or GPU completion",
            gpu_raw_scope="backend-available frequency/ticks are raw signed observations, including invalid zero/negative values; only timer-available duration is measured, unavailable duration is null",
            repeat_scope="compared only with the previous available GPU tuple; all samples retained in counts, no arbitrary identity history"))
