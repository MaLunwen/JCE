"""Bounded effective Render Pipeline policy observations from the trace owner."""

SERVICE = "render_pipeline.publish"
PREFIX = "rp."
LEGACY_FIELDS = (
    "volfog", "csm", "ssao", "ssr", "taa", "bloom", "mblur",
    "scale_milli", "shadow", "cascades",
)
FULL_FIELDS = LEGACY_FIELDS + (
    "shadow_filter", "backbuffer_msaa", "post_quality", "hdr", "depth_prepass", "ssgi",
)
BOOLEAN_FIELDS = frozenset(LEGACY_FIELDS[:7] + ("hdr", "depth_prepass", "ssgi"))
# Match the published scale clamp and descriptor storage types. Only the enum
# has a closed semantic domain. Allocation and hardware limits are separate.
NUMERIC_RANGES = {
    "scale_milli": (250, 2000),
    "shadow": (0, 65535),
    "cascades": (0, 255),
    "shadow_filter": (0, 255),
    "backbuffer_msaa": (0, 255),
    "post_quality": (0, 3),
}


def _uint64(value):
    if type(value) is not int or not 0 <= value < 1 << 64:
        raise ValueError("Render Pipeline identity must be an exact uint64")
    return value


def _identity_json(value):
    return str(value) if value >= 1 << 53 else value


class RenderPipelineObservation:
    """Consume one ordered policy packet per successfully closed scope."""

    def __init__(self, max_active=8192):
        if type(max_active) is not int or max_active <= 0:
            raise ValueError("Render Pipeline active scope limit must be a positive integer")
        self.max_active = max_active
        self.active = {}
        self.packet_schema = None
        self.publication_count = 0
        self.policy_changes = 0
        self.first = self.last = None

    def begin(self, event, index, start):
        args = event["args"]
        identity = _uint64(args["id"])
        parent = _uint64(args["parent"])
        thread = _uint64(event["tid"])
        owner = start[3]
        if (event["cat"] != "task" or not identity or identity != parent or
                start[0] != SERVICE or start[2] != identity or
                owner[0] != 2 or owner[1] != SERVICE or owner[2] > event["ts"]):
            raise ValueError("Render Pipeline publication requires a submitted same-scope service")
        key = ("task", thread, identity)
        if key in self.active or len(self.active) >= self.max_active:
            raise ValueError("Render Pipeline active scope limit or duplicate owner")
        self.active[key] = {
            "begin_raw_index": index,
            "begin_timestamp_us": event["ts"],
            "last_raw_index": index,
            "last_timestamp_us": event["ts"],
            "values": {},
            "field_raw_indices": {},
        }

    def counter(self, event, index, canonical_active):
        name = event.get("name")
        if not isinstance(name, str) or not name.startswith(PREFIX):
            return
        field = name[len(PREFIX):]
        args = event["args"]
        identity = _uint64(args.get("id"))
        parent = _uint64(args.get("parent"))
        thread = _uint64(event["tid"])
        key = ("task", thread, identity)
        start = canonical_active.get(key)
        packet = self.active.get(key)
        if (event["cat"] != "core" or not identity or identity != parent or
                start is None or start[0] != SERVICE or start[2] != identity or
                packet is None or start[1] != packet["begin_timestamp_us"]):
            raise ValueError("Render Pipeline counter has wrong active owner/thread/category")
        if index <= packet["last_raw_index"] or event["ts"] < packet["last_timestamp_us"]:
            raise ValueError("Render Pipeline counter is outside ordered scope interval")
        values = packet["values"]
        position = len(values)
        if position >= len(FULL_FIELDS) or FULL_FIELDS[position] != field:
            raise ValueError("Render Pipeline counter does not match ordered packet fields")
        value = args.get("value")
        lower, upper = (0, 1) if field in BOOLEAN_FIELDS else NUMERIC_RANGES[field]
        if type(value) is not int or not lower <= value <= upper:
            raise ValueError("Render Pipeline counter outside typed range: " + field)
        values[field] = value
        packet["field_raw_indices"][field] = index
        packet["last_raw_index"] = index
        packet["last_timestamp_us"] = event["ts"]

    def end(self, event, index, scope):
        key = (scope["category"], scope["thread"], scope["id"])
        packet = self.active.pop(key, None)
        if packet is None:
            raise ValueError("Render Pipeline publication closed without active owner")
        if (scope["name"] != SERVICE or scope["task_kind"] != 2 or
                scope["id"] != scope["owner"] or scope["state"] != 0):
            raise ValueError("Render Pipeline publication did not close successfully")
        if index <= packet["last_raw_index"] or event["ts"] < packet["last_timestamp_us"]:
            raise ValueError("Render Pipeline fields are outside closing scope interval")
        values = packet["values"]
        if len(values) == len(LEGACY_FIELDS):
            schema = "legacy"
        elif len(values) == len(FULL_FIELDS):
            schema = "full"
        else:
            raise ValueError("Render Pipeline publication has incomplete fields")
        if self.packet_schema is not None and schema != self.packet_schema:
            raise ValueError("Render Pipeline capture mixes legacy and full publications")
        self.packet_schema = schema
        witness = dict(
            scope_id=_identity_json(scope["id"]), thread=_identity_json(scope["thread"]),
            begin_raw_index=packet["begin_raw_index"], end_raw_index=index,
            begin_timestamp_us=packet["begin_timestamp_us"], end_timestamp_us=event["ts"],
            field_raw_indices=packet["field_raw_indices"], values=values,
        )
        self.publication_count += 1
        if self.last is not None and self.last["values"] != values:
            self.policy_changes += 1
        if self.first is None:
            self.first = witness
        self.last = witness

    def finish(self):
        if self.active:
            raise ValueError("Render Pipeline publications have unfinished scopes")
        if not self.publication_count:
            raise ValueError("Render Pipeline observations requested but no publication was captured")
        return dict(
            schema=1, status="RENDER_PIPELINE_PUBLICATIONS_OBSERVED",
            packet_schema=self.packet_schema, publication_count=self.publication_count,
            policy_changes=self.policy_changes, first=self.first, last=self.last,
            unavailable_fields=list(FULL_FIELDS[len(LEGACY_FIELDS):])
            if self.packet_schema == "legacy" else [],
            validation_limits=dict(active_scopes=self.max_active),
            integer_encoding="uint64 identities wider than 53 bits are decimal strings",
            scope="Effective CPU policy at successful publication. First, last and changes "
            "follow capture completion order. Attachment allocation, GPU work, pixels and "
            "presentation require independent observations.",
        )
