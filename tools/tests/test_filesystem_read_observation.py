"""Existing fs.read packets through the real canonical memory/file/CLI owner."""
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools import lifecycle_trace_report as reporter
from tools import filesystem_read_observation as reads


def sample(identity=10, parent=77, thread=7, timestamp=1000,
           service="fs.host.read_all", state=1, values=None):
    fields = dict(path_hash_hi=reads.UINT32_MAX, path_hash_lo=reads.UINT32_MAX,
        total_hi=0, total_lo=0, requested_hi=0, requested_lo=0,
        cap_hi=0, cap_lo=0, actual_hi=0, actual_lo=0,
        dispatch=1, known=8, failure_stage=3, close=0, short=0)
    fields.update(values or {})
    base = dict(pid=1, tid=thread)
    events = [dict(base, ph="i", cat="task", ts=timestamp - 1, name=service,
                   args=dict(task=identity, parent=parent, kind=2)),
              dict(base, ph="B", cat="task", ts=timestamp, name=service,
                   args=dict(id=identity, parent=identity))]
    events.extend(dict(base, ph="C", cat="core", ts=timestamp + 1 + n,
        name=reads.PREFIX + field, args=dict(id=identity, parent=parent, value=value))
        for n, (field, value) in enumerate(fields.items()))
    events.append(dict(base, ph="E", cat="task", ts=timestamp + 30, name=service,
        args=dict(id=identity, parent=identity, ns=30000, state=state)))
    return events


def capture(*groups, init_thread=7):
    events = [dict(pid=1, tid=init_thread, ph="i", cat="task", ts=1, name="app.init",
                   args=dict(task=1, parent=0, kind=2)),
              dict(pid=1, tid=init_thread, ph="B", cat="task", ts=2, name="app.init",
                   args=dict(id=1, parent=1)),
              dict(pid=1, tid=init_thread, ph="E", cat="task", ts=3, name="app.init",
                   args=dict(id=1, parent=1, ns=1000, state=0))]
    events += [event for group in groups for event in group]
    return dict(jceTrace=dict(recorded=len(events), captured=len(events), lost=0,
        overwritten_before_capture=0, cursor_lost=0, capture_generation_stable=True), traceEvents=events)


def success(**changes):
    values = dict(total_lo=7, requested_lo=7, actual_lo=7, known=11,
                  failure_stage=0, close=1, short=1)
    values.update(changes)
    return values


class FilesystemReadTests(unittest.TestCase):
    def setUp(self):
        root = Path(__file__).resolve().parents[2] / "build/tests/filesystem-read-observation"
        root.mkdir(parents=True, exist_ok=True)
        self.folder = tempfile.TemporaryDirectory(prefix="cpu-", dir=root)
        self.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / "trace.json"

    def memory(self, document, **options):
        return reporter.analyze_trace(document, ["app.init"], filesystem_read_details=True, **options)

    def file(self, document, **options):
        self.path.write_text(json.dumps(document, allow_nan=False), encoding="utf-8")
        return reporter.analyze_trace_file(self.path, ["app.init"], filesystem_read_details=True,
                                         chunk_bytes=7, **options)

    def test_failed_read_details_keep_exact_request_hash_caller_and_unknowns(self):
        document = capture(sample())
        report = self.file(document)
        data = report["filesystem_read_details"]
        self.assertEqual(data, self.memory(document)["filesystem_read_details"])
        self.assertEqual(data["complete_packets_count"], 1)
        row = data["diagnostic_rows"][0]
        self.assertEqual((row["scope_id"], row["submitted_parent"], row["thread"]), (10,77,7))
        self.assertEqual(row["path_hash"], str(reads.UINT64_MAX))
        self.assertEqual(row["path_hash_hex"], "ffffffffffffffff")
        self.assertEqual((row["dispatch_name"], row["failure_stage_name"]), ("host","open"))
        self.assertEqual(row["bytes"], dict(total=None, requested=None, cap=None, actual=0))
        self.assertIsNone(row["optionality"]); self.assertIsNone(row["errno"])
        self.assertEqual(row["known_mask"], 8)
        self.assertEqual(len(row["field_raw_indices"]),15)
        self.assertEqual(report["nonrequired_task_outcomes"]["failed"],1)
        self.assertTrue(report["validation_complete"])
        self.assertEqual(report["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())

    def test_default_and_explicit_off_are_identical_and_do_not_import_collector(self):
        document = capture(sample(values=dict(dispatch=99)))
        with mock.patch.dict(sys.modules, {"tools.filesystem_read_observation": None}):
            default = reporter.analyze_trace(document,["app.init"])
            off = reporter.analyze_trace(document,["app.init"],filesystem_read_details=False)
        self.assertEqual(default,off); self.assertNotIn("filesystem_read_details",default)
        with self.assertRaises(ValueError): self.memory(document)
        for value in (1,0,None,"true",[]):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError,"explicit bool"):
                reporter.analyze_trace(capture(),["app.init"],filesystem_read_details=value)
        self.path.write_text(json.dumps(document),encoding="utf-8")
        default = reporter.analyze_trace_file(self.path,["app.init"])
        off = reporter.analyze_trace_file(self.path,["app.init"],filesystem_read_details=False)
        self.assertEqual(default,off)

    def test_all15_fields_can_be_reordered_without_changing_semantics(self):
        events = sample()
        events[2:-1] = reversed(events[2:-1])
        for n,event in enumerate(events[2:-1]): event["ts"] = 1001+n
        row = self.file(capture(events))["filesystem_read_details"]["diagnostic_rows"][0]
        self.assertEqual(row["path_hash_hex"],"ffffffffffffffff")
        self.assertEqual(row["failure_stage"],3)
        self.assertEqual(row["field_raw_indices"]["short"],5)

    def test_known_zero_bytes_remain_distinct_from_missing_older_detail_packets(self):
        old = [e for e in sample(identity=11,timestamp=2000) if e["ph"]!="C"]
        document = capture(sample(state=0,values=success(total_lo=0,requested_lo=0,actual_lo=0,short=0)),old)
        data = self.file(document)["filesystem_read_details"]
        self.assertEqual((data["complete_packets_count"],data["no_detail_scopes_count"]),(1,1))
        self.assertEqual(data["first_complete"]["bytes"],dict(total=0,requested=0,cap=None,actual=0))
        row = data["diagnostic_rows"][0]
        self.assertFalse(row["details_available"])
        self.assertIsNone(row["path_hash"]); self.assertIsNone(row["failure_stage"])
        self.assertTrue(all(v is None for v in row["bytes"].values()))
        unavailable = self.file(capture(old))["filesystem_read_details"]
        self.assertEqual(unavailable["status"],"DETAILS_UNAVAILABLE")
        self.assertEqual(unavailable["scope_outcomes"]["failed"],1)
        empty = self.file(capture())["filesystem_read_details"]
        self.assertEqual(empty["status"],"NO_ACTIVITY")
        self.assertEqual(empty["service_scopes_count"],0)

    def test_null_zero_and_empty_request_hashes_are_preserved_without_validity_claim(self):
        values = [0,14695981039346656037]
        groups = [sample(identity=10+n,timestamp=1000+n*100,
            values=dict(path_hash_hi=value>>32,path_hash_lo=value&reads.UINT32_MAX,
                        dispatch=0,known=0,failure_stage=1)) for n,value in enumerate(values)]
        data = self.file(capture(*groups))["filesystem_read_details"]
        rows = data["diagnostic_rows"]
        self.assertEqual(rows[0]["path_hash"],0)
        self.assertEqual(rows[1]["path_hash_hex"],"cbf29ce484222325")
        self.assertIn("not canonical",data["request_identity_scope"])
        self.assertNotIn("path_valid",rows[0]); self.assertNotIn("path",rows[0])

    def test_capped_short_and_close_failed_successes_keep_original_task_outcomes(self):
        document = capture(sample(state=0,values=success(close=2)),
            sample(identity=11,timestamp=2000,service="fs.host.read_capped",state=0,
                values=success(total_lo=20,cap_lo=8,requested_lo=8,actual_lo=3,known=15,short=2)))
        data = self.file(document)["filesystem_read_details"]
        self.assertEqual(data["scope_outcomes"],dict(succeeded=2,failed=0,cancelled=0))
        self.assertEqual((data["observed_close_failed_count"],data["observed_short_count"]),(1,1))
        self.assertEqual(data["diagnostic_eligible_count"],2)
        self.assertEqual([r["outcome"] for r in data["diagnostic_rows"]],["succeeded","succeeded"])
        self.assertEqual(data["diagnostic_rows"][1]["bytes"],dict(total=20,requested=8,cap=8,actual=3))

    def test_wide_bytes_and_all_dispatch_stage_enums_remain_exact(self):
        groups=[]
        for dispatch in range(4):
            for stage in range(8):
                n=len(groups)
                groups.append(sample(identity=10+n,timestamp=1000+n*100,
                    state=int(stage!=0),values=dict(dispatch=dispatch,failure_stage=stage)))
        data=self.file(capture(*groups))["filesystem_read_details"]
        self.assertEqual(data["complete_packets_count"],32)
        self.assertEqual(len(data["aggregates"]),32)
        wide=success(total_hi=reads.UINT32_MAX,total_lo=reads.UINT32_MAX,
            requested_hi=reads.UINT32_MAX,requested_lo=reads.UINT32_MAX,
            actual_hi=reads.UINT32_MAX,actual_lo=reads.UINT32_MAX)
        row=self.file(capture(sample(state=0,values=wide)))["filesystem_read_details"]["first_complete"]
        self.assertEqual(row["bytes"]["total"],str(reads.UINT64_MAX))
        self.assertEqual(row["wire_fields"]["total_hi"],reads.UINT32_MAX)

    def test_incomplete_duplicate_unknown_and_typed_fields_reject(self):
        for n in range(len(reads.FIELDS)):
            for mode in ("missing","duplicate"):
                events=sample()
                if mode=="missing":events.pop(2+n)
                else:events.insert(2+n,copy.deepcopy(events[2+n]))
                with self.subTest(field=reads.FIELDS[n],mode=mode), self.assertRaises(ValueError):
                    self.file(capture(events))
        for field,bad in (("path_hash_hi",-1),("path_hash_lo",1<<32),("known",16),
                          ("dispatch",4),("failure_stage",8),("close",3),("short",3)):
            with self.subTest(field=field),self.assertRaises(ValueError):
                self.file(capture(sample(values={field:bad})))
        for bad in (True,False,1.0,"1",None):
            with self.subTest(bad=bad),self.assertRaises(ValueError):
                self.file(capture(sample(values=dict(total_lo=bad))))
        events=sample();events[2]["name"]="fs.read.unpublished_field"
        with self.assertRaises(ValueError):self.file(capture(events))

    def test_wrong_identity_caller_thread_category_phase_and_boundary_reject(self):
        mutations=[("args","id",11),("args","parent",10),("args","parent",0),
            ("args","id",0),("args","id",True),("args","parent",1.0),
            ("args","id",1<<64),("event","tid",8),("event","cat","render"),
            ("event","ph","B")]
        for where,key,value in mutations:
            events=sample();(events[2]["args"] if where=="args" else events[2])[key]=value
            with self.subTest(where=where,key=key,value=value),self.assertRaises(ValueError):
                self.file(capture(events))
        for mode in ("before","after","reversed"):
            events=sample()
            if mode=="before":events.insert(1,events.pop(2))
            elif mode=="after":events.append(events.pop(2))
            else:events[3]["ts"]=999
            with self.subTest(mode=mode),self.assertRaises(ValueError):self.file(capture(events))

    def test_interleaved_threads_and_nested_callers_retire_on_canonical_end(self):
        left,right=sample(),sample(identity=11,thread=8)
        events=left[:2]+right[:2]
        for a,b in zip(left[2:-1],right[2:-1]):events.extend((a,b))
        events += [left[-1],right[-1]]
        data=self.file(capture(events))["filesystem_read_details"]
        self.assertEqual(data["peak_active_scopes"],2)
        self.assertEqual(data["complete_packets_count"],2)
        outer=sample(identity=20,parent=77)
        inner=sample(identity=21,parent=20,timestamp=1001)
        for event in outer[2:]:event["ts"]+=100
        data=self.file(capture(outer[:2]+inner+outer[2:]))["filesystem_read_details"]
        self.assertEqual([r["submitted_parent"] for r in data["diagnostic_rows"]],[20,77])
        for limit,options in (("active",dict(max_active=1)),("history",dict(max_history=2))):
            with self.subTest(limit=limit),self.assertRaises(ValueError):self.file(capture(events),**options)
        sequential=capture(*[sample(identity=20+n,timestamp=1000+n*100) for n in range(100)])
        self.assertEqual(self.file(sequential,max_active=1)["filesystem_read_details"]["peak_active_scopes"],1)

    def test_incomplete_wrong_kind_unpaired_and_required_failures_are_not_hidden(self):
        for mode in ("unclosed","unsubmitted","kind","category","no-begin"):
            events=sample()
            if mode=="unclosed":events.pop()
            elif mode=="unsubmitted":events.pop(0)
            elif mode=="kind":events[0]["args"]["kind"]=0
            elif mode=="category":events[1]["cat"]=events[-1]["cat"]="render"
            else:events=[events[0]]
            with self.subTest(mode=mode),self.assertRaises(ValueError):self.file(capture(events))
        document=capture(sample())
        with self.assertRaisesRegex(ValueError,"required phase missing or unsuccessful"):
            reporter.analyze_trace(document,["fs.host.read_all"],filesystem_read_details=True)

    def test_retention_validates_every_late_packet_and_reports_exact_omissions(self):
        document=capture(*[sample(identity=10+n,timestamp=1000+n*100,
            values=dict(path_hash_lo=n)) for n in range(300)])
        data=self.file(document)["filesystem_read_details"]
        self.assertEqual((data["diagnostic_eligible_count"],data["diagnostic_retained_count"],
                          data["diagnostic_omitted_count"]),(300,256,44))
        self.assertEqual(data["diagnostic_rows"][0]["scope_id"],10)
        self.assertEqual(data["diagnostic_rows"][-1]["scope_id"],309)
        self.assertEqual(len(data["aggregates"]),1)
        self.assertEqual(data["aggregates"][0]["count"],300)
        self.assertLess(len(json.dumps(data)),500000)
        document["traceEvents"][-2]["args"]["value"]=3 # Late short enum is malformed.
        with self.assertRaises(ValueError):self.file(document)

    def test_retention_reuses_history_budget_and_has_no_closed_scope_or_path_map(self):
        observer=reads.FilesystemReadObservation(1,1)
        self.assertEqual(observer.max_retained,1)
        document=capture(sample(),sample(identity=11,timestamp=2000))
        data=self.file(document,max_history=3)["filesystem_read_details"]
        self.assertEqual(data["validation_limits"]["retained_diagnostics"],3)
        for limits in ((0,1),(1,0),(True,1),(1,1.0)):
            with self.subTest(limits=limits),self.assertRaises(ValueError):reads.FilesystemReadObservation(*limits)

    def test_cooperative_zero_thread_reuses_canonical_metadata_admission(self):
        document=capture(sample(thread=0),init_thread=0)
        document["traceEvents"].insert(0,dict(ph="M",pid=1,tid=0,name="thread_name",args=dict(name="MAIN")))
        count=len(document["traceEvents"])
        document["jceTrace"].update(recorded=count,captured=count,mode="continuous",
            consumer_mode="cooperative",worker_available=False,write_ok=True,capture_complete=True,
            unexported=0,active_threads=0,active_work_items=0,active_waits=0)
        self.assertEqual(self.file(document)["filesystem_read_details"]["first_complete"]["thread"],0)
        document["jceTrace"]["consumer_mode"]="threaded"
        with self.assertRaises(ValueError):self.file(document)

    def test_loss_generation_counts_eof_and_file_identity_gates_remain_authoritative(self):
        for mode in ("lost","generation","count"):
            document=capture(sample())
            if mode=="lost":document["jceTrace"].update(lost=1,cursor_lost=1)
            elif mode=="generation":document["jceTrace"]["capture_generation_stable"]=False
            else:document["jceTrace"]["recorded"]+=1
            with self.subTest(mode=mode),self.assertRaises(reporter.TraceFileError) as failed:self.file(document)
            self.assertEqual(failed.exception.evidence["sha256"],hashlib.sha256(self.path.read_bytes()).hexdigest())
        for suffix in (b" trailing",b"\xff"):
            self.path.write_bytes(json.dumps(capture(sample())).encode()+suffix)
            with self.assertRaises(reporter.TraceFileError):
                reporter.analyze_trace_file(self.path,["app.init"],filesystem_read_details=True,chunk_bytes=1)

    def test_cli_and_tiny_chunks_use_the_same_optional_packet_owner(self):
        expected=self.file(capture(sample()))["filesystem_read_details"]
        for chunk in (1,13,1024):
            actual=reporter.analyze_trace_file(self.path,["app.init"],filesystem_read_details=True,chunk_bytes=chunk)
            self.assertEqual(actual["filesystem_read_details"],expected)
        output=Path(self.folder.name)/"summary.json"
        args=["lifecycle_trace_report",str(self.path),"--require","app.init","--filesystem-read-details","--out",str(output)]
        with mock.patch.object(sys,"argv",args),redirect_stdout(io.StringIO()):self.assertEqual(reporter.main(),0)
        self.assertEqual(json.loads(output.read_text())["filesystem_read_details"],expected)
        args.remove("--filesystem-read-details")
        with mock.patch.object(sys,"argv",args),redirect_stdout(io.StringIO()):self.assertEqual(reporter.main(),0)
        self.assertNotIn("filesystem_read_details",json.loads(output.read_text()))


if __name__=="__main__":unittest.main()
