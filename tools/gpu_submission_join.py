"""Exact sampled D3D11 source-frame joins over the canonical CPU trace reader."""
from __future__ import annotations

if __package__:
    from .renderer_submission_observation import INT64_MAX, UINT32_MAX, _exact, _uint64
else:
    from renderer_submission_observation import INT64_MAX, UINT32_MAX, _exact, _uint64

SERVICE = "renderer.submit_cpu"
# In production snapshot emission order, excluding unrelated counters.
FIELDS = ("gpu.timer_frequency", "gpu.reported_frame_ticks", "gpu.bgfx_renderer_type",
          "renderer.api_frame_number", "renderer.host_submission_ordinal", "renderer.device_epoch",
          "renderer.device_epoch_available", "gpu.source_frame_number", "gpu.raw_begin_ticks",
          "gpu.raw_end_ticks", "gpu.interval_available")
D3D11 = 2  # BGFX_RENDERER_TYPE_DIRECT3D11 in the pinned provider C99 ABI.


class GpuSubmissionJoin:
    """Retain bounded accepted source identities; refuse ambiguous history."""
    def __init__(self, limit=8192):
        self.limit = limit
        self.active, self.submissions = {}, {}
        self.last_submission, self.epochs = None, set()
        self.counts = dict(matched=0, repeated=0, interval_unavailable=0,
                           backend_unverified=0, epoch_unknown=0, source_missing=0)
        self.first = self.last = None

    def begin(self, event, index, start):
        args, owner = event["args"], start[3]
        identity, parent = _uint64(args["id"]), _uint64(args["parent"])
        tid = _uint64(event["tid"])
        if (event["cat"] != "task" or not identity or identity != parent or
                owner[0] != 2 or owner[1] != SERVICE or owner[2] > event["ts"]):
            raise ValueError("GPU submission join requires a submitted snapshot service")
        key = ("task", tid, identity)
        if key in self.active or len(self.active) >= self.limit:
            raise ValueError("GPU submission join active scope limit or duplicate")
        self.active[key] = dict(index=index, timestamp=event["ts"], values={},
                                field_indices={}, advances=[])

    def submission(self, witness, submitted_parent):
        outer = self.active.get(("task", witness["thread"], submitted_parent))
        if outer is not None:
            if (outer["advances"] or witness["begin_raw_index"] <= outer["index"] or
                    witness["begin_timestamp_us"] < outer["timestamp"] or outer["values"]):
                raise ValueError("ambiguous or misordered snapshot advance")
            outer["advances"].append(witness)
        values = witness["values"]
        epoch = values["context_generation"]
        before, after = values["backend_serial_before"], values["backend_serial_after"]
        ordinal = values["ordinal_before"]
        if after < before or ordinal == UINT32_MAX:
            raise ValueError("wrapped backend serial or host ordinal")
        if epoch == -1:
            self.last_submission = None
            return
        # A fresh context can have internal bgfx startup advances before the
        # first host submission. Subsequent calls on that context advance once.
        if (ordinal == 0 and before != 0) or (ordinal != 0 and after != before + 1):
            raise ValueError("backend serial is discontinuous within a submission")
        previous = self.last_submission
        if previous and previous["values"]["context_generation"] == epoch:
            prior = previous["values"]
            if (before != prior["backend_serial_after"] or ordinal != prior["ordinal_before"] + 1 or
                    witness["thread"] != previous["thread"]):
                raise ValueError("ambiguous or discontinuous renderer submission history")
        else:
            if epoch in self.epochs:
                raise ValueError("renderer epoch reappeared after a context boundary")
            self.epochs.add(epoch)
        key = (epoch, after)
        if key in self.submissions or len(self.submissions) >= self.limit:
            raise ValueError("GPU submission join source identity limit or duplicate")
        self.submissions[key] = (witness, None)
        self.last_submission = witness

    def counter(self, event, index, canonical_active):
        name = event.get("name")
        if name not in FIELDS:
            return
        args = event["args"]
        identity, parent = _uint64(args.get("id")), _uint64(args.get("parent"))
        key = ("task", _uint64(event["tid"]), identity)
        start, packet = canonical_active.get(key), self.active.get(key)
        if (event["cat"] != "render" or not identity or identity != parent or
                start is None or start[0] != SERVICE or start[2] != identity or packet is None):
            raise ValueError("GPU snapshot counter has wrong active owner/thread/category")
        advances, values = packet["advances"], packet["values"]
        if (len(advances) != 1 or index <= advances[0]["end_raw_index"] or
                event["ts"] < advances[0]["end_timestamp_us"]):
            raise ValueError("GPU snapshot requires an earlier nested CPU advance")
        if name in values or name != FIELDS[len(values)]:
            raise ValueError("GPU snapshot field is repeated or out of producer order")
        value = args.get("value")
        if type(value) is not int or not -INT64_MAX - 1 <= value <= INT64_MAX:
            raise ValueError("GPU snapshot value must be exact int64: " + name)
        values[name], packet["field_indices"][name] = value, index

    def end(self, event, index, scope):
        identity, thread = scope["id"], scope["thread"]
        packet = self.active.pop((event["cat"], thread, identity), None)
        if packet is None:
            raise ValueError("GPU snapshot scope closed without an active owner")
        values = packet["values"]
        if not values:
            return  # Snapshot cadence deliberately leaves most scopes empty.
        if (len(values) != len(FIELDS) or scope["category"] != "task" or
                scope["task_kind"] != 2 or scope["owner"] != identity or scope["state"] != 0):
            raise ValueError("GPU snapshot requires a complete successful owner")
        freq, ticks, backend, api_frame, host_ordinal, epoch, epoch_available, source, begin, end, available = (
            values[field] for field in FIELDS)
        if (not -1 <= backend <= UINT32_MAX or not 0 <= source <= UINT32_MAX or
                epoch_available != int(epoch > 0) or epoch == 0 or epoch < -1):
            raise ValueError("GPU snapshot identity fields outside typed range")
        current = packet["advances"][0]["values"]
        if (epoch != current["context_generation"] or api_frame != current["backend_serial_after"] or
                host_ordinal != current["ordinal_before"] + 1):
            raise ValueError("GPU snapshot disagrees with its nested CPU advance identity")
        interval = freq > 0 and begin >= 0 and end >= begin and (begin != 0 or end != 0)
        if available != int(interval) or ticks != (end - begin if interval else -1):
            raise ValueError("GPU snapshot interval/frequency/tick validity disagrees")
        if not interval:
            self.counts["interval_unavailable"] += 1
            return
        if backend != D3D11:
            self.counts["backend_unverified"] += 1
            return
        if epoch == -1:
            self.counts["epoch_unknown"] += 1
            return
        if source > api_frame:
            raise ValueError("GPU snapshot source is newer than its accepted CPU advance")
        key = (epoch, source)
        record = self.submissions.get(key)
        if record is None:
            self.counts["source_missing"] += 1
            return
        witness, signature = record
        first_index = packet["field_indices"][FIELDS[0]]
        if (witness["thread"] != thread or witness["end_raw_index"] >= first_index or
                witness["end_timestamp_us"] > packet["advances"][0]["end_timestamp_us"]):
            raise ValueError("GPU snapshot source advance is outside the ordered timeline")
        observation = (freq, begin, end)
        if signature is not None:
            if observation != signature:
                raise ValueError("GPU snapshot source frame has conflicting reported intervals")
            self.counts["repeated"] += 1
            return
        self.submissions[key] = (witness, observation)
        self.counts["matched"] += 1
        joined = dict(source_advance=witness, snapshot=dict(
            scope_id=identity, thread=thread,
            current_advance_scope_id=packet["advances"][0]["scope_id"],
            begin_raw_index=packet["index"],
            end_raw_index=index, begin_timestamp_us=packet["timestamp"],
            end_timestamp_us=event["ts"], field_raw_indices=packet["field_indices"],
            values=values))
        if self.first is None:
            self.first = joined
        self.last = joined

    def finish(self):
        if self.active or not self.counts["matched"]:
            raise ValueError("no complete exact D3D11 GPU source-frame join")
        return dict(schema=1, status="SAMPLED_D3D11_GPU_SOURCE_FRAME_MATCHED",
                    counts=self.counts, source_submissions=len(self.submissions),
                    first=_exact(self.first), last=_exact(self.last),
                    gpu_completion="UNAVAILABLE", os_present="UNAVAILABLE", dwm="UNAVAILABLE",
                    scope="exact source-frame identity for sampled provider-reported intervals only; "
                          "GPU disjoint/device-lost validity, hardware completion and display unverified")
