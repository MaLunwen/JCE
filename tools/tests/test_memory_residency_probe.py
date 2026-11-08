"""Residency-window controls; no real graphics or benchmark performance claim."""
import sys
import subprocess
import json
import tempfile
import io
from contextlib import redirect_stdout
from unittest import mock
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import memory_residency_probe as probe
from memory_residency_probe import summarize, validate_window

class ResidencyTests(unittest.TestCase):
    def samples(self):
        return [{"elapsed_s": i / 10, "rss_bytes": 100 + i, "commit_bytes": 500,
                 "thread_count": 3, "handle_count": None} for i in range(101)]

    def test_observed_window_and_unavailable_ownership(self):
        result = summarize(self.samples(), 2, 5)
        self.assertEqual(result["sample_count"], 51)
        self.assertEqual(result["rss_bytes"]["min"], 120)
        self.assertEqual(result["rss_bytes"]["max"], 170)
        self.assertEqual(result["allocator_ownership"], "UNAVAILABLE")
        self.assertEqual(result["handle_count"]["availability"], "UNAVAILABLE")

    def test_invalid_or_missing_windows(self):
        for warm, window in [(-1,5), (2,0), (float("nan"),5), (2,float("inf")), (20,5)]:
            with self.subTest(warm=warm,window=window), self.assertRaises(ValueError):
                summarize(self.samples(),warm,window)
        with self.assertRaises(ValueError): summarize([self.samples()[20],self.samples()[70]],2,5)

    def test_timeout_rejected_before_launch(self):
        for timeout in (0, 7, float("nan"), float("inf")):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                validate_window(2, 5, timeout)
        validate_window(2, 5, 8)
        # The real entry point rejects the window before reading inputs/launch.
        result = subprocess.run(
            [sys.executable, "-B", str(Path(__file__).resolve().parents[1] / "memory_residency_probe.py"),
             "--exe", "missing.exe", "--cwd", ".", "--env-file", "missing.json",
             "--out", "missing-output", "--warmup", "2", "--window", "5", "--timeout", "7"],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("timeout must exceed", result.stderr)
        self.assertNotIn("FileNotFoundError", result.stderr)

    def test_thread_options_are_forwarded_and_identity_retained(self):
        scratch = Path(__file__).resolve().parents[2] / "build/tests/process-sampler"
        scratch.mkdir(parents=True, exist_ok=True)
        for enabled in (False, True):
            with self.subTest(enabled=enabled), tempfile.TemporaryDirectory(dir=scratch) as temporary:
                root = Path(temporary)
                exe = root / "fixture.exe"; exe.write_bytes(b"identity fixture")
                env = root / "env.json"; env.write_text("{}", encoding="utf-8")
                identity = {"pid": 12, "creation_time_unix_s": None, "creation_time_source": "UNAVAILABLE"}
                process = {"timed_out": False, "returncode": 0,
                           "samples": self.samples(), "process_identity": identity}
                argv = ["probe", "--exe", str(exe), "--cwd", str(root), "--env-file", str(env),
                        "--out", str(root / "out"), "--warmup", "2", "--window", "5", "--timeout", "8"]
                if enabled:
                    argv += ["--sample-threads", "--thread-limit", "4"]
                with mock.patch.object(sys, "argv", argv), mock.patch.object(probe, "run_process", return_value=process) as launch, redirect_stdout(io.StringIO()):
                    probe.main()
                self.assertEqual(launch.call_args.kwargs["sample_threads"], enabled)
                self.assertEqual(launch.call_args.kwargs["thread_limit"], 4 if enabled else 256)
                report = json.loads((root / "out/residency.json").read_text(encoding="utf-8"))
                self.assertEqual(report["process_identity"], identity)
                self.assertEqual(report["thread_sampling_requested"], enabled)
                self.assertEqual(report["thread_cpu_samples"] is not None, enabled)

    def test_invalid_thread_limit_is_rejected_before_input_reads(self):
        result = subprocess.run([sys.executable, "-B",
            str(Path(__file__).resolve().parents[1] / "memory_residency_probe.py"),
            "--exe", "missing.exe", "--cwd", ".", "--env-file", "missing.json",
            "--out", "missing-output", "--sample-threads", "--thread-limit", "0"],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("thread_limit must be 1..1024", result.stderr)
        self.assertNotIn("FileNotFoundError", result.stderr)

    def test_invalid_sample_order_and_memory(self):
        for key, value in [("rss_bytes",-1), ("rss_bytes",True), ("commit_bytes","500"), ("thread_count",True), ("handle_count",-1), ("elapsed_s",float("nan"))]:
            samples=self.samples();samples[50][key]=value
            with self.subTest(key=key), self.assertRaises(ValueError): summarize(samples,2,5)
        with self.assertRaises(ValueError): summarize(list(reversed(self.samples())),2,5)


    def gpu_cli_owner(self, module, enabled=True):
        scratch=Path(__file__).resolve().parents[2]/"build/tests/process-sampler"
        scratch.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as temporary:
            root=Path(temporary)
            exe=root/"fixture.exe";exe.write_bytes(b"identity fixture")
            env=root/"env.json";env.write_text("{}",encoding="utf-8")
            identity={"pid":42,"creation_time_unix_s":123.25,"creation_time_source":"psutil.create_time"}
            process={"timed_out":False,"returncode":0,"samples":self.samples(),"process_identity":identity}
            argv=["probe","--exe",str(exe),"--cwd",str(root),"--env-file",str(env),
                  "--out",str(root/"out"),"--warmup","2","--window","5","--timeout","8"]
            if enabled:argv += ["--sample-gpu","--gpu-limit","4","--gpu-peer-pid","77"]
            with mock.patch.object(sys,"argv",argv),mock.patch.object(module,"run_process",return_value=process) as launch,redirect_stdout(io.StringIO()):
                module.main()
            self.assertIs(launch.call_args.kwargs["sample_gpu"],enabled)
            self.assertEqual(launch.call_args.kwargs["gpu_limit"],4 if enabled else 256)
            self.assertEqual(launch.call_args.kwargs["gpu_peer_pids"],[77] if enabled else [])
            report=json.loads((root/"out/residency.json").read_text(encoding="utf-8"))
            self.assertEqual(report["process_identity"],identity)
            self.assertIs(report["gpu_sampling_requested"],enabled)
            self.assertEqual(report["gpu_peer_pids"],[77] if enabled else [])
            self.assertEqual(report["gpu_samples"],str(root/"out/process/gpu-samples.json") if enabled else None)

    def test_gpu_cli_wire_and_ordinary_missing_wire_control(self):
        self.gpu_cli_owner(probe)
        self.gpu_cli_owner(probe,False)
        # Execute the complete production entry point with only its dispatch
        # wire omitted; the identical behavioral assertion must fail normally.
        import types
        source=Path(probe.__file__).read_text(encoding="utf-8")
        needle="sample_gpu=args.sample_gpu"
        self.assertEqual(source.count(needle),1)
        old=types.ModuleType("old_gpu_cli_owner");old.__file__=probe.__file__
        exec(compile(source.replace(needle,"sample_gpu=False"),probe.__file__,"exec"),old.__dict__)
        with self.assertRaises(AssertionError):self.gpu_cli_owner(old)

    def test_invalid_gpu_options_reject_before_input_reads_or_launch(self):
        base=["probe","--exe","missing.exe","--cwd",".","--env-file","missing.json","--out","missing-output"]
        for options in (["--gpu-limit","0"],["--gpu-peer-pid","77"],
                        ["--sample-gpu","--gpu-peer-pid","77","--gpu-peer-pid","77"]):
            with self.subTest(options=options),mock.patch.object(sys,"argv",base+options),mock.patch.object(probe,"digest") as reads,mock.patch.object(probe,"run_process") as launch,redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as failure:probe.main()
                self.assertEqual(failure.exception.code,2)
                reads.assert_not_called();launch.assert_not_called()

if __name__ == "__main__": unittest.main()
