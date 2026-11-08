"""Bounded offscreen CPU configuration packets from the canonical trace owner.

The shared view service also covers ordinary 3D setup. Empty scopes stay
unclassified; no allocation ledger, JSON reader or GPU-completion claim lives here.
"""
from __future__ import annotations

SERVICE = "renderer.view_configure"
PREFIX = "offscreen."
FIELDS = ("creation_serial", "origin_capture_generation", "current_capture_generation",
          "origin_observed", "origin_same_capture", "origin_capture_gap",
          "view_id", "width", "height", "color_format", "depth_format", "samples",
          "fbo_handle", "color_handle", "depth_handle", "logical_bytes",
          "requested_logical_bytes", "retired_ordinal", "resource_reason",
          "forwarded_rect_width", "forwarded_rect_height", "submission_ordinal")
EXCLUSIVE = frozenset(("forwarded_rect_width", "forwarded_rect_height"))
FLAGS = frozenset(("origin_observed", "origin_same_capture", "origin_capture_gap"))
HANDLES = frozenset(("fbo_handle", "color_handle", "depth_handle"))
UINT16_MAX, UINT32_MAX, UINT64_MAX = (1 << 16) - 1, (1 << 32) - 1, (1 << 64) - 1
INT64_MIN, INT64_MAX = -(1 << 63), (1 << 63) - 1


def _uint64(value):
    if type(value) is not int or not 0 <= value <= UINT64_MAX:
        raise ValueError("offscreen identity must be an exact uint64")
    return value


def _exact(value):
    if type(value) is int and abs(value) > (1 << 53) - 1:
        return str(value)
    if isinstance(value, dict):
        return {key: _exact(item) for key, item in value.items()}
    return value


class OffscreenConfigureObservation:
    """At most max_active * 22 fields, finite counts and first/last packets."""
    def __init__(self, max_active=8192):
        if type(max_active) is not int or max_active <= 0:
            raise ValueError("offscreen active limit must be a positive integer")
        self.max_active = max_active
        self.active = {}
        self.scopes = self.count = 0
        self.unclassified = [0, 0, 0]
        self.generation = None
        self.first = self.last = None

    def event(self, event):
        name, phase = event.get("name"), event["ph"]
        if name == SERVICE and (event["cat"] != "task" or phase not in ("i", "B", "E")):
            raise ValueError("offscreen configuration requires a submitted task service")
        if isinstance(name, str) and name.startswith(PREFIX) and name[len(PREFIX):] in FIELDS:
            if phase != "C" or event["cat"] != "render":
                raise ValueError("offscreen field has wrong phase/category")

    def begin(self, event, index, start):
        args, owner = event["args"], start[3]
        identity, parent = _uint64(args["id"]), _uint64(args["parent"])
        tid = _uint64(event["tid"])
        if (not identity or identity != parent or owner[0] != 2 or
                owner[1] != SERVICE or owner[2] > event["ts"]):
            raise ValueError("offscreen configuration requires a submitted service owner")
        _uint64(owner[3])
        key = ("task", tid, identity)
        if key in self.active or len(self.active) >= self.max_active:
            raise ValueError("offscreen active limit or duplicate scope")
        self.active[key] = dict(begin_raw_index=index, begin_timestamp_us=event["ts"],
            last_timestamp_us=event["ts"], values={}, raw_indices={}, resource=None,
            creation_serial_wire=None)

    def counter(self, event, index, canonical_active):
        name = event.get("name")
        if not isinstance(name, str) or not name.startswith(PREFIX):
            return
        field, args = name[len(PREFIX):], event["args"]
        identity, parent = _uint64(args.get("id")), _uint64(args.get("parent"))
        tid = _uint64(event["tid"])
        # Counter id is the allocation serial; parent is the configure scope.
        key = ("task", tid, parent)
        start, packet = canonical_active.get(key), self.active.get(key)
        if start is None or start[0] != SERVICE:
            if (field in EXCLUSIVE or start is None or
                    not start[0].startswith("renderer.offscreen.") or
                    start[2] != parent or start[3][0] != 2):
                raise ValueError("offscreen field has no active configure or resource owner")
            return  # Shared fields also belong to real create/retire/destroy scopes.
        if (event["cat"] != "render" or not identity or not parent or
                start[2] != parent or packet is None):
            raise ValueError("offscreen counter has wrong active owner/thread/category")
        values = packet["values"]
        if field not in FIELDS or field in values:
            raise ValueError("unknown or duplicate offscreen configure field")
        if field != FIELDS[len(values)]:
            raise ValueError("offscreen configure fields are out of producer order")
        if index <= packet["begin_raw_index"] or event["ts"] < packet["last_timestamp_us"]:
            raise ValueError("offscreen field is outside ordered scope interval")
        value = args.get("value")
        if type(value) is not int or not INT64_MIN <= value <= INT64_MAX:
            raise ValueError("offscreen value must be an exact signed int64 wire integer")
        if field == "creation_serial":
            packet["creation_serial_wire"] = value
            value &= UINT64_MAX  # Core exports this uint64 identity through signed %lld.
            if not value or value != identity:
                raise ValueError("offscreen creation serial disagrees with resource counter id")
            packet["resource"] = value
        else:
            upper = (1 if field in FLAGS else UINT16_MAX if field in HANDLES or field == "view_id"
                     else INT64_MAX if field in ("logical_bytes", "requested_logical_bytes")
                     else UINT32_MAX)
            if not 0 <= value <= upper:
                raise ValueError("offscreen value outside typed range: " + field)
        if identity != packet["resource"]:
            raise ValueError("offscreen fields disagree on resource serial")
        values[field], packet["raw_indices"][field] = value, index
        packet["last_timestamp_us"] = event["ts"]

    def end(self, event, index, scope):
        packet = self.active.pop((event["cat"], event["tid"], event["args"]["id"]), None)
        if packet is None:
            raise ValueError("offscreen configure closed without active owner")
        if (scope["category"] != "task" or scope["task_kind"] != 2 or
                scope["id"] != scope["owner"]):
            raise ValueError("offscreen configure closed under wrong service")
        self.scopes += 1
        values = packet["values"]
        if not values:
            self.unclassified[scope["state"]] += 1
            return
        if len(values) != len(FIELDS) or scope["state"] != 0:
            raise ValueError("offscreen configure requires 22 unique fields in a successful scope")
        if event["ts"] < packet["last_timestamp_us"] or index <= max(packet["raw_indices"].values()):
            raise ValueError("offscreen fields are outside closing scope interval")
        same = bool(values["origin_observed"] and
                    values["origin_capture_generation"] == values["current_capture_generation"])
        if values["origin_same_capture"] != int(same) or values["origin_capture_gap"] != int(not same):
            raise ValueError("offscreen capture-origin flags disagree with generations")
        generation = values["current_capture_generation"]
        if self.generation is not None and self.generation != generation:
            raise ValueError("offscreen current capture generation changed")
        self.generation = generation
        if (not 16 <= values["width"] <= 8192 or not 16 <= values["height"] <= 8192 or
                values["forwarded_rect_width"] != values["width"] or
                values["forwarded_rect_height"] != values["height"]):
            raise ValueError("offscreen forwarded rectangle disagrees with allocated extent")
        if (values["samples"] != 1 or values["retired_ordinal"] != 0 or
                values["resource_reason"] != 0 or any(values[field] == UINT16_MAX for field in HANDLES)):
            raise ValueError("offscreen successful configuration has invalid live-target fields")
        pixels = values["width"] * values["height"]
        if (values["logical_bytes"] not in (pixels * 8, pixels * 12) or
                values["requested_logical_bytes"] != values["logical_bytes"]):
            raise ValueError("offscreen logical attachment estimates disagree with extent")
        # Format enums remain exact raw backend values, without a second enum registry.
        witness = dict(scope_id=scope["id"], submitted_parent=scope["submitted_parent"],
            thread=scope["thread"], begin_raw_index=packet["begin_raw_index"], end_raw_index=index,
            begin_timestamp_us=packet["begin_timestamp_us"], end_timestamp_us=event["ts"],
            field_raw_indices=packet["raw_indices"], values=values,
            creation_serial_wire=packet["creation_serial_wire"])
        self.count += 1
        if self.first is None:
            self.first = witness
        self.last = witness

    def finish(self, canonical_tasks, completed_services):
        if self.active:
            raise ValueError("unfinished offscreen configure packets")
        submitted = 0
        for identity, (kind, name, _timestamp, _parent) in canonical_tasks.items():
            if name == SERVICE:
                submitted += 1
                if kind != 2 or identity not in completed_services:
                    raise ValueError("offscreen shared service has no completed submitted pair")
        if submitted != self.scopes:
            raise ValueError("offscreen shared-service accounting disagrees with closed scopes")
        if not self.count:
            raise ValueError("offscreen configurations requested but no complete successful packet")
        return _exact(dict(schema=1, status="OFFSCREEN_CONFIGURATION_PACKETS_OBSERVED",
            packets_count=self.count, shared_view_scopes_count=self.scopes,
            unclassified_scopes_count=sum(self.unclassified),
            unclassified_succeeded_count=self.unclassified[0],
            unclassified_failed_count=self.unclassified[1],
            unclassified_cancelled_count=self.unclassified[2],
            current_capture_generation=self.generation, first=self.first, last=self.last,
            validation_limits=dict(active_scopes=self.max_active, fields_per_packet=len(FIELDS)),
            integer_encoding="exact integers; values wider than 53 bits are decimal strings; creation serial reconstructed from signed int64 wire",
            coverage="complete observed offscreen packets only; empty shared view scopes unclassified; not all-offscreen coverage or an allocation ledger",
            gpu_completion_known=False, gpu_completion=None,
            scope="CPU configuration-call observations; raw format enums; no backend acceptance, GPU completion, presentation or final displayed texture proof"))
