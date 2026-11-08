"""Typed synchronous eager shader receipts fed by the canonical trace owner.

No JSON reader, B/E registry, handle incarnation table or GPU lifetime model
lives here. A receipt identifies one create API acquisition, even when bgfx
returns the same uint16 handle for several acquisitions.
"""
from __future__ import annotations

if __package__:
    from .renderer_submission_observation import _exact
else:
    from renderer_submission_observation import _exact

CREATE = "renderer.shader.create"
LOAD_LINK = "renderer.shader.load_link"
RELEASE = "renderer.shader.creator_release"
LINK = "renderer.shader.link"
SCHEMAS = {
    CREATE: tuple("shader.create." + field for field in (
        "capture", "context_known", "context_hi", "context_lo", "bytes", "payload_known",
        *("sha256." + str(n) for n in range(8)), "handle", "cpu_returned")),
    LOAD_LINK: tuple("shader.owner." + field for field in (
        "capture", "vs_create_hi", "vs_create_lo", "fs_create_hi", "fs_create_lo",
        "vs_reason", "fs_reason", "cpu_returned")),
    RELEASE: tuple("shader.release." + field for field in (
        "capture", "create_hi", "create_lo", "reason", "handle", "cpu_returned")),
    LINK: tuple("shader.link." + field for field in (
        "vs", "fs", "program", "creator_releases", "success", "rejection")),
}
FIELDS = {field: service for service, fields in SCHEMAS.items() for field in fields}
PREFIXES = ("shader.create.", "shader.owner.", "shader.release.", "shader.link.")
UINT32_MAX, UINT64_MAX, INVALID_HANDLE = (1 << 32) - 1, (1 << 64) - 1, (1 << 16) - 1
REASONS = ("available", "absent", "capture_changed", "wrong_returned_handle", "context_unavailable_or_changed")
OUTCOMES = ("succeeded", "failed", "cancelled")


def _uint64(value):
    if type(value) is not int or not 0 <= value <= UINT64_MAX:
        raise ValueError("shader receipt identity must be an exact uint64")
    return value


def _pair(values, name):
    return (values[name + "_hi"] << 32) | values[name + "_lo"]


class ShaderProvenanceObservation:
    """Bounded active packets and unconsumed create receipts; finite summaries."""
    def __init__(self, max_active=8192, max_history=200000):
        for value in (max_active, max_history):
            if type(value) is not int or value <= 0:
                raise ValueError("shader provenance limits must be positive integers")
        self.max_active, self.max_history = max_active, max_history
        self.active, self.pending = {}, {}
        self.capture = None
        self.peak_active = self.peak_pending = 0
        self.successful_creates = self.failed_creates = self.consumed = 0
        self.complete_links = self.complete_releases = self.gaps = self.unbound_links = 0
        self.reasons = [0] * len(REASONS)
        self.stats = {service: dict(service=service, scopes=0, packets=0, unavailable=0,
            outcomes=dict.fromkeys(OUTCOMES, 0), first_complete=None, last_complete=None)
            for service in SCHEMAS}

    def event(self, event):
        name, phase = event.get("name"), event["ph"]
        if name in SCHEMAS and (event["cat"] != "task" or phase not in ("i", "B", "E")):
            raise ValueError("shader provenance requires a submitted task service")
        if isinstance(name, str) and name.startswith(PREFIXES):
            if name not in FIELDS or phase != "C" or event["cat"] != "render":
                raise ValueError("unknown shader provenance field or wrong phase/category")

    def begin(self, event, index, start, canonical_tasks):
        args, owner = event["args"], start[3]
        identity, parent, thread = _uint64(args["id"]), _uint64(args["parent"]), _uint64(event["tid"])
        if (not identity or identity != parent or owner[0] != 2 or
                owner[1] != event["name"] or owner[2] > event["ts"]):
            raise ValueError("shader provenance requires a submitted service owner")
        caller = _uint64(owner[3])
        key = ("task", thread, identity)
        if key in self.active or len(self.active) >= self.max_active:
            raise ValueError("shader provenance active limit or duplicate scope")
        outer = None
        if event["name"] == LINK and canonical_tasks.get(caller, (None, None))[1] == LOAD_LINK:
            outer = self.active.get(("task", thread, caller))
            if outer is None or outer["service"] != LOAD_LINK or outer["child"] is not None:
                raise ValueError("shader link has wrong or repeated enclosing load_link owner/thread")
        self.active[key] = dict(service=event["name"], scope_id=identity, thread=thread,
            submitted_parent=caller, begin_raw_index=index, begin_timestamp_us=event["ts"],
            last_timestamp_us=event["ts"], values={}, field_raw_indices={}, child=None,
            outer=caller if outer is not None else None)
        self.peak_active = max(self.peak_active, len(self.active))

    def counter(self, event, index, canonical_active):
        name = event.get("name")
        if name not in FIELDS:
            return
        args = event["args"]
        identity, parent, thread = _uint64(args.get("id")), _uint64(args.get("parent")), _uint64(event["tid"])
        key = ("task", thread, identity)
        start, packet = canonical_active.get(key), self.active.get(key)
        service = FIELDS[name]
        if (not identity or start is None or start[0] != service or start[2] != identity or
                start[3][0] != 2 or packet is None or packet["service"] != service or
                parent != packet["submitted_parent"] or parent != start[3][3]):
            raise ValueError("shader counter has wrong active owner/thread/submitted caller")
        fields, values = SCHEMAS[service], packet["values"]
        if len(values) >= len(fields) or name != fields[len(values)]:
            raise ValueError("shader fields are duplicate or out of producer order")
        if index <= packet["begin_raw_index"] or event["ts"] < packet["last_timestamp_us"]:
            raise ValueError("shader counter is outside ordered service interval")
        if service == LOAD_LINK and packet["child"] is None:
            raise ValueError("shader owner packet precedes its completed link child")
        upper = UINT32_MAX
        suffix = name.rsplit(".", 1)[1]
        if suffix in ("handle", "vs", "fs", "program"):
            upper = INVALID_HANDLE
        elif suffix in ("context_known", "payload_known", "cpu_returned", "success"):
            upper = 1
        elif suffix in ("reason", "vs_reason", "fs_reason"):
            upper = 4
        elif suffix in ("creator_releases", "rejection"):
            upper = 2
        value = args.get("value")
        if type(value) is not int or not 0 <= value <= upper:
            raise ValueError("shader field is outside exact typed range: " + name)
        values[name], packet["field_raw_indices"][name] = value, index
        packet["last_timestamp_us"] = event["ts"]

    def _capture(self, value):
        if self.capture is None:
            self.capture = value
        elif value != self.capture:
            raise ValueError("shader packets disagree on stable capture generation")

    def _receipt(self, identity, reason, handle, packet, capture):
        self.reasons[reason] += 1
        if reason:
            if identity:
                raise ValueError("shader unavailable reason must carry a zero create receipt")
            self.gaps += 1
            return None
        if not identity:
            raise ValueError("shader available reason requires a create receipt")
        receipt = self.pending.get(identity)
        if (receipt is None or receipt["thread"] != packet["thread"] or
                receipt["submitted_parent"] != packet["submitted_parent"] or
                receipt["capture_generation"] != capture or receipt["handle"] != handle or
                not receipt["payload_known"] or not receipt["context_known"] or
                receipt["end_raw_index"] >= packet["begin_raw_index"]):
            raise ValueError("shader receipt is missing, consumed, unclosed or has wrong capture/thread/caller/handle/context")
        return receipt

    def _create(self, witness, values):
        context = _pair(values, "context")
        known, payload = values["context_known"], values["payload_known"]
        words = [values["sha256." + str(n)] for n in range(8)]
        if bool(context) != bool(known) or (payload and not values["bytes"]) or (not payload and any(words)):
            raise ValueError("shader create availability disagrees with context/extent/digest")
        if witness["state"] != int(values["handle"] == INVALID_HANDLE):
            raise ValueError("shader create return handle disagrees with CPU outcome")
        witness.update(capture_generation=values["capture"], context_known=bool(known),
            context_generation=context if known else None, payload_known=bool(payload),
            buffer_bytes=values["bytes"], sha256="".join(format(word, "08x") for word in words) if payload else None,
            handle=values["handle"])
        if values["handle"] == INVALID_HANDLE:
            self.failed_creates += 1
        else:
            if len(self.pending) >= self.max_history:
                raise ValueError("shader unconsumed receipt history limit exceeded")
            self.pending[witness["scope_id"]] = witness
            self.successful_creates += 1
            self.peak_pending = max(self.peak_pending, len(self.pending))

    def _link(self, witness, values, packet):
        stages = values["vs"] != INVALID_HANDLE and values["fs"] != INVALID_HANDLE
        success = values["program"] != INVALID_HANDLE
        releases = int(values["vs"] != INVALID_HANDLE) + int(values["fs"] != INVALID_HANDLE)
        if (values["creator_releases"] != releases or values["success"] != int(success) or
                success and not stages or values["rejection"] != (0 if success else 2 if stages else 1) or
                witness["state"] != int(not success)):
            raise ValueError("shader link handles/releases/result disagree with CPU outcome")
        witness.update(values)
        if packet["outer"] is None:
            self.unbound_links += 1
        else:
            outer = self.active.get(("task", packet["thread"], packet["outer"]))
            if outer is None or outer["child"] is not None:
                raise ValueError("shader link child closed outside its enclosing owner")
            outer["child"] = witness

    def _owner(self, witness, values, packet):
        child = packet["child"]
        if child is None or not child["details_available"] or child["state"] != witness["state"]:
            raise ValueError("shader load_link requires one complete nested matching link child")
        receipts = {}
        for stage in ("vs", "fs"):
            identity, reason = _pair(values, stage + "_create"), values[stage + "_reason"]
            receipt = self._receipt(identity, reason, child[stage], packet, values["capture"])
            receipts[stage] = dict(create_id=identity, reason=reason, reason_name=REASONS[reason], create=receipt)
        vs, fs = receipts["vs"]["create"], receipts["fs"]["create"]
        if vs is not None and fs is not None:
            if vs["scope_id"] == fs["scope_id"] or vs["context_generation"] != fs["context_generation"]:
                raise ValueError("shader stages require distinct acquisitions in the same device context")
            self.complete_links += 1
        for stage in receipts.values():
            if stage["create"] is not None:
                del self.pending[stage["create_id"]]
                self.consumed += 1
        witness.update(capture_generation=values["capture"], stages=receipts, link=child,
            complete_create_link_creator_release=vs is not None and fs is not None)

    def end(self, event, index, scope):
        key = (event["cat"], event["tid"], event["args"]["id"])
        packet = self.active.pop(key, None)
        if packet is None or scope["category"] != "task" or scope["task_kind"] != 2 or scope["id"] != scope["owner"]:
            raise ValueError("shader service closed without its submitted owner")
        values, service = packet["values"], packet["service"]
        if values and len(values) != len(SCHEMAS[service]):
            raise ValueError("shader packet is missing required fields")
        if event["ts"] < packet["last_timestamp_us"] or index <= packet["begin_raw_index"]:
            raise ValueError("shader packet closes outside its ordered interval")
        witness = dict(service=service, scope_id=scope["id"], submitted_parent=scope["submitted_parent"],
            thread=scope["thread"], state=scope["state"], outcome=OUTCOMES[scope["state"]],
            begin_raw_index=packet["begin_raw_index"], end_raw_index=index,
            begin_timestamp_us=packet["begin_timestamp_us"], end_timestamp_us=event["ts"],
            details_available=bool(values), field_raw_indices=packet["field_raw_indices"])
        row = self.stats[service]
        row["scopes"] += 1
        row["outcomes"][witness["outcome"]] += 1
        if not values:
            row["unavailable"] += 1
            if service == LINK and packet["outer"] is not None:
                self.active[("task", packet["thread"], packet["outer"])]["child"] = witness
            return
        prefix = SCHEMAS[service][0].rsplit(".", 1)[0] + "."
        typed = {field[len(prefix):]: value for field, value in values.items()}
        if service != LINK:
            if typed["cpu_returned"] != 1:
                raise ValueError("shader packet does not observe CPU call return")
            self._capture(typed["capture"])
        if service == CREATE:
            self._create(witness, typed)
        elif service == LINK:
            self._link(witness, typed, packet)
        elif service == LOAD_LINK:
            self._owner(witness, typed, packet)
        else:
            if witness["state"] != 0 or typed["handle"] == INVALID_HANDLE:
                raise ValueError("shader creator release requires a valid returned CPU call")
            receipt = self._receipt(_pair(typed, "create"), typed["reason"], typed["handle"], packet, typed["capture"])
            if receipt is not None:
                del self.pending[receipt["scope_id"]]
                self.consumed += 1
                self.complete_releases += 1
            witness.update(capture_generation=typed["capture"], handle=typed["handle"],
                create_id=_pair(typed, "create"), reason=typed["reason"], reason_name=REASONS[typed["reason"]], create=receipt)
        row["packets"] += 1
        if row["first_complete"] is None:
            row["first_complete"] = witness
        row["last_complete"] = witness

    def finish(self, canonical_tasks, completed_services):
        if self.active:
            raise ValueError("unfinished shader provenance packets")
        for service, row in self.stats.items():
            submitted = 0
            for identity, (kind, name, _timestamp, _parent) in canonical_tasks.items():
                if name == service:
                    submitted += 1
                    if kind != 2 or identity not in completed_services:
                        raise ValueError("shader service has no completed submitted pair")
            if submitted != row["scopes"]:
                raise ValueError("shader service accounting disagrees with canonical owner")
        available = sum(self.stats[name]["packets"] for name in (CREATE, LOAD_LINK, RELEASE))
        unavailable = sum(self.stats[name]["unavailable"] for name in (CREATE, LOAD_LINK, RELEASE))
        covered = bool(self.consumed) and not self.pending and not self.gaps and not unavailable
        return dict(schema=1, status="CPU_SHADER_PROVENANCE_OBSERVED" if available else "UNAVAILABLE",
            creator_receipt_coverage="COMPLETE_CAPTURED_CPU_RECEIPTS" if covered else "UNRESOLVED_OR_UNAVAILABLE",
            capture_generation=self.capture, successful_create_count=self.successful_creates,
            failed_create_count=self.failed_creates, consumed_create_receipt_count=self.consumed,
            unresolved_successful_create_count=len(self.pending), provenance_gap_count=self.gaps,
            complete_create_link_creator_release_count=self.complete_links,
            complete_create_creator_release_count=self.complete_releases,
            unbound_shared_link_count=self.unbound_links,
            reasons=dict(zip(REASONS, self.reasons)), services={name: _exact(row) for name, row in self.stats.items()},
            validation_limits=dict(active_scopes=self.max_active, pending_create_receipts=self.max_history,
                maximum_fields_per_packet=16, retained_witnesses_per_service=2, finite_services=4),
            peak_active_scopes=self.peak_active, peak_pending_create_receipts=self.peak_pending,
            integer_encoding="uint32 words; SHA256 is 8 digest-order big-endian words; integers wider than53bits are decimal strings",
            scope="captured synchronous eager-loader CPU buffer/create/link/creator-release receipts only; no raw shader bytes, global handle lifetime, final program references, leak certification, GPU execution/completion or presentation")
