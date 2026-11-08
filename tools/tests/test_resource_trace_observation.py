"""Actual canonical parser + resource consumer controls, not a second JSON parser."""
import copy
import hashlib
import io
import json
import sys
import tempfile
import types
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools import lifecycle_trace_report as reporter
from tools import resource_trace_observation as resource


def sample(identity=1, timestamp=1000, unknown=False, tid=(1 << 53)+7):
    values={name:0 for name in resource.FLAGS+resource.SCALARS+resource.WIDE}
    values.update(schema=1, sample_ns=(1 << 63)+identity, owner_thread_id=tid,
        **{"tagged.partial_scope":1,"tagged.current_bytes":17,"tagged.peak_bytes":27})
    if not unknown:
        values.update(**{"memory.copied":1,"memory.rss_available":1,"memory.commit_available":1,
            "memory.rss_bytes":(1 << 53)+123456,"memory.commit_bytes":(1 << 54)+456789,
            "async.available":1,"async.mode":2,"async.live_tasks":1,"async.queued_tasks":1,
            "async.submitted_tasks":(1 << 63)+9,"gpu.query_allowed":1,"gpu.copied":1,
            "gpu.context_available":1,"gpu.backend_available":1,"gpu.timer_available":1,
            "gpu.reason_available":1,"gpu.context_generation":(1 << 54)+1,"gpu.read_host_frame":50,
            "gpu.source_frame":9,"gpu.frequency":1000000,"gpu.raw_begin":(1 << 53)+11,
            "gpu.raw_end":(1 << 53)+1015})
    counters={name:values[name] for name in resource.FLAGS+resource.SCALARS}
    for name in resource.WIDE:
        bits=values[name]&((1 << 64)-1)
        counters[name+".hi32"]=bits >> 32; counters[name+".lo32"]=bits&0xffffffff
    base=dict(pid=1,tid=tid)
    events=[dict(base,ph="i",cat="task",ts=timestamp-100,name=resource.SERVICE,
                 args=dict(task=identity,parent=123,kind=2)),
            dict(base,ph="B",cat="task",ts=timestamp,name=resource.SERVICE,
                 args=dict(id=identity,parent=identity))]
    events.extend(dict(base,ph="C",cat="core",ts=timestamp+10+index,name=resource.PREFIX+key,
                       args=dict(id=identity,parent=123,value=value))
                  for index,(key,value) in enumerate(counters.items()))
    events.append(dict(base,ph="E",cat="task",ts=timestamp+100,name=resource.SERVICE,
                       args=dict(id=identity,parent=identity,ns=100000,state=0)))
    return events


def capture(*groups):
    events=[event for group in groups for event in group]
    return dict(jceTrace=dict(recorded=len(events),captured=len(events),overwritten_before_capture=0,
                             cursor_lost=0,lost=0,capture_generation_stable=True),traceEvents=events)


def cooperative_zero_capture():
    doc=capture(sample(tid=0))
    doc["traceEvents"].insert(0,dict(ph="M",pid=1,tid=0,name="thread_name",args=dict(name="MAIN")))
    doc["jceTrace"].update(mode="continuous",consumer_mode="cooperative",worker_available=False,
        write_ok=True,capture_complete=True,unexported=0,active_threads=0,active_work_items=0,
        active_waits=0,recorded=len(doc["traceEvents"]),captured=len(doc["traceEvents"]))
    return doc

def counter(doc,name):
    return next(event for event in doc["traceEvents"] if event["name"]==resource.PREFIX+name)


class ResourceObservationTests(unittest.TestCase):
    def setUp(self):
        output=Path(__file__).resolve().parents[2]/"build/tests/resource-trace-observation"
        output.mkdir(parents=True,exist_ok=True)
        self.folder=tempfile.TemporaryDirectory(prefix="cpu-",dir=output)
        self.addCleanup(self.folder.cleanup)
        self.path=Path(self.folder.name)/"trace.json"

    def file(self,doc,**kwargs):
        body=json.dumps(doc,allow_nan=False).encode()
        self.path.write_bytes(body)
        return reporter.analyze_trace_file(self.path,[resource.SERVICE],resource_observations=True,
                                            chunk_bytes=7,**kwargs)

    def test_wide_pairs_raw_signed_unknowns_and_repeated_latched_gpu_remain_exact(self):
        doc=capture(sample(),sample(2,2000,unknown=True),sample(3,3000))
        memory=reporter.analyze_trace(doc,[resource.SERVICE],resource_observations=True)
        streamed=self.file(doc)
        self.assertEqual(memory["resource_observations"],streamed["resource_observations"])
        data=streamed["resource_observations"]
        self.assertEqual(data["samples_count"],3)
        self.assertEqual(data["repeated_previous_available_gpu_tuple_count"],1)
        self.assertEqual(data["sampled_fields"]["memory.rss_bytes"],dict(known_count=2,unknown_count=1,
            sampled_min=str((1 << 53)+123456),sampled_max=str((1 << 53)+123456)))
        self.assertEqual(data["first"]["values"]["gpu.raw_begin"],str((1 << 53)+11))
        self.assertAlmostEqual(data["sampled_fields"]["gpu.duration_ms"]["sampled_min"],1.004)
        self.assertEqual(data["last"]["values"]["owner_thread_id"],str((1 << 53)+7))
        self.assertTrue(streamed["source_fingerprint_stable"])
        self.assertEqual(streamed["sha256"],hashlib.sha256(self.path.read_bytes()).hexdigest())
        self.assertIn("partial tagged",data["scope"])
        self.assertIn("no CPU/GPU join",data["scope"])

    def test_named_cooperative_zero_thread_uses_the_canonical_complete_authority(self):
        doc=cooperative_zero_capture()
        try:
            memory=reporter.analyze_trace(doc,[resource.SERVICE],resource_observations=True)
            streamed=self.file(doc)
        except ValueError as error:
            self.fail("Named cooperative zero-thread resource sample was rejected: "+str(error))
        self.assertEqual(memory["thread_identity_scope"],"named_single_cooperative_thread")
        self.assertEqual(streamed["thread_identity_scope"],memory["thread_identity_scope"])
        self.assertEqual(memory["resource_observations"],streamed["resource_observations"])
        data=memory["resource_observations"]
        self.assertEqual(data["samples_count"],1)
        self.assertEqual(data["first"]["values"]["owner_thread_id"],0)
        self.assertEqual(data["sampled_fields"]["owner_thread_id"]["known_count"],1)
        self.assertEqual(data["sampled_fields"]["owner_thread_id"]["sampled_min"],0)

    def test_zero_thread_without_complete_cooperative_authority_is_rejected(self):
        for mutation in ("unnamed","ambiguous","threaded","worker","mixed","resident","owner"):
            doc=cooperative_zero_capture()
            if mutation=="unnamed":doc["traceEvents"].pop(0)
            elif mutation=="ambiguous":
                doc["traceEvents"].append(dict(ph="M",pid=1,tid=0,name="thread_name",args=dict(name="OTHER")))
            elif mutation=="threaded":doc["jceTrace"].update(consumer_mode="threaded",worker_available=True)
            elif mutation=="worker":doc["jceTrace"]["worker_available"]=True
            elif mutation=="mixed":
                doc["traceEvents"].append(dict(ph="M",pid=1,tid=7,name="thread_name",args=dict(name="OTHER")))
            elif mutation=="resident":doc["jceTrace"]["mode"]="resident"
            else:counter(doc,"owner_thread_id.lo32")["args"]["value"]=1
            doc["jceTrace"].update(recorded=len(doc["traceEvents"]),captured=len(doc["traceEvents"]))
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):self.file(doc)

    def test_old_positive_only_full_consumer_fails_the_unchanged_zero_thread_test(self):
        source=Path(resource.__file__).read_text(encoding="utf-8")
        needle='        if not identity or args["kind"] != 2 or event["cat"] != "task":'
        self.assertEqual(source.count(needle),1)
        former=source.replace(needle,'        if not identity or not tid or args["kind"] != 2 or event["cat"] != "task":',1)
        owner=types.ModuleType("jce_resource_old_positive_thread_control")
        exec(compile(former,str(resource.__file__),"exec"),owner.__dict__)
        unchanged=ResourceObservationTests("test_named_cooperative_zero_thread_uses_the_canonical_complete_authority")
        with mock.patch.object(resource,"ResourceTraceObservation",owner.ResourceTraceObservation):
            result=unittest.TextTestRunner(stream=io.StringIO()).run(unittest.TestSuite([unchanged]))
        self.assertEqual(result.testsRun,1)
        self.assertEqual(len(result.failures),1)
        self.assertEqual(len(result.errors),0)
        self.assertIn("Named cooperative zero-thread resource sample was rejected",result.failures[0][1])

    def test_negative_raw_gpu_clock_is_preserved_and_timer_is_unknown(self):
        doc=capture(sample())
        for name in ("gpu.frequency","gpu.raw_begin","gpu.raw_end"):
            counter(doc,name+".hi32")["args"]["value"]=0xffffffff
            counter(doc,name+".lo32")["args"]["value"]=0xfffffffe
        counter(doc,"gpu.timer_available")["args"]["value"]=0
        counter(doc,"gpu.reason")["args"]["value"]=3
        data=self.file(doc)["resource_observations"]
        self.assertEqual(data["first"]["values"]["gpu.frequency"],-2)
        self.assertIsNone(data["first"]["values"]["gpu.duration_ms"])
        self.assertEqual(data["sampled_fields"]["gpu.duration_ms"]["unknown_count"],1)

    def test_default_is_unchanged_and_explicit_missing_data_fails(self):
        doc=capture(sample())
        for event in doc["traceEvents"]:
            if event["name"]==resource.SERVICE: event["name"]="app.init"
        doc["traceEvents"]=[event for event in doc["traceEvents"] if event["ph"]!="C"]
        doc=capture(doc["traceEvents"])
        plain=reporter.analyze_trace(doc,["app.init"])
        self.assertNotIn("resource_observations",plain)
        with self.assertRaisesRegex(ValueError,"no resource samples"):
            reporter.analyze_trace(doc,["app.init"],resource_observations=True)
        with self.assertRaisesRegex(ValueError,"explicit bool"):
            reporter.analyze_trace(doc,["app.init"],resource_observations=1)
        bad=capture(sample()); counter(bad,"schema")["args"]["value"]=99
        self.assertNotIn("resource_observations",reporter.analyze_trace(bad,[resource.SERVICE]))

    def test_ordinary_bad_counters_schema_flags_and_half_pairs_reject(self):
        mutations=(("schema",2),("memory.rss_available",2),("host_headless",True),
                   ("tagged.partial_scope",0),("sample_ns.hi32",1 << 32),
                   ("sample_ns.lo32",-1),("async.mode",3),("async.live_tasks",.5),
                   ("pid_available",1),("memory.rss_available",0),("gpu.timer_available",0),
                   ("gpu.frequency.lo32",0),("gpu.context_available",0),("async.available",0))
        for name,value in mutations:
            doc=capture(sample()); counter(doc,name)["args"]["value"]=value
            with self.subTest(name=name),self.assertRaises(ValueError):self.file(doc)
        for mode in ("missing","duplicate","unknown"):
            doc=capture(sample()); item=counter(doc,"memory.rss_bytes.lo32")
            if mode=="missing":doc["traceEvents"].remove(item)
            elif mode=="duplicate":doc["traceEvents"].insert(-1,copy.deepcopy(item))
            else:item["name"]="resource.extra_counter"
            doc=capture(doc["traceEvents"])
            with self.subTest(mode=mode),self.assertRaises(ValueError):self.file(doc)

    def test_wrong_owner_parent_thread_or_category_cannot_be_consumed(self):
        for key,value in (("id",2),("parent",124),("value",(1 << 32))):
            doc=capture(sample());counter(doc,"schema")["args"][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):self.file(doc)
        for key,value in (("tid",7),("cat","render")):
            doc=capture(sample());counter(doc,"schema")[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):self.file(doc)
        doc=capture(sample());doc["traceEvents"][0]["tid"]=7
        with self.assertRaisesRegex(ValueError,"owner/thread"):self.file(doc)
        doc=capture(sample());counter(doc,"owner_thread_id.lo32")["args"]["value"]=8
        with self.assertRaisesRegex(ValueError,"owner thread"):self.file(doc)

    def test_whole_scope_footer_and_loss_gates_remain_strict(self):
        for mutation in ("unfinished","failed","cancelled","loss","generation","footer-count"):
            doc=capture(sample())
            if mutation=="unfinished":doc=capture(doc["traceEvents"][:-1])
            elif mutation in ("failed","cancelled"):doc["traceEvents"][-1]["args"]["state"]=1 if mutation=="failed" else 2
            elif mutation=="loss":doc["jceTrace"].update(cursor_lost=1,lost=1)
            elif mutation=="generation":doc["jceTrace"]["capture_generation_stable"]=False
            else:doc["jceTrace"]["captured"]+=1;doc["jceTrace"]["recorded"]+=1
            with self.subTest(mutation=mutation),self.assertRaises(reporter.TraceFileError) as caught:self.file(doc)
            self.assertEqual(caught.exception.evidence["sha256"],hashlib.sha256(self.path.read_bytes()).hexdigest())
        doc=capture(sample());self.path.write_bytes(json.dumps(doc).encode()+b" trailing")
        with self.assertRaises(reporter.TraceFileError):
            reporter.analyze_trace_file(self.path,[resource.SERVICE],resource_observations=True,chunk_bytes=1)

    def test_resource_report_cannot_bypass_actual_second_read_file_identity(self):
        doc=capture(sample());self.path.write_text(json.dumps(doc),encoding="utf-8")
        real_open=Path.open
        path=self.path
        class ChangingReader:
            def __init__(self,stream):self.stream=stream;self.changed=False
            def __enter__(self):return self
            def __exit__(self,*args):return self.stream.__exit__(*args)
            def __getattr__(self,name):return getattr(self.stream,name)
            def seek(self,offset,*args):
                if not self.changed and offset==0:
                    self.changed=True
                    with real_open(path,"ab") as output:output.write(b" ")
                return self.stream.seek(offset,*args)
        def open_port(actual,*args,**kwargs):
            stream=real_open(actual,*args,**kwargs)
            return ChangingReader(stream) if actual==path and args and args[0]=="rb" else stream
        with mock.patch.object(Path,"open",open_port),self.assertRaises(reporter.TraceFileError) as caught:
            reporter.analyze_trace_file(path,[resource.SERVICE],resource_observations=True,chunk_bytes=7)
        evidence=caught.exception.evidence
        self.assertFalse(evidence["source_fingerprint_stable"])
        self.assertNotEqual(evidence["sha256"],evidence["source_sha256_after"])
        self.assertTrue(evidence["json_utf8_eof_validated"])

    def test_active_fixed_cap_rejects_without_unbounded_identity_history(self):
        analysis=reporter._TraceAnalysis([resource.SERVICE],[],[],resource_observations=True)
        for identity in range(1,resource.MAX_ACTIVE+1):analysis.feed(sample(identity)[0])
        with self.assertRaisesRegex(ValueError,"active sample limit"):analysis.feed(sample(99)[0])

    def test_cli_switch_and_cross_chunk_stream_use_canonical_whole_parser(self):
        doc=capture(sample());self.path.write_text(json.dumps(doc),encoding="utf-8")
        out=Path(self.folder.name)/"summary.json"
        args=["lifecycle_trace_report",str(self.path),"--require",resource.SERVICE,"--out",str(out),"--resource-observations"]
        with mock.patch.object(sys,"argv",args),redirect_stdout(io.StringIO()):self.assertEqual(reporter.main(),0)
        data=json.loads(out.read_text());self.assertEqual(data["resource_observations"]["samples_count"],1)
        reference=data["resource_observations"]
        for chunk in (1,13,1024):
            report=reporter.analyze_trace_file(self.path,[resource.SERVICE],resource_observations=True,chunk_bytes=chunk)
            self.assertEqual(report["resource_observations"],reference)
            self.assertTrue(report["json_utf8_eof_validated"])


if __name__=="__main__":unittest.main()
