"""Bounded readback CPU ownership packets from the canonical trace owner.

No JSON reader, B/E pairing, backend query or global completion inference.
"""
from __future__ import annotations

SERVICE = "readback.operation"
PREFIX = "readback.op."
FIELDS = ("operation_lo", "operation_hi", "capture_linked", "context_generation", "ticket", "stage",
          "ready_frame", "observed_frame", "observed_frame_known", "readback_ready_observed",
          "destination_released", "backend_stopped", "destination_bytes")
UINT32_MAX, INT64_MAX = (1 << 32) - 1, (1 << 63) - 1
STAGES = {1: "submit", 2: "ready_observed", 3: "cancel_intent", 4: "destination_release",
          5: "release_after_backend_stop"}


def _uint64(value):
    if type(value) is not int or not 0 <= value < 1 << 64:
        raise ValueError("readback scope identity must be an exact uint64")
    return value


def _exact(value):
    if type(value) is int and abs(value) > (1 << 53) - 1:
        return str(value)
    if isinstance(value, dict):
        return {key: _exact(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_exact(item) for item in value]
    return value


class ReadbackOperationObservation:
    def __init__(self, max_active=8192):
        if type(max_active) is not int or max_active < 1:
            raise ValueError("readback operation limit must be positive")
        self.max_active = max_active
        self.active, self.pending = {}, {}
        self.count = self.linked = self.unlinked = self.unknown_context = 0
        self.submits = self.ready = self.releases = self.stops = self.unready_stops = 0
        self.stage_counts = {stage: 0 for stage in STAGES}
        self.first = self.last = None

    def begin(self, event, index, start):
        args, owner = event["args"], start[3]
        identity, parent, tid = _uint64(args["id"]), _uint64(args["parent"]), _uint64(event["tid"])
        if (event["cat"] != "task" or not identity or identity != parent or
                owner[0] != 2 or owner[1] != SERVICE or owner[2] > event["ts"]):
            raise ValueError("readback operation needs a submitted CPU service")
        key = ("task", tid, identity)
        if key in self.active or len(self.active) + len(self.pending) >= self.max_active:
            raise ValueError("readback combined active/pending limit or duplicate scope")
        self.active[key] = dict(begin_raw_index=index, begin_timestamp_us=event["ts"],
                                last_timestamp_us=event["ts"], values={}, field_raw_indices={})

    def counter(self, event, index, canonical_active):
        name = event.get("name")
        if not isinstance(name, str) or not name.startswith(PREFIX):
            return
        field = name[len(PREFIX):]
        if field not in FIELDS:
            raise ValueError("unknown readback operation field")
        args = event["args"]
        identity, parent, tid = _uint64(args.get("id")), _uint64(args.get("parent")), _uint64(event["tid"])
        key = ("task", tid, identity)
        start, packet = canonical_active.get(key), self.active.get(key)
        if (event["cat"] != "render" or not identity or parent != identity or start is None or
                start[0] != SERVICE or start[2] != identity or packet is None):
            raise ValueError("readback counter has wrong active owner/thread/category")
        values = packet["values"]
        if field in values or field != FIELDS[len(values)]:
            raise ValueError("readback fields must be unique and in producer order")
        value = args.get("value")
        if type(value) is not int:
            raise ValueError("readback fields must be exact integers")
        if field == "context_generation":
            valid = value == -1 or 1 <= value <= INT64_MAX
        elif field == "destination_bytes":
            valid = 0 <= value <= INT64_MAX
        elif field in ("capture_linked", "observed_frame_known", "readback_ready_observed", "destination_released", "backend_stopped"):
            valid = value in (0, 1)
        elif field == "stage":
            valid = value in STAGES
        else:
            valid = 0 <= value <= UINT32_MAX
        if not valid or index <= packet["begin_raw_index"] or event["ts"] < packet["last_timestamp_us"]:
            raise ValueError("readback field outside typed range or ordered CPU interval")
        values[field], packet["field_raw_indices"][field] = value, index
        packet["last_timestamp_us"] = event["ts"]

    def _transition(self, values, scope):
        stage = values["stage"]
        operation = values["operation_lo"] | values["operation_hi"] << 32
        linked, ready = bool(values["capture_linked"]), bool(values["readback_ready_observed"])
        if linked != bool(operation) or (not linked and values["context_generation"] != -1):
            raise ValueError("readback capture linkage disagrees with operation/context")
        if values["destination_released"] != int(stage in (4, 5)) or values["backend_stopped"] != int(stage == 5):
            raise ValueError("readback stage contradicts destination/backend CPU ownership")
        if stage == 1 and (not linked or operation != scope["id"] or ready):
            raise ValueError("readback submit must own its operation without readiness")
        if stage == 2:
            delta = (values["observed_frame"] - values["ready_frame"]) & UINT32_MAX
            if not ready or not values["observed_frame_known"] or delta >= 1 << 31:
                raise ValueError("readback ready lacks known wrap-safe promised frame observation")
        if stage == 4 and not ready:
            raise ValueError("readback ordinary release lacks observed ready truth")
        if not linked:
            return operation
        key = operation, values["context_generation"], values["ticket"]
        if stage == 1:
            if key in self.pending or len(self.pending) + len(self.active) >= self.max_active:
                raise ValueError("readback pending operation limit or duplicate submit")
            self.pending[key] = dict(operation_id=operation, context_generation=key[1], ticket=key[2],
                ready_frame=values["ready_frame"], destination_bytes=values["destination_bytes"],
                ready_observed=False, cancel_intent_observed=False)
            self.submits += 1
            return operation
        prior = self.pending.get(key)
        if prior is None:
            raise ValueError("readback transition has no matching capture operation/context/ticket")
        if any(prior[field] != values[field] for field in ("ready_frame", "destination_bytes")):
            raise ValueError("readback operation promise or destination extent changed")
        if ready != (prior["ready_observed"] or stage == 2) or (stage == 2 and prior["ready_observed"]):
            raise ValueError("readback ready must be observed once and retained across transitions")
        if stage == 2:
            prior["ready_observed"] = True
            self.ready += 1
        elif stage == 3:
            prior["cancel_intent_observed"] = True
        elif stage == 4:
            if not prior["ready_observed"]:
                raise ValueError("readback release precedes captured ready observation")
            self.releases += 1
            del self.pending[key]
        elif stage == 5:
            self.stops += 1
            self.unready_stops += not prior["ready_observed"]
            del self.pending[key]
        return operation

    def end(self, event, index, scope):
        packet = self.active.pop((event["cat"], event["tid"], event["args"]["id"]), None)
        if packet is None or len(packet["values"]) != len(FIELDS):
            raise ValueError("readback CPU service closed without thirteen fields")
        if (scope["category"] != "task" or scope["task_kind"] != 2 or scope["id"] != scope["owner"] or
                scope["state"] != 0 or event["ts"] < packet["last_timestamp_us"] or
                index <= max(packet["field_raw_indices"].values())):
            raise ValueError("readback CPU observation did not complete in ordered scope")
        values = packet["values"]
        operation = self._transition(values, scope)
        self.count += 1
        self.linked += values["capture_linked"]
        self.unlinked += not values["capture_linked"]
        self.unknown_context += values["context_generation"] == -1
        self.stage_counts[values["stage"]] += 1
        witness = dict(scope_id=scope["id"], thread=scope["thread"], begin_raw_index=packet["begin_raw_index"],
            end_raw_index=index, begin_timestamp_us=packet["begin_timestamp_us"], end_timestamp_us=event["ts"],
            field_raw_indices=packet["field_raw_indices"], operation_id=operation,
            join_status="CAPTURE_OPERATION_LINKED" if values["capture_linked"] else "UNAVAILABLE_UNLINKED_CAPTURE",
            values=values)
        if self.first is None:
            self.first = witness
        self.last = witness

    def finish(self):
        if self.active:
            raise ValueError("unfinished readback CPU observation scopes")
        return _exact(dict(schema=1, status="READBACK_OPERATION_PACKETS_OBSERVED" if self.count else "NO_ACTIVITY",
            packets_count=self.count, linked_packets_count=self.linked, unlinked_packets_count=self.unlinked,
            unknown_context_packets_count=self.unknown_context, submitted_operations_count=self.submits,
            linked_ready_operations_observed_count=self.ready, ordinary_destination_releases_count=self.releases,
            backend_stopped_destination_releases_count=self.stops, backend_stopped_unready_releases_count=self.unready_stops,
            stages=[dict(stage=stage, name=name, count=self.stage_counts[stage]) for stage, name in STAGES.items()],
            first=self.first, last=self.last, outstanding_operations_count=len(self.pending),
            outstanding_operations=list(self.pending.values()), combined_active_pending_limit=self.max_active,
            integer_encoding="Exact integers; values wider than53 bits are decimal strings",
            scope="Per-capture CPU operation/context/ticket ownership and provider-promised serial observations only; outstanding entries are not global leaks; backend stop is not readiness, fence, whole-device GPU completion or Present"))
