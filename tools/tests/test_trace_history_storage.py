"""Synthetic CPU-only exact-history controls; no application or raw capture."""
import copy
import hashlib
import json
import math
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest import mock

from tools import lifecycle_trace_report as trace
from tools import terminal_ack_observation as ack
from tools import trace_history_storage as storage
from tools.tests.test_lifecycle_trace_report import capture
from tools.tests.test_terminal_ack_observation import trace_document, packet, encoded, TOKEN


class ExactHistoryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jce-exact-history-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.path = self.root / "trace.json"
        self.serial = 0

    def options(self, **changes):
        self.serial += 1
        values = dict(history_db_path=self.root / (str(self.serial) + ".sqlite"),
                      disk_history_max_records=10000, disk_history_max_bytes=2 * 1024 * 1024)
        values.update(changes)
        return values

    def file(self, document, *, required=("app.init",), **options):
        raw = json.dumps(document)
        if not self.path.exists() or self.path.read_text(encoding="utf-8") != raw:
            self.path.write_text(raw, encoding="utf-8")
        return trace.analyze_trace_file(self.path, required, **options)

    def assert_fingerprint_error(self, document, pattern, **options):
        with self.assertRaisesRegex(trace.TraceFileError, pattern) as caught:
            self.file(document, **options)
        evidence = caught.exception.evidence
        self.assertEqual(evidence["sha256"], hashlib.sha256(self.path.read_bytes()).hexdigest())
        self.assertEqual(evidence["sha256"], evidence["source_sha256_after"])
        self.assertEqual(evidence["input_bytes"], self.path.stat().st_size)
        self.assertTrue(evidence["source_fingerprint_stable"])
        return evidence

    def test_default_and_explicit_disk_reports_are_semantically_equal(self):
        memory = self.file(capture())
        self.assertNotIn("history_storage", memory)
        disk = self.file(capture(), **self.options())
        info = disk.pop("history_storage")
        self.assertEqual(memory, disk)
        self.assertTrue(info["closed"])
        self.assertFalse(info["failed"])
        self.assertEqual((info["task_records"], info["completed_services"]), (1, 1))
        self.assertLessEqual(info["database_bytes"], info["max_bytes"])

    def test_disk_budget_does_not_fan_out_to_other_memory_limits(self):
        doc = capture()
        for task in (10, 11):
            doc["traceEvents"].append(dict(ph="i", cat="task", pid=1, tid=1, ts=3000,
                name="work", args=dict(task=task, parent=1, kind=0)))
        doc["jceTrace"].update(recorded=5, captured=5)
        self.assert_fingerprint_error(doc, "historical task identity limit", max_history=1)
        self.assertEqual(self.file(doc, max_history=1, **self.options())["historical_task_count"], 3)
        doc["traceEvents"][-1]["tid"] = 2
        self.assert_fingerprint_error(doc, "retained thread clock", max_history=1, **self.options())

    def test_exact_wide_identities_and_scalar_timestamp_types(self):
        h = storage.SQLiteHistory(self.root / "values.sqlite", 100, 1024 * 1024)
        try:
            keys = [(1 << 53) + 1, (1 << 63) + 1, (1 << 64) - 1, 1 << 100]
            stamps = [1 << 100, 2.5, -0.0, 42]
            for key, stamp in zip(keys, stamps):
                h.tasks[key] = (2, "service", stamp, keys[-1])
                h.completed_services.add(key)
            rows = list(h.tasks.items())
            self.assertEqual([k for k, _ in rows], keys)
            for key, expected in zip(keys, stamps):
                got = h.tasks.get(key)
                self.assertEqual(type(got[2]), type(expected))
                self.assertEqual(got[2], expected)
                self.assertEqual(got[3], keys[-1])
                self.assertIn(key, h.completed_services)
            self.assertEqual(math.copysign(1, h.tasks.get(keys[2])[2]), -1)
            self.assertEqual(len(h.tasks), 4)
            self.assertNotIsInstance(h.tasks, dict)
            self.assertEqual(set(vars(h.tasks)), {"owner"})
        finally:
            h.close()

    def test_late_duplicate_task_and_completion_beyond_batches(self):
        for completion in (False, True):
            doc = capture()
            for key in range(2, 40):
                doc["traceEvents"].append(dict(ph="i", cat="task", pid=1, tid=1, ts=3000,
                    name="work", args=dict(task=key, parent=1, kind=0)))
            if completion:
                doc["traceEvents"].extend([
                    dict(ph="B", cat="task", pid=1, tid=1, ts=4000, name="app.init", args=dict(id=1,parent=1)),
                    dict(ph="E", cat="task", pid=1, tid=1, ts=5000, name="app.init", args=dict(id=1,parent=1,ns=1,state=0))])
            else:
                doc["traceEvents"].append(copy.deepcopy(doc["traceEvents"][0]));doc["traceEvents"][-1]["ts"] = 4000
            doc["jceTrace"].update(recorded=len(doc["traceEvents"]),captured=len(doc["traceEvents"]))
            with mock.patch.object(storage, "BATCH_WRITES", 3):
                self.assert_fingerprint_error(doc, "completed more than once" if completion else "repeated submitted", **self.options())

    def test_full_observer_finish_scans_and_existing_packet_consumers(self):
        from tools.tests import test_filesystem_read_observation as fs
        from tools.tests import test_shader_provenance_observation as shader
        from tools.tests import test_offscreen_configure_observation as offscreen
        from tools.tests import test_resource_trace_observation as resource
        from tools.tests import test_renderer_submission_observation as submit
        from tools.tests import test_gpu_view_observation as gpu
        cases = [(fs.capture(fs.sample()), ["app.init"], dict(filesystem_read_details=True)),
                 (shader.success(), ["app.init"], dict(shader_provenance=True)),
                 (offscreen.capture(offscreen.sample()), ["app.init"], dict(offscreen_configurations=True)),
                 (resource.capture(resource.sample()), [resource.resource.SERVICE], dict(resource_observations=True)),
                 (submit.capture(submit.sample()), [submit.submission.SERVICE], dict(renderer_submissions=True)),
                 (gpu.capture(gpu.sample()), [gpu.gpu.SERVICE], dict(gpu_view_observations=True))]
        for document, required, flags in cases:
            with self.subTest(flags=flags):
                memory = self.file(document, required=required, **flags)
                disk = self.file(document, required=required, **flags, **self.options())
                disk.pop("history_storage")
                self.assertEqual(memory, disk)
        bad = fs.capture(fs.sample())
        bad["traceEvents"].append(dict(ph="i", cat="task",pid=1,tid=7,ts=100000,name="fs.host.read_all",args=dict(task=88,parent=0,kind=2)))
        bad["jceTrace"].update(recorded=len(bad["traceEvents"]),captured=len(bad["traceEvents"]))
        self.assert_fingerprint_error(bad,"no completed submitted pair",filesystem_read_details=True, **self.options())

    def test_shared_task_session_budget_and_completion_no_extra_row(self):
        h = storage.SQLiteHistory(self.root / "budget.sqlite", 2, 1024 * 1024)
        try:
            h.tasks[1] = (2,"x",0,0)
            h.completed_services.add(1)
            h.terminal_sessions.add((1 << 64) - 1)
            self.assertEqual(h.evidence()["records"], 2)
            with self.assertRaisesRegex(storage.HistoryStorageError,"record limit"):
                h.terminal_sessions.add(2)
        finally:
            h.close()
        self.assertTrue(h.evidence()["failed"])

    def test_late_terminal_session_duplicate_and_ack_disk_route(self):
        document = trace_document()
        self.path.write_text(json.dumps(document),encoding="utf-8")
        stderr = self.root / "stderr.log"
        stderr.write_bytes(encoded(packet(trace_status="complete",trace_published=True,trace_write_ok=True,cleanup_scope="10",log_file_close="api_ok")))
        process=dict(timed_out=False,returncode=0,stderr_path=str(stderr),process_identity=dict(pid=123,creation_time_unix_s=None,creation_time_source="unavailable"))
        options=self.options()
        result=ack.observe_terminal_ack(process,TOKEN,expected_stderr_path=stderr,trace_path=self.path,return_trace_report=True,
            trace_history_db_path=options["history_db_path"],trace_disk_history_max_records=options["disk_history_max_records"],trace_disk_history_max_bytes=options["disk_history_max_bytes"])
        self.assertTrue(result["trace"]["history_storage"]["closed"])
        self.assertTrue(result["trace_report"]["history_storage"]["closed"])
        self.assertEqual(result["trace_report"]["history_storage"]["terminal_session_records"],1)
        self.assertEqual(result["trace_report"]["cleanup_ownership"]["terminal_sessions"]["observed_count"],1)
        later=copy.deepcopy(document["traceEvents"][3:])
        for event in later:
            event["ts"]+=100
            if event["ph"]=="i":event["args"]["task"]=11
            else:event["args"].update(id=11,parent=11)
        document["traceEvents"]+=later;document["jceTrace"].update(recorded=len(document["traceEvents"]),captured=len(document["traceEvents"]))
        self.assert_fingerprint_error(document,"uniquely belong",required=["engine.log_quiesce"],cleanup_ownership=True,terminal_session_binding=True,**self.options())

    def test_byte_limit_and_existing_path_ownership(self):
        for suffix in ("", "-journal", "-wal", "-shm"):
            path=self.root/("existing"+str(len(suffix))+".sqlite");sentinel=Path(str(path)+suffix);sentinel.write_bytes(b"keep")
            with self.assertRaisesRegex(storage.HistoryStorageError,"already exists"):
                storage.SQLiteHistory(path,100,1024*1024)
            self.assertEqual(sentinel.read_bytes(),b"keep")
        self.assert_fingerprint_error(capture(),"byte budget",**self.options(disk_history_max_bytes=1))
        options=self.options(disk_history_max_bytes=5*4096)
        h=storage.SQLiteHistory(options["history_db_path"],10000,options["disk_history_max_bytes"])
        try:
            with self.assertRaises(storage.HistoryStorageError):
                for key in range(10000):h.tasks[key]=(0,"x"*1000,1,0)
        finally:h.close()
        self.assertLessEqual(options["history_db_path"].stat().st_size,options["disk_history_max_bytes"])

    def test_argument_admission_and_memory_cap_contract(self):
        for options in (dict(history_db_path=self.root/"x"),dict(disk_history_max_records=2),dict(history_db_path="relative",disk_history_max_records=2,disk_history_max_bytes=40960),dict(history_db_path=self.root/"x",disk_history_max_records=True,disk_history_max_bytes=40960)):
            with self.assertRaises(ValueError):self.file(capture(),**options)
        with self.assertRaisesRegex(ValueError,"requested terminal trace"):
            ack.observe_terminal_ack({},TOKEN,expected_stderr_path=self.root/"missing",trace_history_db_path=self.root/"x",trace_disk_history_max_records=100,trace_disk_history_max_bytes=65536)
        with self.assertRaises(ValueError):ack.terminal_trace_history_budget(1000001,trace_requested=True)

    def test_injected_storage_failures_retain_both_full_fingerprints(self):
        original=storage.SQLiteHistory._execute
        for prefix in ("INSERT INTO tasks", "SELECT kind", "UPDATE tasks", "COMMIT"):
            def broken(owner,sql,parameters=(),prefix=prefix):
                if sql.startswith(prefix):owner._fail(sqlite3.OperationalError("injected "+prefix))
                return original(owner,sql,parameters)
            with self.subTest(prefix=prefix),mock.patch.object(storage.SQLiteHistory,"_execute",broken):
                evidence=self.assert_fingerprint_error(capture(),"injected",**self.options())
                self.assertTrue(evidence["history_storage"]["closed"])
                self.assertTrue(evidence["history_storage"]["failed"])

    def test_incomplete_source_remains_invalid_and_no_database_reopen(self):
        doc=capture();doc["jceTrace"].update(lost=241611,cursor_lost=241611,recorded=241614)
        self.assert_fingerprint_error(doc,"incomplete",**self.options())
        options=self.options();self.file(capture(),**options)
        self.assert_fingerprint_error(capture(),"already exists",**options)


    def test_session_and_finish_scan_storage_failures(self):
        original=storage.SQLiteHistory._execute
        document=trace_document()
        for prefix,flags,required in [
            ("INSERT INTO sessions",dict(cleanup_ownership=True,terminal_session_binding=True),["engine.log_quiesce"]),
            ("SELECT identity,kind",dict(cleanup_ownership=True),["engine.log_quiesce"])]:
            def broken(owner,sql,parameters=(),prefix=prefix):
                if sql.startswith(prefix):owner._fail(sqlite3.OperationalError("injected "+prefix))
                return original(owner,sql,parameters)
            with self.subTest(prefix=prefix),mock.patch.object(storage.SQLiteHistory,"_execute",broken):
                error=self.assert_fingerprint_error(document,"injected",required=required,**flags,**self.options())
                self.assertTrue(error["history_storage"]["closed"])

    def test_commit_failure_counts_sql_accepted_row(self):
        h=storage.SQLiteHistory(self.root/"commit.sqlite",100,1024*1024)
        original=storage.SQLiteHistory._execute
        def broken(owner,sql,parameters=()):
            if sql=="COMMIT":owner._fail("injected commit")
            return original(owner,sql,parameters)
        try:
            with mock.patch.object(storage,"BATCH_WRITES",1),mock.patch.object(storage.SQLiteHistory,"_execute",broken):
                with self.assertRaises(storage.HistoryStorageError):h.tasks[1]=(2,"a",0,0)
            self.assertEqual(h.evidence()["task_records"],1)
        finally:h.close()

    def test_close_failure_is_invalid_even_after_real_close(self):
        real_connect=storage.sqlite3.connect
        class Proxy:
            def __init__(self,connection):self.connection=connection
            def execute(self,*a,**k):return self.connection.execute(*a,**k)
            def close(self):
                self.connection.close()
                raise sqlite3.OperationalError("injected after real close")
        with mock.patch.object(storage.sqlite3,"connect",side_effect=lambda *a,**k:Proxy(real_connect(*a,**k))):
            error=self.assert_fingerprint_error(capture(),"after real close",**self.options())
        self.assertFalse(error["history_storage"]["connection_close_returned"])
        self.assertFalse(error["history_storage"]["closed"])
        self.assertTrue(error["history_storage"]["fd_close_returned"])

    def test_preclose_failure_retains_unverified_handle_without_pass(self):
        real_connect=storage.sqlite3.connect;connections=[]
        class Proxy:
            def __init__(self,connection):self.connection=connection;connections.append(connection)
            def execute(self,*a,**k):return self.connection.execute(*a,**k)
            def close(self):raise sqlite3.OperationalError("injected before close")
        try:
            with mock.patch.object(storage.sqlite3,"connect",side_effect=lambda *a,**k:Proxy(real_connect(*a,**k))):
                error=self.assert_fingerprint_error(capture(),"before close",**self.options())
            self.assertFalse(error["history_storage"]["closed"])
        finally:
            for connection in connections:connection.close()

    def test_canonical_cli_explicit_disk_and_active_name_limits(self):
        import contextlib, io, sys
        options=self.options();out=self.root/"report.json"
        self.path.write_text(json.dumps(capture()),encoding="utf-8")
        argv=["lifecycle_trace_report",str(self.path),"--require","app.init","--out",str(out),
              "--history-db",str(options["history_db_path"]),"--disk-history-max-records","10000",
              "--disk-history-max-bytes","2097152"]
        with mock.patch.object(sys,"argv",argv),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(trace.main(),0)
        self.assertTrue(json.loads(out.read_text())["history_storage"]["closed"])
        doc=capture();doc["traceEvents"][2:2]=[dict(ph="B",cat="frame",pid=1,tid=1,ts=1500,name="other",args=dict(id=2,parent=1))]
        doc["jceTrace"].update(recorded=4,captured=4)
        self.assert_fingerprint_error(doc,"active scope limit",max_active=1,**self.options())
        self.assert_fingerprint_error(doc,"distinct retained name",max_names=1,**self.options())

    def test_finish_cursor_iteration_and_oserror_close_are_canonical_failures(self):
        original=storage.SQLiteHistory._execute
        document=trace_document()
        for phase in ("iterate","close"):
            class Cursor:
                def __init__(self,actual):self.actual=actual
                def __iter__(self):
                    yield from self.actual
                    if phase=="iterate":raise OSError("injected finish iteration")
                def close(self):
                    self.actual.close()
                    if phase=="close":raise OSError("injected finish cursor close")
            def wrapped(owner,sql,parameters=()):
                result=original(owner,sql,parameters)
                return Cursor(result) if sql.startswith("SELECT identity,kind") else result
            with self.subTest(phase=phase),mock.patch.object(storage.SQLiteHistory,"_execute",wrapped):
                error=self.assert_fingerprint_error(document,"injected finish",required=["engine.log_quiesce"],cleanup_ownership=True,**self.options())
                self.assertTrue(error["history_storage"]["closed"])


if __name__ == "__main__":
    unittest.main()
