"""Bounded typed CPU submission observations fed by the canonical trace owner.

No JSON reader or alternate B/E pairing lives here. Counters describe the CPU
call boundary, not GPU completion, presentation or a join to latched GPU data.
"""
from __future__ import annotations

SERVICE = "renderer.frame_advance_cpu"
TAIL = "engine.renderer_tail_cpu"
WRAPPER = "renderer.submit_cpu"
PREFIX = "renderer.submit."
CLOCK_PREFIX = "renderer.submit_clock."
TAIL_CLOCK_PREFIX = "engine.renderer_tail_clock."
FIELDS = ("context_generation", "backend_serial_before", "backend_serial_after", "ordinal_before")
CLOCK_FIELDS = ("frequency", "begin", "end")
UINT32_MAX = (1 << 32) - 1
INT64_MAX = (1 << 63) - 1


def _uint64(value):
    if type(value) is not int or not 0 <= value < 1 << 64:
        raise ValueError("renderer submission identity is not an exact uint64")
    return value


def _exact(value):
    # Keep wide identities/generations exact for JSON consumers using doubles.
    if type(value) is int and abs(value) > (1 << 53) - 1:
        return str(value)
    if isinstance(value, dict):
        return {key: _exact(item) for key, item in value.items()}
    return value


class RendererSubmissionObservation:
    """Submission identity and optional raw counter edges on one canonical scope."""
    def __init__(self, max_active=8192):
        self.max_active = max_active
        self.active = {}
        self.count = self.known = self.unknown = self.wraps = 0
        self.first = self.last = None
        self.clock_count = self.tail_count = self.joined_count = 0
        self.tails, self.wrappers = {}, {}
        self.first_tail = self.last_tail = None

    def begin(self, event, index, start):
        args = event["args"]
        identity, parent = _uint64(args["id"]), _uint64(args["parent"])
        tid = _uint64(event["tid"])
        owner = start[3]
        name = event["name"]
        key = ("task", tid, identity)
        tail = self.tails.get(("task", tid, owner[3]))
        if name == WRAPPER and tail is None:
            return
        if (event["cat"] != "task" or identity != parent or not identity or
            owner[0] != 2 or owner[1] != name or owner[2] > event["ts"]):
            raise ValueError("renderer submission requires a submitted service scope")
        if len(self.active) + len(self.tails) + len(self.wrappers) >= self.max_active:
            raise ValueError("renderer submission active limit")
        if name == WRAPPER:
            if key in self.wrappers or tail.get("wrapper") is not None:
                raise ValueError("duplicate renderer wrapper for one engine tail")
            tail["wrapper"] = identity
            self.wrappers[key] = tail
            return
        if name == SERVICE:
            wrapper = self.wrappers.get(("task", tid, owner[3]))
            if wrapper is not None:
                if "open_advance" in wrapper or "advance" in wrapper:
                    raise ValueError("multiple API advances in one renderer wrapper")
                wrapper["open_advance"] = identity
        observations = self.tails if name == TAIL else self.active
        if key in observations:
            raise ValueError("duplicate renderer submission scope")
        observations[key] = dict(begin_raw_index=index, begin_timestamp_us=event["ts"],
            last_timestamp_us=event["ts"], values={}, raw_indices={},
            clock_values={}, clock_indices={})

    def counter(self, event, index, canonical_active):
        name = event.get("name")
        if not isinstance(name, str) or not name.startswith((PREFIX, CLOCK_PREFIX, TAIL_CLOCK_PREFIX)):
            return
        is_tail = name.startswith(TAIL_CLOCK_PREFIX)
        is_clock = is_tail or name.startswith(CLOCK_PREFIX)
        prefix = TAIL_CLOCK_PREFIX if is_tail else (CLOCK_PREFIX if is_clock else PREFIX)
        fields = CLOCK_FIELDS if is_clock else FIELDS
        field = name[len(prefix):]
        if field not in fields:
            raise ValueError("unknown renderer submission field: " + field)
        args = event["args"]
        identity, parent = _uint64(args.get("id")), _uint64(args.get("parent"))
        tid = _uint64(event["tid"])
        key = ("task", tid, identity)
        observations = self.tails if is_tail else self.active
        start, sample = canonical_active.get(key), observations.get(key)
        if (event["cat"] != "render" or not identity or identity != parent or
            start is None or start[0] != (TAIL if is_tail else SERVICE) or
            start[2] != identity or sample is None):
            raise ValueError("renderer submission counter has wrong active owner/thread/category")
        if index <= sample["begin_raw_index"] or event["ts"] < sample["last_timestamp_us"]:
            raise ValueError("renderer submission counter is outside ordered scope interval")
        values, indices = (sample["clock_values"], sample["clock_indices"]) if is_clock else (
            sample["values"], sample["raw_indices"])
        if field in values:
            raise ValueError("duplicate renderer submission field: " + field)
        if (is_clock and not is_tail and len(sample["values"]) != len(FIELDS)) or field != fields[len(values)]:
            raise ValueError("renderer submission fields are out of producer order")
        value = args.get("value")
        if type(value) is not int:
            raise ValueError("renderer submission value must be an exact integer")
        if is_clock:
            valid = (1 if field == "frequency" else 0) <= value <= INT64_MAX
        elif field == "context_generation":
            valid = value == -1 or 1 <= value <= INT64_MAX
        else:
            valid = 0 <= value <= UINT32_MAX
        if not valid:
            raise ValueError("renderer submission value outside typed range: " + field)
        values[field], indices[field] = value, index
        sample["last_timestamp_us"] = event["ts"]

    def end(self, event, index, scope):
        key = (event["cat"], event["tid"], event["args"]["id"])
        if scope["name"] == WRAPPER:
            tail = self.wrappers.pop(key, None)
            if tail is not None:
                if "open_advance" in tail:
                    raise ValueError("renderer wrapper closed before its nested API advance")
                tail["wrapper_closed"] = True
                tail["wrapper_state"] = scope["state"]
            return None
        if scope["name"] == TAIL:
            self.end_tail(key, index, event, scope)
            return None
        sample = self.active.pop(key, None)
        if sample is None or len(sample["values"]) != len(FIELDS):
            raise ValueError("renderer submission scope closed without four unique fields")
        if (scope["category"] != "task" or scope["task_kind"] != 2 or
            scope["id"] != scope["owner"] or scope["state"] != 0):
            raise ValueError("renderer submission service did not complete successfully")
        clock = sample["clock_values"]
        last_field_index = max(sample["clock_indices"].values() if clock else sample["raw_indices"].values())
        if event["ts"] < sample["last_timestamp_us"] or index <= last_field_index:
            raise ValueError("renderer submission fields are outside closing scope interval")
        interval = self.read_clock(sample)
        values = sample["values"]
        known = values["context_generation"] != -1
        wrapped = values["backend_serial_after"] < values["backend_serial_before"]
        witness = dict(scope_id=scope["id"], parent=scope["owner"], thread=scope["thread"],
            category=scope["category"], begin_raw_index=sample["begin_raw_index"], end_raw_index=index,
            begin_timestamp_us=sample["begin_timestamp_us"], end_timestamp_us=event["ts"],
            field_raw_indices=sample["raw_indices"], values=values, context_generation_known=known,
            backend_serial_decreased=wrapped,
            backend_serial_delta_modulo_uint32=(values["backend_serial_after"] - values["backend_serial_before"]) & UINT32_MAX)
        if interval is not None:
            witness["performance_counter"] = interval
            self.clock_count += 1
        tail = self.wrappers.get(("task", scope["thread"], scope["submitted_parent"]))
        if tail is not None:
            if tail.pop("open_advance", None) != scope["id"]:
                raise ValueError("renderer API advance did not close its admitted interval")
            tail["advance"] = witness
        self.count += 1
        self.known += known
        self.unknown += not known
        self.wraps += wrapped
        if self.first is None:
            self.first = witness
        self.last = witness
        return witness

    @staticmethod
    def read_clock(sample):
        clock = sample["clock_values"]
        if not clock:
            return None
        if len(clock) != len(CLOCK_FIELDS) or clock["end"] < clock["begin"]:
            raise ValueError("renderer submission clock is incomplete or reversed")
        return dict(domain="jce-performance-counter", frequency_hz=clock["frequency"],
                    begin=clock["begin"], end=clock["end"], field_raw_indices=sample["clock_indices"])

    def end_tail(self, key, index, event, scope):
        sample = self.tails.pop(key)
        if sample.get("wrapper") and not sample.get("wrapper_closed"):
            raise ValueError("engine renderer tail closed before its nested wrapper")
        interval = self.read_clock(sample)
        if interval is not None and (event["ts"] < sample["last_timestamp_us"]
                                     or index <= max(sample["clock_indices"].values())):
            raise ValueError("engine renderer tail clock is outside its scope")
        advance = sample.get("advance")
        api = advance.get("performance_counter") if advance else None
        if interval and api:
            if not (interval["frequency_hz"] == api["frequency_hz"] and
                    interval["begin"] <= api["begin"] <= api["end"] <= interval["end"]):
                raise ValueError("renderer API clock outside engine tail clock")
            self.joined_count += 1
        witness = dict(scope_id=scope["id"], thread=scope["thread"], state=scope["state"],
            begin_raw_index=sample["begin_raw_index"], end_raw_index=index,
            performance_counter=interval, api_interval_available=bool(interval and api),
            wrapper_scope_id=sample.get("wrapper"), wrapper_state=sample.get("wrapper_state"),
            api_advance=advance)
        self.tail_count += 1
        if self.first_tail is None:
            self.first_tail = witness
        self.last_tail = witness

    def finish(self):
        if self.active or self.tails or self.wrappers:
            raise ValueError("unfinished renderer submission packets")
        if not self.count:
            raise ValueError("no renderer submission packets consumed")
        result = dict(schema=1, status="CPU_SUBMISSION_PACKETS_OBSERVED", packets_count=self.count,
            context_generation_known_count=self.known, context_generation_unknown_count=self.unknown,
            backend_serial_decreased_count=self.wraps, first=_exact(self.first), last=_exact(self.last),
            integer_encoding="exact integers; values wider than 53 bits are decimal strings",
            witness_order="first/last in exported scope-close order; every packet validated",
            scope="paired CPU submission call boundaries only; uint32 zero/wrap valid, -1 generation unknown; no GPU completion, presentation or CPU/GPU join")
        if self.clock_count:
            result["performance_counter_packet_count"] = self.clock_count
        if self.tail_count:
            result["engine_renderer_tails"] = dict(observed_count=self.tail_count,
                clock_joined_count=self.joined_count, first=_exact(self.first_tail), last=_exact(self.last_tail),
                scope="CPU engine tail contains the linked API advance; no GPU, wait actor or Present inference")
        return result
