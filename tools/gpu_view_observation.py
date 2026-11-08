"""Bounded latched GPU-view observations fed by the canonical trace owner.

This module owns no JSON reader, B/E pairing, backend query or frame join.
Reported provider intervals are not presentation, fences or GPU completion.
"""
from __future__ import annotations

SERVICE = "renderer.submit_cpu"
PREFIX = "gpu.view."
FIELDS = ("device_epoch", "api_frame", "submission_ordinal", "source_frame",
          "begin_ticks", "end_ticks", "timer_frequency", "interval_available", "reported_ticks")
SIGNATURE = ("device_epoch", "source_frame", "begin_ticks", "end_ticks", "timer_frequency")
UINT16_MAX, UINT32_MAX = (1 << 16) - 1, (1 << 32) - 1
INT64_MIN, INT64_MAX = -(1 << 63), (1 << 63) - 1


def _uint64(value):
    if type(value) is not int or not 0 <= value < 1 << 64:
        raise ValueError("GPU view identity is not an exact uint64")
    return value


def _exact(value):
    if type(value) is int and abs(value) > (1 << 53) - 1:
        return str(value)
    if isinstance(value, dict):
        return {key: _exact(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_exact(item) for item in value]
    return value


class GpuViewObservation:
    """Nine fields/view, bounded active records and known-(epoch, view) rows."""
    def __init__(self, max_active=8192, max_groups=4096):
        if any(type(v) is not int or v <= 0 for v in (max_active, max_groups)):
            raise ValueError("GPU view limits must be positive integers")
        self.max_active, self.max_groups = max_active, max_groups
        self.active, self.rows = {}, {}
        self.active_views = self.count = self.available = self.unavailable = 0
        self.unknown = self.repeats = self.zeros = self.scopes = self.empty_scopes = 0
        self.unknown_barrier = 0
        self.first = self.last = None

    def begin(self, event, index, start):
        args, owner = event["args"], start[3]
        identity, parent, tid = _uint64(args["id"]), _uint64(args["parent"]), _uint64(event["tid"])
        if (event["cat"] != "task" or not identity or identity != parent or
                owner[0] != 2 or owner[1] != SERVICE or owner[2] > event["ts"]):
            raise ValueError("GPU views require a submitted renderer.submit_cpu service")
        _uint64(owner[3])
        key = ("task", tid, identity)
        if key in self.active or len(self.active) >= self.max_active:
            raise ValueError("GPU view active scope limit or duplicate owner")
        self.active[key] = dict(begin_raw_index=index, begin_timestamp_us=event["ts"],
                                last_timestamp_us=event["ts"], views={})

    def counter(self, event, index, canonical_active):
        name = event.get("name")
        if not isinstance(name, str) or not name.startswith(PREFIX):
            return
        address = name[len(PREFIX):].split(".")
        if (len(address) != 2 or not address[0].isascii() or not address[0].isdecimal() or
                len(address[0]) > 5):
            raise ValueError("invalid GPU view counter address")
        view, field = int(address[0]), address[1]
        if view > UINT16_MAX or str(view) != address[0] or field not in FIELDS:
            raise ValueError("unknown GPU view ID or field")
        args = event["args"]
        identity, parent, tid = _uint64(args.get("id")), _uint64(args.get("parent")), _uint64(event["tid"])
        key = ("task", tid, identity)
        start, packet = canonical_active.get(key), self.active.get(key)
        if (event["cat"] != "render" or not identity or identity != parent or start is None or
                start[0] != SERVICE or start[2] != identity or packet is None):
            raise ValueError("GPU view counter has wrong active owner/thread/category")
        if index <= packet["begin_raw_index"] or event["ts"] < packet["last_timestamp_us"]:
            raise ValueError("GPU view counter is outside ordered scope interval")
        record = packet["views"].get(view)
        if record is None:
            if self.active_views >= self.max_active:
                raise ValueError("GPU view active record limit exceeded")
            record = dict(values={}, raw_indices={})
            packet["views"][view] = record
            self.active_views += 1
        values = record["values"]
        if field in values:
            raise ValueError("duplicate GPU view field: " + field)
        if field != FIELDS[len(values)]:
            raise ValueError("GPU view fields are out of producer order")
        value = args.get("value")
        if type(value) is not int:
            raise ValueError("GPU view value must be an exact integer")
        if field == "device_epoch":
            valid = value == -1 or 1 <= value <= INT64_MAX
        elif field in ("api_frame", "submission_ordinal", "source_frame"):
            valid = 0 <= value <= UINT32_MAX
        elif field == "interval_available":
            valid = value in (0, 1)
        else:
            valid = INT64_MIN <= value <= INT64_MAX
        if not valid:
            raise ValueError("GPU view value outside typed range: " + field)
        values[field], record["raw_indices"][field] = value, index
        packet["last_timestamp_us"] = event["ts"]

    def end(self, event, index, scope):
        key = (event["cat"], event["tid"], event["args"]["id"])
        packet = self.active.pop(key, None)
        if packet is None:
            raise ValueError("GPU view scope closed without active owner")
        self.scopes += 1
        views = packet["views"]
        self.active_views -= len(views)
        if not views:
            # Cadence, no-view and unavailable backend scopes legitimately emit
            # no view records. Canonical task outcomes remain in the report.
            self.empty_scopes += 1
            return
        if (scope["category"] != "task" or scope["task_kind"] != 2 or
                scope["id"] != scope["owner"] or scope["state"] != 0):
            raise ValueError("GPU view service did not complete successfully")
        if event["ts"] < packet["last_timestamp_us"]:
            raise ValueError("GPU view fields are outside closing scope interval")
        shared = None
        for view, record in views.items():
            values = record["values"]
            if len(values) != len(FIELDS) or index <= max(record["raw_indices"].values()):
                raise ValueError("GPU view scope closed without nine unique fields")
            common = tuple(values[field] for field in ("device_epoch", "api_frame", "submission_ordinal", "timer_frequency"))
            if shared is not None and shared != common:
                raise ValueError("GPU views disagree on their shared submission snapshot")
            shared = common
            freq, begin, end = (values[field] for field in ("timer_frequency", "begin_ticks", "end_ticks"))
            available = freq > 0 and begin >= 0 and end >= begin and (begin != 0 or end != 0)
            ticks = end - begin if available else -1
            if values["interval_available"] != int(available) or values["reported_ticks"] != ticks:
                raise ValueError("GPU view interval flag/reported ticks disagree with raw clock formula")
            self._observe(view, record, packet, event, index, scope, available,
                          ticks * 1000.0 / freq if available else None)

    def _observe(self, view, record, packet, event, index, scope, available, duration):
        values = record["values"]
        known = values["device_epoch"] != -1
        repeated = False
        self.count += 1
        self.available += available
        self.unavailable += not available
        self.zeros += available and values["reported_ticks"] == 0
        if known:
            key = (values["device_epoch"], view)
            row = self.rows.get(key)
            if row is None:
                if len(self.rows) >= self.max_groups:
                    raise ValueError("GPU view aggregate group limit exceeded")
                row = dict(device_epoch=key[0], view_id=view, samples_count=0, available_samples_count=0,
                    unavailable_samples_count=0, zero_duration_samples_count=0,
                    adjacent_repeated_samples_count=0, duration_samples_count=0,
                    duration_ms_sum=0.0, duration_ms_min=None, duration_ms_max=None, last_signature=None)
                self.rows[key] = row
            signature = dict(view_id=view, **{field: values[field] for field in SIGNATURE})
            repeated = (row["last_signature"] == signature and
                        row.get("unknown_barrier") == self.unknown_barrier)
            row["unknown_barrier"] = self.unknown_barrier
            row["last_signature"] = signature
            row["last_api_frame"] = values["api_frame"]
            row["last_submission_ordinal"] = values["submission_ordinal"]
            row["samples_count"] += 1
            row["available_samples_count"] += available
            row["unavailable_samples_count"] += not available
            row["zero_duration_samples_count"] += available and values["reported_ticks"] == 0
            row["adjacent_repeated_samples_count"] += repeated
            if available and not repeated:
                row["duration_samples_count"] += 1
                row["duration_ms_sum"] += duration
                row["duration_ms_min"] = duration if row["duration_ms_min"] is None else min(row["duration_ms_min"], duration)
                row["duration_ms_max"] = duration if row["duration_ms_max"] is None else max(row["duration_ms_max"], duration)
        else:
            # -1 is not an identity. Do not mix unknown contexts or compare
            # their timestamps as if they described the same device/view.
            self.unknown += 1
            self.unknown_barrier += 1
        self.repeats += repeated
        witness = dict(scope_id=scope["id"], parent=scope["owner"], thread=scope["thread"], view_id=view,
            begin_raw_index=packet["begin_raw_index"], end_raw_index=index,
            begin_timestamp_us=packet["begin_timestamp_us"], end_timestamp_us=event["ts"],
            field_raw_indices=record["raw_indices"], values=values, device_epoch_known=known,
            duration_ms=duration, adjacent_repeated_known_view_signature=repeated)
        if self.first is None:
            self.first = witness
        self.last = witness

    def finish(self):
        if self.active or self.active_views:
            raise ValueError("GPU view packets have unfinished scopes")
        if not self.count:
            raise ValueError("GPU view observations requested but no view packets were captured")
        rows = []
        for key in sorted(self.rows):
            row = dict(self.rows[key])
            row.pop("unknown_barrier")
            row["duration_ms_mean"] = row["duration_ms_sum"] / row["duration_samples_count"] if row["duration_samples_count"] else None
            if not row["duration_samples_count"]:
                row["duration_ms_sum"] = None
            rows.append(row)
        return _exact(dict(schema=1, status="GPU_VIEW_PACKETS_OBSERVED" if self.available else "GPU_VIEW_TIMING_UNAVAILABLE",
            samples_count=self.count, available_samples_count=self.available, unavailable_samples_count=self.unavailable,
            zero_duration_samples_count=self.zeros, unknown_epoch_samples_count=self.unknown,
            adjacent_repeated_known_view_samples_count=self.repeats,
            submission_scopes_observed=self.scopes, submission_scopes_without_view_packets=self.empty_scopes,
            first=self.first, last=self.last, views=rows,
            validation_limits=dict(active_scopes_and_view_records=self.max_active, known_epoch_view_groups=self.max_groups),
            integer_encoding="exact integers; values wider than 53 bits are decimal strings",
            repeat_scope="previous exported observation for the same known (device_epoch, view_id) only; source frame, raw ticks and frequency compared; no global uniqueness history; unknown epochs never grouped or compared and break comparison continuity",
            duration_scope="available reported provider intervals only; mean/sum exclude adjacent repeated signatures, not a per-frame average or global deduplication; unavailable duration is null, valid zero remains zero",
            scope="latched per-view source frames and raw provider clocks; no CPU/CSV frame join, cross-view frame equality, full-frame coverage, presentation, fence or GPU completion"))
