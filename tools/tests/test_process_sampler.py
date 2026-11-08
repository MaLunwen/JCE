"""Live startup observations cannot be stale or inferred after child exit."""
import os
import json
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

from tools.process_sampler import _process_reader, _sample, _creation_time, run_process


class LiveMarkerTests(unittest.TestCase):
    def scratch(self):
        root = Path(__file__).resolve().parents[2] / "build/tests/process-sampler"
        root.mkdir(parents=True, exist_ok=True)
        return tempfile.TemporaryDirectory(dir=root)

    def test_real_live_marker_and_missing_control(self):
        with self.scratch() as temporary:
            root = Path(temporary)
            path = root / "ready"
            result = run_process([sys.executable, "-c",
                "from pathlib import Path; import time; "
                "Path('ready').write_bytes(b'ready'); time.sleep(0.3)"],
                env=os.environ.copy(), cwd=root, timeout=5, output_dir=root,
                interval=0.02, live_markers={"ready": (path, b"ready"),
                    "missing": (root / "missing", b"ready")})
            self.assertEqual(result["returncode"], 0)
            self.assertGreater(result["live_markers_s"]["ready"], 0)
            self.assertLess(result["live_markers_s"]["ready"], result["elapsed_s"])
            self.assertIsNone(result["live_markers_s"]["missing"])
            with self.assertRaises(ValueError):
                run_process(["unused"], env={}, cwd=root, timeout=5,
                    output_dir=root, live_markers={"stale": (path, b"ready")})

    def test_exit_only_marker_cannot_become_startup_latency(self):
        with self.scratch() as temporary:
            root = Path(temporary)
            child = mock.Mock(returncode=0, pid=123)
            child.poll.return_value = 0
            with mock.patch("tools.process_sampler.subprocess.Popen", return_value=child), \
                    mock.patch("tools.process_sampler._process_reader", return_value=None), \
                    mock.patch("tools.process_sampler._has_live_marker", return_value=True) as check:
                result = run_process(["unused"], env={}, cwd=root, timeout=5,
                    output_dir=root, live_markers={"ready": (root / "ready", b"ready")})
            check.assert_not_called()
            self.assertIsNone(result["live_markers_s"]["ready"])

    def test_exit_during_initial_lookup_retains_exit_and_logs(self):
        class Gone(Exception):
            pass
        psutil = SimpleNamespace(Error=Gone, Process=mock.Mock(side_effect=Gone("already exited")))
        with self.scratch() as temporary, mock.patch.dict(sys.modules, psutil=psutil):
            root = Path(temporary)
            result = run_process([sys.executable, "-c", "print('early exit'); raise SystemExit(7)"],
                                 env=os.environ.copy(), cwd=root, timeout=5, output_dir=root)
            self.assertEqual(result["returncode"], 7)
            self.assertFalse(result["timed_out"])
            self.assertFalse(result["sampling_available"])
            self.assertIsNone(result["peak_rss_bytes"])
            self.assertIsNone(result["cpu_time_s"])
            self.assertIn("early exit", result["stdout"])
            self.assertTrue((root / "result.json").is_file())

    def test_initial_lookup_does_not_hide_programming_errors(self):
        psutil = SimpleNamespace(Error=OSError, Process=mock.Mock(side_effect=RuntimeError("bug")))
        with mock.patch.dict(sys.modules, psutil=psutil), self.assertRaises(RuntimeError):
            _process_reader(123)


class ThreadObservationTests(unittest.TestCase):
    def process(self):
        return SimpleNamespace(memory_info=lambda: SimpleNamespace(rss=100, private=200),
            cpu_times=lambda: SimpleNamespace(user=1.25, system=0.25),
            threads=mock.Mock(return_value=[SimpleNamespace(id=7, user_time=0.5, system_time=0.25)]),
            create_time=mock.Mock(return_value=123456.25))

    def test_disabled_does_not_query_threads(self):
        proc = self.process()
        sample = _sample(proc, 0)
        proc.threads.assert_not_called()
        self.assertNotIn("thread_cpu", sample)
        self.assertEqual(sample["cpu_time_s"], 1.5)

    def test_optional_thread_failure_retains_aggregate_sample(self):
        class Denied(Exception):
            pass
        proc = self.process()
        proc.threads.side_effect = Denied()
        with mock.patch.dict(sys.modules, psutil=SimpleNamespace(Error=Denied)):
            sample = _sample(proc, 0, sample_threads=True, thread_limit=8)
        self.assertEqual(sample["rss_bytes"], 100)
        self.assertFalse(sample["thread_cpu_available"])
        self.assertEqual(sample["thread_cpu_reason"], "Denied")
        self.assertIsNone(sample["threads_observed_count"])
        self.assertIsNone(sample["threads_truncated"])
        self.assertEqual(sample["thread_cpu"], [])

    def test_thread_output_is_bounded_and_retains_cumulative_cpu(self):
        proc = self.process()
        proc.threads.return_value = [SimpleNamespace(id=i, user_time=i/4, system_time=0.5)
                                    for i in range(1, 5)]
        sample = _sample(proc, 0, sample_threads=True, thread_limit=2)
        self.assertTrue(sample["thread_cpu_available"])
        self.assertTrue(sample["threads_truncated"])
        self.assertEqual(sample["threads_observed_count"], 4)
        self.assertEqual([r["tid"] for r in sample["thread_cpu"]], [4, 3])
        self.assertEqual(sample["thread_cpu"][0],
                         {"tid": 4, "user_time_s": 1.0, "system_time_s": 0.5})
        self.assertLessEqual(sample["thread_cpu_query_start_elapsed_s"],
                             sample["thread_cpu_query_end_elapsed_s"])

    def test_missing_and_invalid_thread_values_are_unavailable(self):
        proc = self.process()
        del proc.threads
        sample = _sample(proc, 0, sample_threads=True)
        self.assertEqual(sample["thread_cpu_reason"], "UNSUPPORTED")
        proc.threads = lambda: [SimpleNamespace(id=7, user_time=float("nan"), system_time=0)]
        sample = _sample(proc, 0, sample_threads=True)
        self.assertFalse(sample["thread_cpu_available"])
        self.assertEqual(sample["thread_cpu_reason"], "INVALID")
        self.assertIsNone(sample["threads_observed_count"])

    def test_thread_query_does_not_hide_programming_errors(self):
        proc = self.process()
        for error in (RuntimeError("bug"), ValueError("bug")):
            proc.threads.side_effect = error
            with self.assertRaises(type(error)):
                _sample(proc, 0, sample_threads=True)

    def test_process_creation_time_has_explicit_unavailable_status(self):
        proc = self.process()
        self.assertEqual(_creation_time(proc), (123456.25, "psutil.create_time"))
        self.assertEqual(_creation_time(None), (None, "UNAVAILABLE"))
        proc.create_time.side_effect = OSError()
        self.assertEqual(_creation_time(proc), (None, "OSError"))
        proc.create_time.side_effect = None
        proc.create_time.return_value = float("inf")
        self.assertEqual(_creation_time(proc), (None, "INVALID"))
        proc.create_time.side_effect = RuntimeError("bug")
        with self.assertRaises(RuntimeError):
            _creation_time(proc)

    def test_observer_error_reaps_only_launched_child(self):
        root = Path(__file__).resolve().parents[2] / "build/tests/process-sampler"
        root.mkdir(parents=True, exist_ok=True)
        child = mock.Mock(pid=123, returncode=None)
        child.poll.return_value = None
        proc = self.process()
        proc.create_time.side_effect = RuntimeError("observer bug")
        with tempfile.TemporaryDirectory(dir=root) as temporary, \
                mock.patch("tools.process_sampler.subprocess.Popen", return_value=child), \
                mock.patch("tools.process_sampler._process_reader", return_value=proc):
            with self.assertRaises(RuntimeError):
                run_process(["unused"], env={}, cwd=temporary, timeout=1, output_dir=temporary)
        child.kill.assert_called_once_with()
        child.wait.assert_called_once_with(timeout=sampler._CHILD_EXIT_GRACE_SECONDS)

    def test_invalid_options_reject_before_launch(self):
        with mock.patch("tools.process_sampler.subprocess.Popen") as launch:
            for options in ({"sample_threads": 1}, {"thread_limit": 0},
                            {"thread_limit": True}, {"thread_limit": 1025}):
                with self.assertRaises(ValueError):
                    run_process(["unused"], env={}, cwd=".", timeout=1,
                                output_dir="unused", **options)
            launch.assert_not_called()

    def test_real_thread_sampling_retains_pid_and_saved_output(self):
        try:
            import psutil
        except ImportError:
            self.skipTest("optional psutil unavailable")
        root = Path(__file__).resolve().parents[2] / "build/tests/process-sampler"
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as temporary:
            result = run_process([sys.executable, "-c",
                "import threading,time; "
                "t=threading.Thread(target=lambda: time.sleep(0.4)); "
                "t.start(); t.join()"], env=os.environ.copy(), cwd=temporary,
                timeout=5, output_dir=temporary, interval=0.02, sample_threads=True)
            self.assertEqual(result["returncode"], 0)
            self.assertIsInstance(result["process_identity"]["pid"], int)
            self.assertGreater(result["process_identity"]["creation_time_unix_s"], 0)
            samples = [s for s in result["samples"] if s["thread_cpu_available"]]
            self.assertTrue(samples)
            self.assertTrue(any(s["threads_observed_count"] >= 2 for s in samples))
            self.assertTrue((Path(temporary) / "samples.json").is_file())


# Typed DLL ports exercise the actual ctypes provider, without a Windows/GPU
# query. The real API prototypes, buffers, statuses and cleanup body are used.
import ctypes as ct
from tools import process_sampler as sampler
from tools import windows_gpu_observation as gpu


class _Function:
    def __init__(self, body): self.body=body
    def __call__(self,*args): return self.body(*args)


class _Dll:
    def __init__(self, rows=None):
        self.rows=rows if rows is not None else [("pid_42_luid_0x0_0x169c5_phys_0_eng_0_engtype_3D",0,0.0)]
        self.opens=self.adds=self.collects=self.closes=self.probes=self.fills=0
        self.open_status=self.add_status=self.collect_status=0
        self.mode="normal"
        for name,body in (("PdhOpenQueryW",self.open),("PdhAddEnglishCounterW",self.add),
            ("PdhCollectQueryData",self.collect),("PdhCloseQuery",self.close),
            ("PdhGetFormattedCounterArrayW",self.array)):
            setattr(self,name,_Function(body))
    @staticmethod
    def put(pointer,kind,value):ct.cast(pointer,ct.POINTER(kind))[0]=value
    def open(self,source,user,out):
        self.opens+=1;self.put(out,gpu.HANDLE,100);return self.open_status
    def add(self,query,path,user,out):
        self.adds+=1
        if path!=gpu.COUNTER_PATH:raise AssertionError("wrong English wildcard")
        self.put(out,gpu.HANDLE,101);return self.add_status
    def collect(self,query):self.collects+=1;return self.collect_status
    def close(self,query):self.closes+=1;return 0
    def array(self,counter,fmt,size,count,buffer):
        if fmt!=0x8200:raise AssertionError("raw uncapped DOUBLE format required")
        texts=[name.encode("utf-16-le")+b"\0\0" for name,_,_ in self.rows]
        needed=len(self.rows)*ct.sizeof(gpu._Item)+sum(map(len,texts))
        self.put(count,gpu.DWORD,len(self.rows))
        if buffer is None:
            self.probes+=1
            self.put(size,gpu.DWORD,gpu.BUFFER_LIMIT+1 if self.mode=="limit" else needed)
            return gpu.MORE_DATA if needed or self.mode=="limit" else 0
        self.fills+=1
        if self.mode=="raise":raise RuntimeError("provider programming error")
        if self.mode=="churn" or (self.mode=="growth" and self.fills==1):return gpu.MORE_DATA
        capacity=ct.cast(size,ct.POINTER(gpu.DWORD))[0]
        if capacity<needed:return gpu.MORE_DATA
        self.put(size,gpu.DWORD,needed)
        base=ct.addressof(buffer.contents);offset=len(self.rows)*ct.sizeof(gpu._Item)
        for index,((name,status,value),text) in enumerate(zip(self.rows,texts)):
            buffer[index].name=base+offset
            buffer[index].formatted.CStatus=status
            buffer[index].formatted.value.doubleValue=value
            ct.memmove(base+offset,text,len(text));offset+=len(text)
        if self.mode=="pointer":buffer[0].name=base+needed+2
        if self.mode=="count":self.put(count,gpu.DWORD,1000000)
        return 0


class _Clock:
    def __init__(self):self.value=0.0
    def __call__(self):return self.value


def _subjects():
    return [dict(pid=42,creation_time_unix_s=123.25,purpose="target",identity_available=True),
            dict(pid=77,creation_time_unix_s=456.5,purpose="explicit_peer",identity_available=True)]


class WindowsGpuTests(unittest.TestCase):
    def observer(self,rows=None):
        dll=_Dll(rows);clock=_Clock()
        return gpu.WindowsGpuObservation(dll=dll,platform="win32",clock=clock),dll,clock
    def test_zero_origin_priming_then_actual_zero_and_explicit_peer_rows(self):
        obs,dll,clock=self.observer([
            ("pid_42_luid_0x0_0x169c5_phys_0_eng_0_engtype_3D",0,0),
            ("pid_77_luid_0x1_0xabcdef_phys_1_eng_2_engtype_Copy",1,12.5),
            ("pid_999_luid_0x1_0xabcdef_phys_1_eng_2_engtype_Copy",0,95)])
        first=obs.sample(_subjects(),0,8);self.assertEqual(first["reason"],"PRIMING")
        self.assertFalse(first["available"]);self.assertEqual(dll.probes,0)
        clock.value=.5;self.assertEqual(obs.sample(_subjects(),0,8)["reason"],"WAITING_RATE_WINDOW")
        self.assertEqual(dll.collects,1)
        clock.value=1;result=obs.sample(_subjects(),0,8)
        self.assertTrue(result["available"]);self.assertTrue(result["query_data_complete"])
        self.assertEqual(result["rate_window_start_elapsed_s"],0)
        self.assertEqual(result["query_instance_count"],3)
        self.assertEqual([r["pid"] for r in result["rows"]],[42,77])
        self.assertTrue(result["rows"][0]["available"])
        self.assertEqual(result["rows"][0]["utilization_percent"],0)
        self.assertEqual(result["rows"][1]["adapter_luid"],"00000001:00ABCDEF")
        self.assertNotIn("gpu_total_percent",result);self.assertNotIn("fps",result)
        obs.close();obs.close();self.assertEqual(dll.closes,1)
        self.assertEqual(obs.sample(_subjects(),0,8)["reason"],"CLOSED")
        # A slow collect must not consume the required between-collection wait.
        obs,dll,clock=self.observer()
        collect=dll.PdhCollectQueryData.body
        def delayed_collect(query):
            value=collect(query);clock.value+=.25;return value
        dll.PdhCollectQueryData.body=delayed_collect
        self.assertEqual(obs.sample(_subjects(),0,8)["reason"],"PRIMING")
        clock.value=1.0
        self.assertEqual(obs.sample(_subjects(),0,8)["reason"],"WAITING_RATE_WINDOW")
        self.assertEqual(dll.collects,1)
        clock.value=1.25
        self.assertTrue(obs.sample(_subjects(),0,8)["available"])
        self.assertEqual(dll.collects,2);obs.close()
    def test_invalid_new_instance_status_nan_and_out_of_range_never_become_zero(self):
        cases = ((gpu.INVALID_DATA, 0, "PDH_STATUS", None),
                 (0, float("nan"), "NONFINITE", None),
                 (0, -1, "OUT_OF_RANGE", -1), (0, 101, "OUT_OF_RANGE", 101))
        for status, value, issue, observed in cases:
            obs,dll,clock=self.observer([("pid_42_luid_0x0_0x1_phys_0_eng_0_engtype_3D",status,value)])
            obs.sample(_subjects(),0,8);clock.value=1
            result=obs.sample(_subjects(),0,8)
            self.assertFalse(result["available"]);self.assertIsNone(result["rows"][0]["utilization_percent"])
            self.assertEqual(result["rows"][0]["reason"],"INVALID_COUNTER_DATA")
            self.assertEqual(result["rows"][0]["invalid_counter_detail"],
                             dict(issue=issue, formatted_value=observed))
            obs.close();self.assertEqual(dll.closes,1)
    def test_no_instance_unknown_identity_and_cross_platform_are_not_measured_zero(self):
        obs,dll,clock=self.observer([]);obs.sample(_subjects(),0,8);clock.value=1
        result=obs.sample(_subjects(),0,8)
        self.assertFalse(result["available"]);self.assertEqual(result["reason"],"NO_MATCHING_INSTANCE")
        self.assertTrue(result["query_data_complete"]);obs.close()
        obs,dll,clock=self.observer();subjects=_subjects()
        for subject in subjects:subject["identity_available"]=False
        self.assertEqual(obs.sample(subjects,0,8)["reason"],"IDENTITY_UNAVAILABLE")
        self.assertEqual(dll.opens,0)
        obs=gpu.WindowsGpuObservation(dll=dll,platform="linux",clock=clock)
        self.assertEqual(obs.sample(_subjects(),0,8)["reason"],"UNSUPPORTED_PLATFORM")
        self.assertEqual(dll.opens,0)
    def test_denied_unsupported_and_collection_failure_close_and_reprime(self):
        for status,reason in ((5,"DENIED"),(0xc0000bb8,"UNSUPPORTED_COUNTER")):
            obs,dll,clock=self.observer();dll.add_status=status
            self.assertEqual(obs.sample(_subjects(),0,8)["reason"],reason)
            obs.close();self.assertEqual(dll.closes,1)
        obs,dll,clock=self.observer();obs.sample(_subjects(),0,8)
        clock.value=1;dll.collect_status=gpu.NO_DATA
        self.assertEqual(obs.sample(_subjects(),0,8)["reason"],"NO_DATA")
        dll.collect_status=0;clock.value=2
        self.assertEqual(obs.sample(_subjects(),0,8)["reason"],"PRIMING")
        obs.close();self.assertEqual(dll.closes,1)
    def test_array_growth_reprobes_and_all_bounds_reject_without_partial_acceptance(self):
        for mode in ("growth","churn","limit","pointer","count"):
            obs,dll,clock=self.observer();dll.mode=mode;obs.sample(_subjects(),0,8);clock.value=1
            result=obs.sample(_subjects(),0,8)
            if mode=="growth":
                self.assertTrue(result["available"]);self.assertEqual(dll.probes,2);self.assertEqual(dll.fills,2)
            else:self.assertFalse(result["available"]);self.assertFalse(result["query_data_complete"])
            obs.close();self.assertEqual(dll.closes,1)
    def test_output_limit_counts_all_matching_rows_without_summing_or_os_query_cap(self):
        rows=[(f"pid_42_luid_0x0_0x1_phys_0_eng_{i}_engtype_3D",0,i) for i in range(5)]
        obs,dll,clock=self.observer(rows);obs.sample(_subjects(),0,2);clock.value=1
        result=obs.sample(_subjects(),0,2)
        self.assertEqual(len(result["rows"]),2);self.assertEqual(result["observed_matching_rows"],5)
        self.assertEqual(result["query_instance_count"],5);self.assertTrue(result["output_truncated"])
        self.assertEqual(result["reason"],"OUTPUT_TRUNCATED");obs.close()
    def test_empty_os_engine_label_preserves_counter_identity_and_old_guard_control(self):
        def check(module):
            prefix="pid_42_luid_0x00000000_0x000169C5_phys_0_eng_"
            rows=[(prefix+"11_engtype_",0,0),
                  (prefix+"12_engtype_",gpu.INVALID_DATA,80),
                  (prefix+"5_engtype_3D",1,12.5),
                  (prefix+"9",0,0),
                  ("pid_42_luid_0x0_0x169C5_eng_9_engtype_",0,0),
                  ("pid_999_luid_0x0_0x169C5_phys_0_eng_11_engtype_",0,99),
                  ("pid_77_luid_0x0_0x169C5_phys_0_eng_11_engtype_",0,40)]
            dll=_Dll(rows);clock=_Clock();subjects=_subjects()
            subjects[1]["identity_available"]=False
            obs=module.WindowsGpuObservation(dll=dll,platform="win32",clock=clock)
            try:
                obs.sample(subjects,0,8);clock.value=1
                result=obs.sample(subjects,0,8)
                self.assertEqual(len(result["rows"]),3)
                self.assertEqual(result["unparsed_instance_count"],2)
                self.assertEqual(result["invalid_matching_rows"],3)
                empty,invalid,named=result["rows"]
                self.assertEqual((empty["pid"],empty["adapter_luid"],empty["physical_gpu_index"],empty["engine_index"]),
                                 (42,"00000000:000169C5",0,11))
                self.assertIsNone(empty["engine_type"])
                self.assertIs(empty["engine_type_available"],False)
                self.assertEqual(empty["engine_type_reason"],"EMPTY_OS_LABEL")
                self.assertTrue(empty["available"]);self.assertEqual(empty["utilization_percent"],0)
                self.assertIsNone(invalid["engine_type"]);self.assertFalse(invalid["available"])
                self.assertIsNone(invalid["utilization_percent"])
                self.assertEqual(invalid["cstatus"],"0xC0000BBA")
                self.assertEqual(named["engine_type"],"3D");self.assertTrue(named["engine_type_available"])
                self.assertEqual(named["engine_type_reason"],"AVAILABLE")
                self.assertEqual(named["utilization_percent"],12.5)
                self.assertTrue(all(r["pid"]==42 for r in result["rows"]))
            finally:obs.close()
            self.assertEqual(dll.closes,1)
        check(gpu)
        # Revert only the complete owner's positive-label requirement. The
        # unchanged actual row-count assertion must fail ordinarily.
        from types import ModuleType
        source=Path(gpu.__file__).read_text(encoding="utf-8")
        needle="((?:[A-Za-z0-9][A-Za-z0-9_ ]{0,63})?)"
        self.assertEqual(source.count(needle),1)
        old=ModuleType("old_positive_engine_type_owner")
        exec(compile(source.replace(needle,"([A-Za-z0-9][A-Za-z0-9_ ]{0,63})"),gpu.__file__,"exec"),old.__dict__)
        with self.assertRaises(AssertionError):check(old)

    def test_unparsed_selected_names_retain_bounded_witnesses_and_omitted_field_control(self):
        long_name="pid_42_"+"x"*300
        def check(module):
            rows=[(long_name,gpu.INVALID_DATA,0)]+[(f"pid_42_unsupported_{i}",0,0) for i in range(8)]
            rows += [("pid_77_unsupported",0,0),("pid_999_unsupported",0,0),
                     ("pid_42_luid_0x0_0x1_phys_0_eng_0_engtype_3D",0,0)]
            dll=_Dll(rows);clock=_Clock()
            obs=module.WindowsGpuObservation(dll=dll,platform="win32",clock=clock)
            subjects=_subjects();subjects[1]["identity_available"]=False
            try:
                obs.sample(subjects,0,8);clock.value=1
                result=obs.sample(subjects,0,8)
                self.assertTrue(result["available"]);self.assertEqual(len(result["rows"]),1)
                self.assertEqual(result["invalid_matching_rows"],9)
                self.assertEqual(result["unparsed_instance_count"],9)
                self.assertEqual(result["unparsed_instance_unretained_count"],1)
                witnesses=result["unparsed_instance_witnesses"]
                self.assertEqual(len(witnesses),8)
                self.assertTrue(all(w["pid"]==42 and w["identity_available"] for w in witnesses))
                self.assertTrue(all(w["reason"]=="UNPARSED_NAME" for w in witnesses))
                self.assertEqual(witnesses[0].get("instance_name"),long_name[:256])
                self.assertIs(witnesses[0]["name_truncated"],True)
                self.assertEqual(witnesses[0]["cstatus"],"0xC0000BBA")
                self.assertEqual(witnesses[0]["creation_time_unix_s"],123.25)
                self.assertIs(witnesses[1]["name_truncated"],False)
            finally:obs.close()
            self.assertEqual(dll.closes,1)
        check(gpu)
        # Complete production module, one omitted witness field; unchanged
        # ordinary equality assertion must fail, without a crash or query leak.
        from types import ModuleType
        source=Path(gpu.__file__).read_text(encoding="utf-8")
        needle="instance_name=name[:256], name_truncated=len(name)>256,"
        self.assertEqual(source.count(needle),1)
        old=ModuleType("old_unparsed_witness_owner")
        exec(compile(source.replace(needle,"name_truncated=len(name)>256,"),gpu.__file__,"exec"),old.__dict__)
        with self.assertRaises(AssertionError):check(old)

    def test_programming_errors_propagate_and_owned_query_is_closed(self):
        obs,dll,clock=self.observer();obs.sample(_subjects(),0,8);clock.value=1;dll.mode="raise"
        with self.assertRaises(RuntimeError):obs.sample(_subjects(),0,8)
        obs.close();self.assertEqual(dll.closes,1)
        obs,dll,clock=self.observer();dll.PdhAddEnglishCounterW.body=lambda *args:None
        with self.assertRaises(TypeError):obs.sample(_subjects(),0,8)
        self.assertEqual(dll.closes,1)


class GpuSamplerIntegrationTests(unittest.TestCase):
    def scratch(self):
        root=Path(__file__).resolve().parents[2]/"build/tests/process-sampler"
        root.mkdir(parents=True,exist_ok=True)
        return tempfile.TemporaryDirectory(dir=root)
    def test_default_off_never_constructs_provider_and_invalid_options_reject_prelaunch(self):
        child=mock.Mock(pid=42,returncode=0);child.poll.return_value=0
        with self.scratch() as folder,mock.patch.object(sampler.subprocess,"Popen",return_value=child),\
            mock.patch.object(sampler,"_process_reader",return_value=None),mock.patch.object(sampler,"_gpu_provider") as factory:
            result=sampler.run_process(["unused"],env={},cwd=folder,output_dir=folder,timeout=1)
            factory.assert_not_called();self.assertNotIn("gpu_samples_path",result)
        with mock.patch.object(sampler.subprocess,"Popen") as launch:
            for opts in ({"sample_gpu":1},{"gpu_limit":0},{"gpu_limit":True},{"gpu_peer_pids":[7]},
                         {"sample_gpu":True,"gpu_peer_pids":[7,7]}):
                with self.assertRaises(ValueError):sampler.run_process(["unused"],env={},cwd=".",output_dir="unused",timeout=1,**opts)
            launch.assert_not_called()
    def test_real_sampler_wire_retains_explicit_subjects_and_closes_after_normal_exit(self):
        child=mock.Mock(pid=42,returncode=0);child.poll.side_effect=[None,None,0,0]
        readers={pid:SimpleNamespace(pid=pid,create_time=lambda birth=birth:birth) for pid,birth in ((42,123.25),(77,456.5))}
        dll=_Dll();provider=gpu.WindowsGpuObservation(dll=dll,platform="win32")
        sample=dict(elapsed_s=.1,rss_bytes=100,commit_bytes=200,cpu_time_s=.01)
        with self.scratch() as folder,mock.patch.object(sampler.subprocess,"Popen",return_value=child),\
            mock.patch.object(sampler,"_process_reader",side_effect=lambda pid:readers[pid]),\
            mock.patch.object(sampler,"_sample",return_value=sample),\
            mock.patch.object(sampler,"_gpu_provider",return_value=provider),mock.patch.object(sampler.time,"sleep"):
            result=sampler.run_process(["unused"],env={},cwd=folder,output_dir=folder,timeout=1,
                                      sample_gpu=True,gpu_peer_pids=[77],gpu_limit=2)
            data=json.loads(Path(result["gpu_samples_path"]).read_text())
            self.assertTrue(data);self.assertEqual(data[0]["reason"],"PRIMING")
            self.assertEqual([s["pid"] for s in data[0]["subjects"]],[42,77])
            self.assertEqual(result["gpu_peer_pids"],[77]);self.assertEqual(result["gpu_close_status"],0)
            self.assertEqual(result["process_identity"]["pid"],42)
        self.assertEqual(dll.closes,1);child.kill.assert_not_called()
    def test_fresh_reader_birth_change_refuses_peer_rows_without_rebinding(self):
        original=SimpleNamespace(pid=77,create_time=lambda:123.25)
        current=SimpleNamespace(pid=77,create_time=lambda:456.5)
        provider=mock.Mock()
        provider.sample.return_value=dict(rows=[dict(pid=77,available=True,utilization_percent=50)],available=True)
        with mock.patch.object(sampler,"_process_reader",return_value=current),\
             mock.patch.object(sampler,"_gpu_provider",return_value=provider):
            owner=sampler._GpuSampler(77,original,123.25,())
            data=owner.sample(0,8)
            self.assertFalse(data["subjects_after_query"][0]["identity_available"])
            self.assertFalse(data["available"]);self.assertIsNone(data["rows"][0]["utilization_percent"])
            self.assertEqual(data["reason"],"IDENTITY_CHANGED")
        provider.sample.return_value=dict(rows=[],available=False,unparsed_instance_witnesses=[
            dict(pid=77,creation_time_unix_s=123.25,instance_name="pid_77_unsupported",
                 reason="UNPARSED_NAME",identity_available=True,identity_reason="AVAILABLE")])
        # Before-query identity is valid; the same PID has a different birth
        # after-query. Retain a historical witness without attributing it alive.
        with mock.patch.object(sampler,"_process_reader",side_effect=[original,current]),\
             mock.patch.object(sampler,"_gpu_provider",return_value=provider):
            data=sampler._GpuSampler(77,original,123.25,()).sample(0,8)
            self.assertTrue(provider.sample.call_args.args[0][0]["identity_available"])
            witness=data["unparsed_instance_witnesses"][0]
            self.assertFalse(witness["identity_available"])
            self.assertEqual(witness["identity_reason"],"IDENTITY_CHANGED")
            self.assertEqual(witness["creation_time_unix_s"],123.25)
            self.assertEqual(witness["reason"],"UNPARSED_NAME")
        dll=_Dll([("pid_77_luid_0x0_0x169C5_phys_0_eng_11_engtype_",0,40)])
        clock=_Clock();provider=gpu.WindowsGpuObservation(dll=dll,platform="win32",clock=clock)
        with mock.patch.object(sampler,"_process_reader",side_effect=[original,original,original,current]),\
             mock.patch.object(sampler,"_gpu_provider",return_value=provider):
            owner=sampler._GpuSampler(77,original,123.25,())
            try:
                self.assertEqual(owner.sample(0,8)["reason"],"PRIMING")
                clock.value=1;data=owner.sample(0,8)
                self.assertTrue(data["subjects"][0]["identity_available"])
                self.assertEqual(len(data["rows"]),1)
                self.assertIsNone(data["rows"][0]["engine_type"])
                self.assertEqual(data["rows"][0]["engine_type_reason"],"EMPTY_OS_LABEL")
                self.assertFalse(data["rows"][0]["available"])
                self.assertIsNone(data["rows"][0]["utilization_percent"])
                self.assertEqual(data["rows"][0]["reason"],"IDENTITY_CHANGED")
            finally:owner.close()
        self.assertEqual(dll.closes,1)
    def test_gpu_observer_error_preserves_target_only_kill_reap_and_close(self):
        child=mock.Mock(pid=42,returncode=None);child.poll.return_value=None
        reader=SimpleNamespace(pid=42,create_time=lambda:123.25)
        provider=mock.Mock();provider.sample.side_effect=RuntimeError("bug")
        sample=dict(elapsed_s=.1,rss_bytes=100,commit_bytes=200,cpu_time_s=.01)
        with self.scratch() as folder,mock.patch.object(sampler.subprocess,"Popen",return_value=child),\
            mock.patch.object(sampler,"_process_reader",return_value=reader),\
            mock.patch.object(sampler,"_sample",return_value=sample),\
            mock.patch.object(sampler,"_gpu_provider",return_value=provider):
            with self.assertRaises(RuntimeError):sampler.run_process(["unused"],env={},cwd=folder,
                output_dir=folder,timeout=1,sample_gpu=True)
        child.kill.assert_called_once_with();child.wait.assert_called_once_with(timeout=sampler._CHILD_EXIT_GRACE_SECONDS);provider.close.assert_called_once_with()


class PerformanceClockTests(unittest.TestCase):
    def scratch(self):
        root = Path(__file__).resolve().parents[2] / "build/tests/process-sampler"
        root.mkdir(parents=True, exist_ok=True)
        return tempfile.TemporaryDirectory(dir=root)

    def test_wide_origin_and_provider_metadata_precede_launch_and_survive_json(self):
        origin = (1 << 53) + 123
        info = SimpleNamespace(implementation="sentinel perf", monotonic=True,
                               adjustable=False, resolution=1e-7)
        child = mock.Mock(pid=42, returncode=0)
        child.poll.return_value = 0
        order = []
        def clock_info(name):
            order.append("info")
            self.assertEqual(name, "perf_counter")
            return info
        def origin_ns():
            order.append("origin")
            return origin
        def launch(*args, **kwargs):
            order.append("launch")
            return child
        with self.scratch() as folder, \
                mock.patch.object(sampler.time, "get_clock_info", side_effect=clock_info) as metadata, \
                mock.patch.object(sampler.time, "perf_counter_ns", side_effect=origin_ns), \
                mock.patch.object(sampler.time, "perf_counter", return_value=origin / 1e9 + 0.25), \
                mock.patch.object(sampler.time, "monotonic", return_value=9000), \
                mock.patch.object(sampler.subprocess, "Popen", side_effect=launch), \
                mock.patch.object(sampler, "_process_reader", return_value=None):
            result = sampler.run_process(["unused"], env={}, cwd=folder,
                                         output_dir=folder, timeout=5)
            self.assertIn("clock", result)
            self.assertEqual(order, ["info", "origin", "launch"])
            metadata.assert_called_once_with("perf_counter")
            clock = result["clock"]
            self.assertEqual(clock["origin_perf_counter_ns"], str(origin))
            self.assertEqual(clock["implementation"], "sentinel perf")
            self.assertEqual(clock["resolution_s"], 1e-7)
            self.assertEqual(json.loads((Path(folder) / "result.json").read_text())["clock"], clock)
            self.assertAlmostEqual(result["elapsed_s"], 0.25)
            child.kill.assert_not_called()

    def test_aggregate_query_has_exact_ns_bracket_instead_of_frame_timestamp(self):
        origin = (1 << 53) + 123
        calls = []
        def ns():
            calls.append("clock")
            return origin + (1000 if len(calls) == 1 else 3000)
        proc = SimpleNamespace(
            memory_info=lambda: (calls.append("memory") or SimpleNamespace(rss=100, private=200)),
            cpu_times=lambda: SimpleNamespace(user=1, system=0),
            io_counters=lambda: SimpleNamespace(read_bytes=1, write_bytes=2))
        with mock.patch.object(sampler.time, "perf_counter_ns", side_effect=ns), \
                mock.patch.object(sampler.time, "monotonic", return_value=9000):
            sample = sampler._sample(proc, origin / 1e9)
        self.assertIn("query_start_perf_counter_ns", sample)
        self.assertEqual(calls, ["clock", "memory", "clock"])
        self.assertEqual(sample["query_start_perf_counter_ns"], str(origin + 1000))
        self.assertEqual(sample["query_end_perf_counter_ns"], str(origin + 3000))
        self.assertEqual(json.loads(json.dumps(sample))["query_end_perf_counter_ns"], str(origin + 3000))
        self.assertEqual(sample["rss_bytes"], 100)

    def test_thread_query_uses_performance_epoch(self):
        with mock.patch.object(sampler.time, "perf_counter", return_value=1000.25), \
                mock.patch.object(sampler.time, "monotonic", return_value=9000):
            sample = sampler._thread_sample(SimpleNamespace(), 1000, 8)
        self.assertEqual(sample["thread_cpu_query_start_elapsed_s"], 0.25)
        self.assertEqual(sample["thread_cpu_query_end_elapsed_s"], 0.25)

    def test_live_file_visibility_uses_performance_epoch(self):
        child = mock.Mock(pid=42, returncode=0)
        child.poll.side_effect = [None, None, None, 0, 0]
        with self.scratch() as folder, \
                mock.patch.object(sampler.time, "perf_counter_ns", return_value=1000_000_000_000), \
                mock.patch.object(sampler.time, "perf_counter", return_value=1000.25), \
                mock.patch.object(sampler.time, "monotonic", return_value=9000), \
                mock.patch.object(sampler.time, "sleep"), \
                mock.patch.object(sampler.subprocess, "Popen", return_value=child), \
                mock.patch.object(sampler, "_process_reader", return_value=None), \
                mock.patch.object(sampler, "_has_frame_marker", return_value=True), \
                mock.patch.object(sampler, "_has_live_marker", return_value=True):
            result = sampler.run_process(["unused"], env={}, cwd=folder,
                output_dir=folder, timeout=5, marker_path=Path(folder) / "frames",
                live_markers={"ready": (Path(folder) / "ready", b"ready")})
        self.assertEqual(result["first_frame_s"], 0.25)
        self.assertEqual(result["live_markers_s"]["ready"], 0.25)
        child.kill.assert_not_called()

    def test_stuck_child_does_not_turn_watchdog_into_unbounded_wait(self):
        def wait(*, timeout=None):
            if timeout is None:
                return None
            self.assertGreater(timeout, 0)
            self.assertLessEqual(timeout, 5)
            raise sampler.subprocess.TimeoutExpired(["unused"], timeout)

        child = mock.Mock(pid=42, returncode=None)
        child.poll.return_value = None
        child.wait.side_effect = wait
        with self.scratch() as folder, \
                mock.patch.object(sampler.time, "perf_counter_ns", return_value=1000_000_000_000), \
                mock.patch.object(sampler.time, "perf_counter", return_value=1002), \
                mock.patch.object(sampler.subprocess, "Popen", return_value=child), \
                mock.patch.object(sampler, "_process_reader", return_value=None):
            with self.assertRaises(sampler.subprocess.TimeoutExpired):
                sampler.run_process(["unused"], env={}, cwd=folder,
                                    output_dir=folder, timeout=1)
            self.assertFalse((Path(folder) / "result.json").exists())
        child.kill.assert_called_once_with()
        child.wait.assert_called_once()

    def test_timeout_uses_performance_clock_when_monotonic_is_stalled(self):
        state = dict(polls=0, killed=False)
        def poll():
            state["polls"] += 1
            return 0 if state["killed"] or state["polls"] > 1 else None
        child = mock.Mock(pid=42, returncode=0)
        child.poll.side_effect = poll
        child.kill.side_effect = lambda: state.update(killed=True)
        with self.scratch() as folder, \
                mock.patch.object(sampler.time, "perf_counter_ns", return_value=1000_000_000_000), \
                mock.patch.object(sampler.time, "perf_counter", return_value=1002), \
                mock.patch.object(sampler.time, "monotonic", return_value=9000), \
                mock.patch.object(sampler.time, "sleep"), \
                mock.patch.object(sampler.subprocess, "Popen", return_value=child), \
                mock.patch.object(sampler, "_process_reader", return_value=None):
            result = sampler.run_process(["unused"], env={}, cwd=folder,
                                         output_dir=folder, timeout=1)
        self.assertTrue(result["timed_out"])
        child.kill.assert_called_once_with()
        self.assertGreaterEqual(child.wait.call_count, 1)

    def test_actual_gpu_factory_shares_origin_and_rate_window_epoch(self):
        perf, monotonic, dll = _Clock(), _Clock(), _Dll()
        perf.value, monotonic.value = 1000.25, 9000
        actual_type = gpu.WindowsGpuObservation
        def provider(**kwargs):
            # Typed DLL/clock ports retain the real provider collection body.
            # Its fallback is deliberately a different epoch from perf_counter.
            return actual_type(dll=dll, platform="win32", **({"clock": monotonic} | kwargs))
        with mock.patch.object(sampler.time, "perf_counter", perf), \
                mock.patch.object(sampler.time, "monotonic", monotonic), \
                mock.patch.object(gpu, "WindowsGpuObservation", side_effect=provider):
            owner = sampler._gpu_provider()
            try:
                first = owner.sample(_subjects(), 1000, 8)
                self.assertEqual(first["query_start_elapsed_s"], 0.25)
                self.assertEqual(first["query_end_elapsed_s"], 0.25)
                perf.value = 1001.5
                second = owner.sample(_subjects(), 1000, 8)
                self.assertEqual(second["reason"], "AVAILABLE")
                self.assertEqual(second["rate_window_start_elapsed_s"], 0.25)
                self.assertEqual(second["query_end_elapsed_s"], 1.5)
            finally:
                owner.close()
        self.assertEqual(dll.closes, 1)

    def test_gpu_wrapper_duration_uses_same_performance_provider(self):
        clock = _Clock()
        clock.value = 1000
        reader = SimpleNamespace(pid=42, create_time=lambda: 123.25)
        provider = mock.Mock()
        def observe(subjects, origin, limit):
            clock.value = 1000.25
            return dict(rows=[], available=False)
        provider.sample.side_effect = observe
        with mock.patch.object(sampler.time, "perf_counter", clock), \
                mock.patch.object(sampler.time, "monotonic", return_value=9000), \
                mock.patch.object(sampler, "_process_reader", return_value=reader), \
                mock.patch.object(sampler, "_gpu_provider", return_value=provider):
            owner = sampler._GpuSampler(42, reader, 123.25, ())
            try:
                result = owner.sample(1000, 8)
                self.assertEqual(result["observation_query_elapsed_s"], 0.25)
            finally:
                owner.close()

    def test_real_timeout_preserves_owned_child_retirement(self):
        with self.scratch() as folder:
            result = sampler.run_process([sys.executable, "-c", "import time; time.sleep(30)"],
                env=os.environ.copy(), cwd=folder, output_dir=folder, timeout=0.15, interval=0.02)
        self.assertTrue(result["timed_out"])
        self.assertNotEqual(result["returncode"], 0)
        self.assertLess(result["elapsed_s"], 5)


if __name__ == "__main__":
    unittest.main()
