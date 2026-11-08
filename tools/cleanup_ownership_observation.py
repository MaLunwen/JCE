"""Bounded cleanup outcome packets consumed by the canonical lifecycle reader.

This is an opt-in captured CPU ownership boundary, not an Engine identity ledger
or proof that the post-marker capture/SDL/allocation tail or GPU has completed.
"""
from __future__ import annotations

SERVICE = "engine.cleanup_attempt"
FIELDS = ("cleanup.reason", "cleanup.completed")
AUDIO_SERVICE = "engine.audio_master_retire"
AUDIO_FIELDS = ("audio_master.had_device", "audio_master.device_live_after", "audio_master.retired")
TERMINAL_SESSION = "engine.terminal_session"
REASONS = ("completed", "active_borrow_or_construction", "reentry",
           "sampler", "frame_kpi", "async_executor", "audio_master")


def _identity(value):
    if type(value) is not int or not 0 <= value < 1 << 64:
        raise ValueError("cleanup ownership identity must be an exact uint64")
    return value


def _exact(value):
    if type(value) is int and value > (1 << 53) - 1:
        return str(value)
    if isinstance(value, dict):
        return {key: _exact(item) for key, item in value.items()}
    return value


class CleanupOwnershipObservation:
    """Per-attempt fields, fixed reason counts and first/last closed witnesses."""
    def __init__(self, max_active=8192, *, terminal_session_binding=False, max_history=200000, terminal_sessions=None):
        if type(terminal_session_binding) is not bool:
            raise ValueError("terminal_session_binding must be an explicit bool")
        if type(max_history) is not int or max_history <= 0:
            raise ValueError("terminal session history limit must be a positive integer")
        self.max_active = max_active
        self.active = {}
        self.count = 0
        self.reasons = [0] * len(REASONS)
        self.first = self.last = None
        self.audio_count = self.audio_typed = self.audio_uninit = self.audio_no_device = 0
        self.audio_failed = self.audio_retained = 0
        self.audio_first = self.audio_last = None
        self.terminal = (dict(seen=terminal_sessions if terminal_sessions is not None else set(),
            count=0, first=None, last=None, final=None,
            max_history=terminal_sessions.max_records if terminal_sessions is not None else max_history)
            if terminal_session_binding else None)
        self.disk_sessions = terminal_sessions is not None

    def event(self, event):
        name, phase = event.get("name"), event["ph"]
        if self.terminal is not None and name == TERMINAL_SESSION:
            if phase != "C" or event["cat"] != "core":
                raise ValueError("invalid terminal session trace counter phase/category")
        if name in (SERVICE, AUDIO_SERVICE) and (event["cat"] != "task" or phase not in ("i", "B", "E")):
            raise ValueError("cleanup ownership attempt must be a submitted task service")
        if isinstance(name, str) and name.startswith("cleanup."):
            if name not in FIELDS or phase != "C" or event["cat"] != "core":
                raise ValueError("unknown cleanup ownership field or wrong phase/category")
        if isinstance(name, str) and name.startswith("audio_master."):
            if name not in AUDIO_FIELDS or phase != "C" or event["cat"] != "core":
                raise ValueError("unknown audio master observation field or wrong phase/category")

    def begin(self, event, index, start):
        args = event["args"]
        identity, parent = _identity(args["id"]), _identity(args["parent"])
        tid = _identity(event["tid"])
        owner = start[3]
        service = event["name"]
        if (not identity or identity != parent or owner[0] != 2 or
                service not in (SERVICE, AUDIO_SERVICE) or owner[1] != service or owner[2] > event["ts"]):
            raise ValueError("cleanup ownership requires a submitted service scope")
        _identity(owner[3])
        key = ("task", tid, identity)
        if key in self.active or len(self.active) >= self.max_active:
            raise ValueError("cleanup ownership active limit or duplicate scope")
        self.active[key] = dict(service=service, begin_raw_index=index, begin_timestamp_us=event["ts"],
                               last_timestamp_us=event["ts"], values={}, raw_indices={})

    def counter(self, event, index, canonical_active):
        name = event.get("name")
        if self.terminal is not None and name == TERMINAL_SESSION:
            self._terminal_counter(event, index, canonical_active)
            return
        if name not in FIELDS and name not in AUDIO_FIELDS:
            return
        fields, service = (FIELDS, SERVICE) if name in FIELDS else (AUDIO_FIELDS, AUDIO_SERVICE)
        args = event["args"]
        identity, parent = _identity(args.get("id")), _identity(args.get("parent"))
        key = ("task", _identity(event["tid"]), identity)
        start, sample = canonical_active.get(key), self.active.get(key)
        if (not identity or identity != parent or start is None or
                start[0] != service or start[2] != identity or sample is None or sample["service"] != service):
            raise ValueError("cleanup ownership counter has wrong active owner/thread")
        if index <= sample["begin_raw_index"] or event["ts"] < sample["last_timestamp_us"]:
            raise ValueError("cleanup ownership counter is outside ordered scope interval")
        if name in sample["values"]:
            raise ValueError("duplicate cleanup ownership field")
        if name != fields[len(sample["values"])]:
            raise ValueError("cleanup ownership fields are out of producer order")
        value = args.get("value")
        upper = len(REASONS) - 1 if name == FIELDS[0] else 1
        if type(value) is not int or not 0 <= value <= upper:
            raise ValueError("cleanup ownership field must be an exact bounded integer")
        sample["values"][name], sample["raw_indices"][name] = value, index
        sample["last_timestamp_us"] = event["ts"]

    def _terminal_counter(self, event, index, canonical_active):
        args = event["args"]
        identity, parent, thread = _identity(args.get("id")), _identity(args.get("parent")), _identity(event["tid"])
        key = ("task", thread, identity)
        start, sample = canonical_active.get(key), self.active.get(key)
        session = args.get("value")
        if (not identity or parent != identity or type(session) is not int or not 0 < session < 1 << 63 or
                start is None or start[0] != SERVICE or start[2] != identity or start[3][0] != 2 or
                sample is None or sample["service"] != SERVICE or
                index <= sample["begin_raw_index"] or event["ts"] < sample["last_timestamp_us"]):
            raise ValueError("terminal session counter has wrong active cleanup owner/thread/interval")
        if sample.get("terminal_session") is not None or session in self.terminal["seen"]:
            raise ValueError("terminal session counter does not uniquely belong to one cleanup")
        if not self.disk_sessions and len(self.terminal["seen"]) >= self.terminal["max_history"]:
            raise ValueError("terminal session history limit exceeded")
        self.terminal["seen"].add(session)
        sample["terminal_session"] = dict(scope_id=identity, session=session,
            counter=dict(raw_index=index, thread=thread, timestamp_us=event["ts"]))
        sample["last_timestamp_us"] = event["ts"]

    def end(self, event, index, scope):
        sample = self.active.pop((event["cat"], event["tid"], event["args"]["id"]), None)
        if sample is None:
            raise ValueError("cleanup ownership scope has no active observation")
        if event["ts"] < sample["last_timestamp_us"]:
            raise ValueError("cleanup ownership fields are outside closing scope interval")
        if sample["service"] == AUDIO_SERVICE:
            self._audio_end(sample, event, index, scope)
            return
        if len(sample["values"]) != len(FIELDS):
            raise ValueError("cleanup ownership attempt closed without both unique fields")
        reason, completed = (sample["values"][name] for name in FIELDS)
        if completed != int(reason == 0) or scope["state"] != (0 if completed else 1):
            raise ValueError("cleanup ownership reason/completed/task outcome disagree")
        witness = dict(scope_id=scope["id"], parent=scope["owner"], thread=scope["thread"],
            submitted_parent=scope["submitted_parent"], begin_raw_index=sample["begin_raw_index"],
            end_raw_index=index, begin_timestamp_us=sample["begin_timestamp_us"],
            end_timestamp_us=event["ts"], field_raw_indices=sample["raw_indices"],
            reason=reason, reason_name=REASONS[reason], completed=completed, state=scope["state"])
        self.count += 1
        self.reasons[reason] += 1
        if self.first is None:
            self.first = witness
        self.last = witness
        if self.terminal is not None:
            marker = sample.get("terminal_session")
            if marker is not None:
                if not completed or not marker["counter"]["raw_index"] < index:
                    raise ValueError("terminal session marker requires successful closed cleanup")
                self.terminal["count"] += 1
                if self.terminal["first"] is None:
                    self.terminal["first"] = marker
                self.terminal["last"] = marker
            self.terminal["final"] = marker

    def _audio_end(self, sample, event, index, scope):
        values = sample["values"]
        if values and len(values) != len(AUDIO_FIELDS):
            raise ValueError("audio master scope closed with a partial observation triple")
        had = after = retired = uninit = None
        if values:
            had, after, retired = (values[name] for name in AUDIO_FIELDS)
            if scope["state"] != (0 if retired else 1) or (after and (retired or not had)):
                raise ValueError("audio master device/result/task outcome disagree")
            uninit = bool(had and not after and retired)
            self.audio_typed += 1
            self.audio_uninit += int(uninit)
            self.audio_no_device += int(not had and retired)
            self.audio_failed += int(not retired)
            self.audio_retained += int(bool(after and not retired))
        witness = dict(scope_id=scope["id"], submitted_parent=scope["submitted_parent"],
            thread=scope["thread"], begin_raw_index=sample["begin_raw_index"], end_raw_index=index,
            begin_timestamp_us=sample["begin_timestamp_us"], end_timestamp_us=event["ts"],
            field_raw_indices=sample["raw_indices"], state=scope["state"],
            status="TYPED_OBSERVATION" if values else "UNAVAILABLE_LEGACY_SCOPE_WITHOUT_FIELDS",
            had_device=had, device_live_after=after, retired=retired,
            device_uninit_return_observed=uninit)
        self.audio_count += 1
        if self.audio_first is None:
            self.audio_first = witness
        self.audio_last = witness

    def _audio_summary(self):
        status = ("UNAVAILABLE" if not self.audio_typed else
                  "AVAILABLE" if self.audio_typed == self.audio_count else "PARTIAL_TYPED_COVERAGE")
        return dict(schema=1, status=status, attempts_count=self.audio_count,
            typed_attempts_count=self.audio_typed, unavailable_attempts_count=self.audio_count - self.audio_typed,
            device_uninit_return_count=self.audio_uninit if self.audio_typed else None,
            no_device_success_count=self.audio_no_device if self.audio_typed else None,
            failed_attempts_count=self.audio_failed if self.audio_typed else None,
            retained_live_refusal_count=self.audio_retained if self.audio_typed else None,
            first=_exact(self.audio_first), last=_exact(self.audio_last),
            scope="serialized actual master device flag before/after CPU retirement; no-device success "
                  "does not prove native uninit; absent legacy fields remain unavailable; "
                  "no acoustic, portable join latency, process exit or GPU proof")

    def finish(self, metadata, canonical_tasks, completed_services):
        # A resident ring snapshot can end at an earlier success and omit a later
        # refusal. Only the existing full continuous-stream footer admits this gate.
        if metadata.get("mode") != "continuous" or metadata.get("capture_complete") is not True:
            raise ValueError("cleanup ownership requires a complete continuous capture")
        submitted = audio_submitted = 0
        for identity, (kind, name, _timestamp, parent) in canonical_tasks.items():
            if name == SERVICE:
                _identity(identity)
                _identity(parent)
                submitted += 1
                if kind != 2 or identity not in completed_services:
                    raise ValueError("cleanup ownership submitted attempt has no completed service pair")
            elif name == AUDIO_SERVICE:
                _identity(identity)
                _identity(parent)
                audio_submitted += 1
                if kind != 2 or identity not in completed_services:
                    raise ValueError("audio master submitted attempt has no completed service pair")
        if audio_submitted != self.audio_count:
            raise ValueError("audio master observations missing canonical service closure")
        if self.active or not self.count or submitted != self.count:
            raise ValueError("cleanup ownership attempts missing or incomplete")
        if self.last["completed"] != 1:
            raise ValueError("cleanup ownership final captured attempt did not retire successfully")
        result = dict(schema=1, status="FINAL_CAPTURED_CLEANUP_SUCCESS_OBSERVED", attempts_count=self.count,
            successful_count=self.reasons[0], deferred_count=sum(self.reasons[1:]),
            reason_counts={name: count for name, count in zip(REASONS, self.reasons)},
            first=_exact(self.first), last=_exact(self.last), audio_master=self._audio_summary(),
            witness_order="exported scope-close order; nested/reentrant attempts are paired independently",
            integer_encoding="exact integers; identities wider than 53 bits are decimal strings",
            scope="complete captured CPU cleanup outcome packets only; no stable Engine/session identity, "
                      "no all-owner retirement, post-marker capture/SDL/allocation tail, process exit, leak or GPU completion proof")
        if self.terminal is not None:
            result["terminal_sessions"] = dict(schema=1,
                status="FINAL_CAPTURED_TERMINAL_SESSION_OBSERVED" if self.terminal["final"] else "UNAVAILABLE",
                observed_count=self.terminal["count"], first=_exact(self.terminal["first"]),
                last=_exact(self.terminal["last"]), final=_exact(self.terminal["final"]),
                validation_limits=dict(historical_sessions=self.terminal["max_history"], retained_witnesses=3),
                scope="exact scoped CPU terminal-session markers through the canonical event/cleanup owner; no host token or GPU completion claim")
        if self.terminal is not None and self.disk_sessions:
            result["terminal_sessions"]["validation_limits"]["history_budget_scope"] = "shared disk tasks plus terminal sessions"
        return result
