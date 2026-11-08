"""Bounded screenshot-request counters fed by the canonical lifecycle reader.

This owns neither JSON parsing nor task lifetimes. Protocol completion is the
CPU encoder/file outcome or shutdown cancellation, never GPU or durable IO.
"""
from __future__ import annotations

if __package__:
    from .renderer_submission_observation import _uint64, _exact
else:
    from renderer_submission_observation import _uint64, _exact

PREFIX = "renderer.screenshot."
ADMIT, CPU_PIXELS, TERMINAL = PREFIX + "admit", PREFIX + "cpu_pixels", PREFIX + "terminal"
MODES = ("backbuffer", "readback")
# Private wire values match JceTraceTaskState; the real-writer Native control
# emits the actual C enum constants rather than a second assumed C encoding.
OUTCOMES = ("succeeded", "failed", "cancelled")


class ScreenshotRequestObservation:
    """Monotonic admissions plus bounded unclosed records; no closed-ID history."""
    def __init__(self, max_active=8192):
        if type(max_active) is not int or max_active <= 0:
            raise ValueError("screenshot request active limit must be a positive integer")
        self.max_active = max_active
        self.active = {}
        self.last_admit = self.admitted = self.closed = self.peak_active = 0
        self.modes = [0, 0]
        self.outcomes = [0, 0, 0]
        self.first = self.last = None
        self.cpu_pixels_count = 0
        self.first_cpu_pixels = self.last_cpu_pixels = None

    def event(self, event, index):
        name = event.get("name")
        if not isinstance(name, str) or not name.startswith(PREFIX):
            return
        if name not in (ADMIT, CPU_PIXELS, TERMINAL):
            raise ValueError("unknown screenshot request field")
        if event["ph"] != "C" or event["cat"] != "render":
            raise ValueError("screenshot request requires a render counter")
        args = event["args"]
        identity, parent = _uint64(args.get("id")), _uint64(args.get("parent"))
        thread, value = _uint64(event["tid"]), _uint64(args.get("value"))
        if not identity or parent:
            raise ValueError("screenshot request needs nonzero uint64 identity and parent zero")
        if name == ADMIT:
            if value >= len(MODES):
                raise ValueError("unknown screenshot request mode")
            if identity <= self.last_admit:
                raise ValueError("screenshot request admission identity is not strictly increasing")
            if len(self.active) >= self.max_active:
                raise ValueError("screenshot request active limit exceeded")
            self.active[identity] = dict(mode=value, admit_raw_index=index,
                admit_timestamp_us=event["ts"], admit_thread=thread)
            self.last_admit = identity
            self.admitted += 1
            self.peak_active = max(self.peak_active, len(self.active))
            self.modes[value] += 1
            return
        start = self.active.get(identity)
        if name == CPU_PIXELS:
            if start is None or start["mode"] != 0:
                raise ValueError("screenshot CPU pixels require an unmatched backend screenshot admission")
            if "cpu_pixels" in start:
                raise ValueError("duplicate screenshot CPU pixels")
            if value > 0xFFFF:
                raise ValueError("screenshot CPU pixels framebuffer index must be uint16")
            if index <= start["admit_raw_index"] or event["ts"] < start["admit_timestamp_us"]:
                raise ValueError("screenshot CPU pixels precede their admission")
            witness = dict(identity=identity, raw_index=index, timestamp_us=event["ts"],
                           thread=thread, framebuffer_index=value)
            start["cpu_pixels"] = witness
            self.cpu_pixels_count += 1
            if self.first_cpu_pixels is None:
                self.first_cpu_pixels = witness
            self.last_cpu_pixels = witness
            return
        if value >= len(OUTCOMES):
            raise ValueError("unknown screenshot request terminal outcome")
        if start is None:
            raise ValueError("screenshot request terminal has no unmatched admission")
        if index <= start["admit_raw_index"] or event["ts"] < start["admit_timestamp_us"]:
            raise ValueError("screenshot request terminal precedes its admission")
        pixels = start.get("cpu_pixels")
        if pixels is not None and (index <= pixels["raw_index"] or event["ts"] < pixels["timestamp_us"]):
            raise ValueError("screenshot request terminal precedes its CPU pixels")
        del self.active[identity]
        witness = dict(identity=identity, parent=0, mode=start["mode"],
            mode_name=MODES[start["mode"]], state=value, outcome=OUTCOMES[value],
            **{key: item for key, item in start.items() if key != "mode"},
            terminal_raw_index=index, terminal_timestamp_us=event["ts"], terminal_thread=thread)
        self.closed += 1
        self.outcomes[value] += 1
        if self.first is None:
            self.first = witness
        self.last = witness

    def finish(self, metadata):
        # A partial/resident window cannot establish the shutdown boundary.
        if metadata.get("mode") != "continuous" or metadata.get("capture_complete") is not True:
            raise ValueError("screenshot requests require a complete continuous capture")
        if self.active or self.closed != self.admitted:
            raise ValueError("screenshot request is missing its unique terminal")
        return _exact(dict(schema=1,
            status="CPU_SCREENSHOT_REQUEST_TERMINALS_OBSERVED" if self.closed else "NO_ACTIVITY",
            activity_observed=bool(self.closed), admitted_count=self.admitted, terminal_count=self.closed,
            mode_counts=dict(zip(MODES, self.modes)), outcome_counts=dict(zip(OUTCOMES, self.outcomes)),
            peak_unclosed_admissions=self.peak_active, admission_serial_high_water=self.last_admit,
            first=self.first, last=self.last,
            cpu_pixels_count=self.cpu_pixels_count,
            first_cpu_pixels=self.first_cpu_pixels, last_cpu_pixels=self.last_cpu_pixels,
            integer_encoding="exact integers; values wider than 53 bits are decimal strings",
            witness_order="first/last in exported terminal order; every ledger event validated",
            scope="complete captured CPU request outcomes under continuously enabled capture; "
                  "CPU pixels witness the validated backend callback payload; "
                  "NO_ACTIVITY is not screenshot acceptance; no GPU completion, durable storage or task lifetime"))
