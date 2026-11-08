"""Bounded filesystem read details fed by the canonical lifecycle analyzer.

The producer already emits these scoped counters. This module owns no JSON
reader, task history, file access, filesystem policy or path decoding.
"""
from __future__ import annotations

if __package__:
    from .renderer_submission_observation import _exact
else:
    from renderer_submission_observation import _exact

SERVICES = frozenset(("fs.host.read_all", "fs.host.read_capped"))
PREFIX = "fs.read."
PAIRS = ("path_hash", "total", "requested", "cap", "actual")
FIELDS = tuple(part + suffix for part in PAIRS for suffix in ("_hi", "_lo")) + (
    "dispatch", "known", "failure_stage", "close", "short")
FIELD_SET = frozenset(FIELDS)
FIELD_MAX = {"dispatch": 3, "known": 15, "failure_stage": 7, "close": 2, "short": 2}
DISPATCH = ("none", "host", "vfs", "vfs_then_host")
FAILURE = ("none_observed", "invalid_request", "isolated_vfs_read", "open",
           "size", "limit", "allocation", "short_read")
CLOSE = ("unobserved_or_not_applicable", "api_ok", "api_failed")
SHORT = ("unobserved_or_not_applicable", "full_observed", "short_observed")
OUTCOMES = ("succeeded", "failed", "cancelled")
UINT32_MAX, UINT64_MAX = (1 << 32) - 1, (1 << 64) - 1
MAX_RETAINED = 256


def _uint64(value):
    if type(value) is not int or not 0 <= value <= UINT64_MAX:
        raise ValueError("filesystem read identity must be an exact uint64")
    return value


class FilesystemReadObservation:
    """At most max_active*15 fields; bounded first/last diagnostic witnesses."""
    def __init__(self, max_active=8192, max_history=200000):
        for value in (max_active, max_history):
            if type(value) is not int or value <= 0:
                raise ValueError("filesystem read limits must be positive integers")
        self.max_active, self.max_retained = max_active, min(max_history, MAX_RETAINED)
        self.active, self.aggregates, self.rows = {}, {}, []
        self.scopes = self.packets = self.unknown = self.eligible = self.peak_active = 0
        self.outcomes, self.available_outcomes = [0, 0, 0], [0, 0, 0]
        self.close_failures = self.short_reads = 0
        self.first = self.last = None

    def event(self, event):
        name, phase = event.get("name"), event["ph"]
        if name in SERVICES and (event["cat"] != "task" or phase not in ("i", "B", "E")):
            raise ValueError("filesystem read requires a submitted task service")
        if isinstance(name, str) and name.startswith(PREFIX):
            if name[len(PREFIX):] not in FIELD_SET:
                raise ValueError("unknown filesystem read field")
            if phase != "C" or event["cat"] != "core":
                raise ValueError("filesystem read field has wrong phase/category")

    def begin(self, event, index, start):
        args, owner = event["args"], start[3]
        identity, parent = _uint64(args["id"]), _uint64(args["parent"])
        thread = _uint64(event["tid"])
        if (not identity or identity != parent or owner[0] != 2 or
                owner[1] != event["name"] or owner[2] > event["ts"]):
            raise ValueError("filesystem read requires a submitted service owner")
        caller = _uint64(owner[3])
        key = ("task", thread, identity)
        if key in self.active or len(self.active) >= self.max_active:
            raise ValueError("filesystem read active limit or duplicate scope")
        self.active[key] = dict(begin_raw_index=index, begin_timestamp_us=event["ts"],
            last_timestamp_us=event["ts"], submitted_parent=caller, values={}, raw_indices={})
        self.peak_active = max(self.peak_active, len(self.active))

    def counter(self, event, index, canonical_active):
        name = event.get("name")
        if not isinstance(name, str) or not name.startswith(PREFIX):
            return
        field, args = name[len(PREFIX):], event["args"]
        identity, parent, thread = (_uint64(args.get("id")),
            _uint64(args.get("parent")), _uint64(event["tid"]))
        key = ("task", thread, identity)
        start, packet = canonical_active.get(key), self.active.get(key)
        # B/E parent is the service self-id; counter parent is its submitted
        # caller. They are different wire relationships, including caller zero.
        if (event["cat"] != "core" or not identity or start is None or
                start[0] not in SERVICES or start[2] != identity or
                start[3][0] != 2 or packet is None or
                parent != start[3][3] or parent != packet["submitted_parent"]):
            raise ValueError("filesystem read counter has wrong active owner/thread/parent")
        if field not in FIELD_SET or field in packet["values"]:
            raise ValueError("unknown or duplicate filesystem read field")
        if index <= packet["begin_raw_index"] or event["ts"] < packet["last_timestamp_us"]:
            raise ValueError("filesystem read counter is outside ordered scope interval")
        upper = FIELD_MAX.get(field, UINT32_MAX)
        value = args.get("value")
        if type(value) is not int or not 0 <= value <= upper:
            raise ValueError("filesystem read value outside exact typed range: " + field)
        packet["values"][field], packet["raw_indices"][field] = value, index
        packet["last_timestamp_us"] = event["ts"]

    def end(self, event, index, scope):
        key = (event["cat"], event["tid"], event["args"]["id"])
        packet = self.active.pop(key, None)
        if packet is None or (scope["category"] != "task" or
                scope["task_kind"] != 2 or scope["id"] != scope["owner"]):
            raise ValueError("filesystem read closed without its submitted service owner")
        fields = packet["values"]
        if fields and len(fields) != len(FIELDS):
            raise ValueError("filesystem read packet needs all15 unique fields")
        if event["ts"] < packet["last_timestamp_us"] or index <= packet["begin_raw_index"]:
            raise ValueError("filesystem read fields are outside closing scope interval")
        state = scope["state"]
        witness = dict(service=scope["name"], scope_id=scope["id"],
            submitted_parent=scope["submitted_parent"], thread=scope["thread"],
            state=state, outcome=OUTCOMES[state], begin_raw_index=packet["begin_raw_index"],
            end_raw_index=index, begin_timestamp_us=packet["begin_timestamp_us"],
            end_timestamp_us=event["ts"], measured_ns=event["args"]["ns"],
            details_available=bool(fields), field_raw_indices=packet["raw_indices"],
            path_hash=None, path_hash_hex=None, dispatch=None, dispatch_name=None,
            failure_stage=None, failure_stage_name=None, known_mask=None,
            bytes=dict(total=None, requested=None, cap=None, actual=None),
            close=None, close_name=None, short=None, short_name=None,
            optionality=None, errno=None)
        self.scopes += 1
        self.outcomes[state] += 1
        if fields:
            rebuilt = {name: (fields[name + "_hi"] << 32) | fields[name + "_lo"] for name in PAIRS}
            byte_values = {name: rebuilt[name] if fields["known"] & (1 << n) else None
                           for n, name in enumerate(PAIRS[1:])}
            self.packets += 1
            self.available_outcomes[state] += 1
            self.close_failures += fields["close"] == 2
            self.short_reads += fields["short"] == 2
            witness.update(path_hash=rebuilt["path_hash"], path_hash_hex=format(rebuilt["path_hash"], "016x"),
                dispatch=fields["dispatch"], dispatch_name=DISPATCH[fields["dispatch"]],
                failure_stage=fields["failure_stage"], failure_stage_name=FAILURE[fields["failure_stage"]],
                known_mask=fields["known"], bytes=byte_values, wire_fields=dict(fields),
                close=fields["close"], close_name=CLOSE[fields["close"]],
                short=fields["short"], short_name=SHORT[fields["short"]])
            if self.first is None:
                self.first = witness
            self.last = witness
        else:
            self.unknown += 1
        aggregate_key = (scope["name"], state, witness["dispatch"], witness["failure_stage"])
        row = self.aggregates.setdefault(aggregate_key, dict(service=scope["name"], state=state,
            outcome=OUTCOMES[state], dispatch=witness["dispatch"], failure_stage=witness["failure_stage"], count=0))
        row["count"] += 1
        # Preserve the API outcome. Capped short reads and failed close may
        # return a buffer successfully; these details are still useful anomalies.
        if state != 0 or witness["close"] == 2 or witness["short"] == 2:
            self.eligible += 1
            if len(self.rows) < self.max_retained:
                self.rows.append(witness)
            else:
                self.rows[-1] = witness

    def finish(self, canonical_tasks, completed_services):
        if self.active:
            raise ValueError("unfinished filesystem read packets")
        submitted = 0
        for identity, (kind, name, _timestamp, _parent) in canonical_tasks.items():
            if name in SERVICES:
                submitted += 1
                if kind != 2 or identity not in completed_services:
                    raise ValueError("filesystem read has no completed submitted pair")
        if submitted != self.scopes:
            raise ValueError("filesystem read service accounting disagrees with closed scopes")
        status = ("NO_ACTIVITY" if not self.scopes else "DETAILS_UNAVAILABLE" if not self.packets
                  else "FILESYSTEM_READ_DETAILS_OBSERVED")
        return dict(schema=1, status=status, service_scopes_count=self.scopes,
            complete_packets_count=self.packets, no_detail_scopes_count=self.unknown,
            scope_outcomes=dict(zip(OUTCOMES, self.outcomes)),
            details_available_outcomes=dict(zip(OUTCOMES, self.available_outcomes)),
            observed_close_failed_count=self.close_failures, observed_short_count=self.short_reads,
            diagnostic_eligible_count=self.eligible, diagnostic_retained_count=len(self.rows),
            diagnostic_omitted_count=self.eligible - len(self.rows),
            diagnostic_rows=[_exact(row) for row in self.rows],
            first_complete=_exact(self.first), last_complete=_exact(self.last),
            aggregates=list(self.aggregates.values()),
            validation_limits=dict(active_scopes=self.max_active, fields_per_packet=len(FIELDS),
                retained_diagnostics=self.max_retained, finite_aggregate_keys=198),
            peak_active_scopes=self.peak_active,
            retention="first N-1 and last eligible scopes in exported close order; all packets validated",
            integer_encoding="exact integers; values wider than53bits are decimal strings; path_hash_hex preserves all64bits",
            request_identity_scope="hash of exact request bytes, not canonical path or validity; zero may mean NULL or a hash value; empty requests also have a hash",
            scope="CPU read observations only; optionality/errno unavailable; OPEN is not ENOENT and ISOLATED_VFS is not absence; no raw paths, IO durability, GPU completion or all-filesystem coverage")
