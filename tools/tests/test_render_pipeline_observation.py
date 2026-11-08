"""Effective pipeline packets through the canonical memory and file readers."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools import lifecycle_trace_report as reporter
from tools import render_pipeline_observation as rp


VALUES = {
    "volfog": 0, "csm": 1, "ssao": 0, "ssr": 0, "taa": 0, "bloom": 1, "mblur": 0,
    "scale_milli": 1000, "shadow": 2048, "cascades": 3, "shadow_filter": 2,
    "backbuffer_msaa": 1, "post_quality": 2, "hdr": 1, "depth_prepass": 1, "ssgi": 0,
}


def publication(identity=1, thread=7, ts=1000, fields=None, **changes):
    fields = rp.FULL_FIELDS if fields is None else fields
    values = dict(VALUES, **changes)
    base = dict(pid=1, tid=thread)
    events = [
        dict(base, ph="i", cat="task", ts=ts - 1, name=rp.SERVICE,
             args=dict(task=identity, parent=0, kind=2)),
        dict(base, ph="B", cat="task", ts=ts, name=rp.SERVICE,
             args=dict(id=identity, parent=identity)),
    ]
    for field in fields:
        events.append(dict(base, ph="C", cat="core", ts=ts + len(events) - 1,
                           name=rp.PREFIX + field,
                           args=dict(id=identity, parent=identity, value=values[field])))
    events.append(dict(base, ph="E", cat="task", ts=ts + len(events) + 1, name=rp.SERVICE,
                       args=dict(id=identity, parent=identity, ns=10000, state=0)))
    return events


def capture(*publications):
    events = [event for publication in publications for event in publication]
    return dict(jceTrace=dict(recorded=len(events), captured=len(events),
                             overwritten_before_capture=0, cursor_lost=0, lost=0,
                             capture_generation_stable=True), traceEvents=events)


def observe(*publications):
    return reporter.analyze_trace(capture(*publications), [rp.SERVICE],
                                  render_pipeline_observations=True)["render_pipeline_observations"]


class RenderPipelineObservationTests(unittest.TestCase):
    def test_full_packet_matches_memory_and_bounded_stream_reader(self):
        identity, thread = (1 << 63) + 11, (1 << 53) + 7
        events = publication(identity, thread)
        document = capture(events)
        output = Path(__file__).resolve().parents[2] / "build/tests/render-pipeline-observation"
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="cpu-", dir=output) as folder:
            path = Path(folder) / "trace.json"
            path.write_text(json.dumps(document, allow_nan=False), encoding="utf-8")
            streamed = reporter.analyze_trace_file(path, [rp.SERVICE],
                render_pipeline_observations=True, chunk_bytes=7)
        row = streamed["render_pipeline_observations"]
        self.assertEqual(row, observe(events))
        self.assertEqual(row["packet_schema"], "full")
        self.assertEqual(row["publication_count"], 1)
        self.assertEqual(row["first"], row["last"])
        self.assertEqual(row["first"]["values"], VALUES)
        self.assertEqual(row["first"]["scope_id"], str(identity))
        self.assertEqual(row["first"]["thread"], str(thread))
        self.assertEqual(row["first"]["field_raw_indices"], dict(zip(rp.FULL_FIELDS, range(2, 18))))
        self.assertEqual(row["unavailable_fields"], [])
        self.assertTrue(streamed["validation_complete"])

    def test_legacy_publication_reports_unavailable_new_fields(self):
        row = observe(publication(fields=rp.LEGACY_FIELDS))
        self.assertEqual(row["packet_schema"], "legacy")
        self.assertEqual(row["first"]["values"], {k: VALUES[k] for k in rp.LEGACY_FIELDS})
        self.assertEqual(row["unavailable_fields"], [
            "shadow_filter", "backbuffer_msaa", "post_quality", "hdr", "depth_prepass", "ssgi",
        ])

    def test_first_last_and_change_count_do_not_retain_a_history(self):
        row = observe(publication(1, ts=1000), publication(2, ts=2000),
                      publication(3, ts=3000, shadow=4096),
                      publication(4, ts=4000, shadow=4096))
        self.assertEqual(row["publication_count"], 4)
        self.assertEqual(row["policy_changes"], 1)
        self.assertEqual(row["first"]["scope_id"], 1)
        self.assertEqual(row["last"]["scope_id"], 4)
        self.assertEqual(row["last"]["values"]["shadow"], 4096)

    def test_mixed_schemas_refused_in_both_orders(self):
        for first, second in ((rp.LEGACY_FIELDS, rp.FULL_FIELDS), (rp.FULL_FIELDS, rp.LEGACY_FIELDS)):
            with self.subTest(first=len(first)), self.assertRaisesRegex(ValueError, "mixes legacy and full"):
                observe(publication(1, ts=1000, fields=first),
                        publication(2, ts=2000, fields=second))

    def test_incomplete_packets_refused(self):
        for count in (0, 9, 11, 15):
            with self.subTest(count=count), self.assertRaisesRegex(ValueError, "incomplete fields"):
                observe(publication(fields=rp.FULL_FIELDS[:count]))

    def test_duplicate_unknown_and_reordered_fields_refused(self):
        for replacement in ("volfog", "future_field", "ssao"):
            events = publication()
            events[3]["name"] = rp.PREFIX + replacement
            with self.subTest(field=replacement), self.assertRaisesRegex(ValueError, "ordered packet fields"):
                observe(events)

    def test_exact_typed_values_and_ranges(self):
        cases = (
            ("taa", 2), ("taa", True), ("hdr", 0.0), ("scale_milli", 249),
            ("scale_milli", 2001), ("shadow", 65536), ("cascades", 256),
            ("shadow_filter", -1), ("backbuffer_msaa", 256), ("post_quality", 4),
        )
        for field, value in cases:
            with self.subTest(field=field, value=value), self.assertRaisesRegex(ValueError, "typed range"):
                observe(publication(**{field: value}))

    def test_wrong_owner_thread_or_category_refused(self):
        for fault in ("parent", "identity", "thread", "category"):
            events = publication()
            if fault == "parent":
                events[2]["args"]["parent"] = 2
            elif fault == "identity":
                events[2]["args"]["id"] = events[2]["args"]["parent"] = 2
            elif fault == "thread":
                events[2]["tid"] = 8
            else:
                events[2]["cat"] = "render"
            with self.subTest(fault=fault), self.assertRaisesRegex(ValueError, "wrong active owner"):
                observe(events)

    def test_scope_order_and_success_are_required(self):
        events = publication()
        events[3]["ts"] = events[2]["ts"] - 1
        with self.assertRaisesRegex(ValueError, "timestamp moved backwards"):
            observe(events)
        observer = rp.RenderPipelineObservation()
        start = (rp.SERVICE, 1000, 1, (2, rp.SERVICE, 999, 0))
        observer.begin(events[1], 1, start)
        active = {("task", 7, 1): start}
        observer.counter(events[2], 2, active)
        with self.assertRaisesRegex(ValueError, "ordered scope interval"):
            observer.counter(events[3], 3, active)
        events = publication()
        events[-1]["args"]["state"] = 1
        with self.assertRaisesRegex(ValueError, "did not close successfully"):
            observe(events)

    def test_observer_active_budget_and_unfinished_scope(self):
        observer = rp.RenderPipelineObservation(max_active=1)
        event = publication()[1]
        observer.begin(event, 1, (rp.SERVICE, 1000, 1, (2, rp.SERVICE, 999, 0)))
        other = publication(2, ts=2000)[1]
        with self.assertRaisesRegex(ValueError, "active scope limit"):
            observer.begin(other, 3, (rp.SERVICE, 2000, 2, (2, rp.SERVICE, 1999, 0)))
        with self.assertRaisesRegex(ValueError, "unfinished scopes"):
            observer.finish()

    def test_opt_in_requires_a_publication_and_default_output_stays_unchanged(self):
        document = capture(publication())
        self.assertNotIn("render_pipeline_observations",
                         reporter.analyze_trace(document, [rp.SERVICE]))
        events = publication(fields=())
        for event in events:
            event["name"] = "another.service"
        with self.assertRaisesRegex(ValueError, "no publication"):
            reporter.analyze_trace(capture(events), ["another.service"],
                                   render_pipeline_observations=True)


if __name__ == "__main__":
    unittest.main()
