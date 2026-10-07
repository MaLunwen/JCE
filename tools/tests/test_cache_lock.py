"""Real CPU subprocess ownership tests, with all fixtures under build/tests."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "tools/build/cache_lock.py"
spec = importlib.util.spec_from_file_location("cache_lock_test_module", MODULE)
locks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(locks)
CHILD = """
import importlib.util, pathlib, sys, time
spec=importlib.util.spec_from_file_location('lock_child', sys.argv[1])
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
with m.cache_locks([sys.argv[2]], sys.argv[3], timeout=float(sys.argv[4])):
    print('OWNED', flush=True)
    time.sleep(float(sys.argv[5]))
"""


class CacheOwnershipTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build/tests/cache-lock"
        parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=parent)
        self.root = Path(self.temporary.name)
        self.directory = self.root / "locks"
        self.resource = self.root / "native"

    def tearDown(self):
        self.temporary.cleanup()

    def child(self, resource=None, timeout=3, hold=0):
        return subprocess.Popen([sys.executable, "-B", "-c", CHILD, str(MODULE),
            str(resource or self.resource), str(self.directory), str(timeout), str(hold)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def finish(self, process):
        stdout, stderr = process.communicate(timeout=8)
        self.assertEqual(0, process.returncode, stderr)
        return stdout

    def test_contending_process_waits_then_acquires(self):
        with locks.cache_locks([self.resource], self.directory):
            child = self.child()
            try:
                time.sleep(.25)
                self.assertIsNone(child.poll())
            finally:
                pass
        self.assertIn("OWNED", self.finish(child))
        self.assertEqual(1, len(list(self.directory.glob("*.lock"))))

    def test_timeout_preserves_resource_and_kernel_crash_release(self):
        self.resource.mkdir()
        sentinel = self.resource / "sentinel"
        sentinel.write_text("preserve")
        with locks.cache_locks([self.resource], self.directory):
            child = self.child(timeout=.2)
            _, stderr = child.communicate(timeout=5)
            self.assertNotEqual(0, child.returncode)
            self.assertIn("TimeoutError", stderr)
        self.assertEqual("preserve", sentinel.read_text())
        owner = self.child(hold=30)
        try:
            self.assertEqual("OWNED", owner.stdout.readline().strip())
            owner.kill()
            owner.communicate(timeout=5)
            self.assertIn("OWNED", self.finish(self.child(timeout=1)))
        finally:
            if owner.poll() is None:
                owner.kill(); owner.communicate()

    def test_nested_alias_exception_and_independent_resource(self):
        alias = self.root / "sub" / ".." / "native"
        with locks.cache_locks([self.resource], self.directory):
            with locks.cache_locks([alias], self.directory):
                self.assertEqual(1, len(list(self.directory.glob("*.lock"))))
            self.assertIn("OWNED", self.finish(self.child(self.root / "other")))
            with self.assertRaises(RuntimeError):
                with locks.cache_locks([self.root / "new"], self.directory):
                    pass
        with self.assertRaises(ValueError):
            with locks.cache_locks([self.resource], self.directory):
                raise ValueError("fixture")
        self.assertIn("OWNED", self.finish(self.child()))

    def test_cross_thread_nested_resources_reject_without_waiting(self):
        barrier = threading.Barrier(2)
        outcomes = []
        def worker(own, other):
            try:
                with locks.cache_locks([own], self.directory, timeout=2):
                    barrier.wait(timeout=2)
                    started = time.monotonic()
                    try:
                        with locks.cache_locks([other], self.directory, timeout=1):
                            outcomes.append(("incorrectly acquired", 0))
                    except RuntimeError:
                        outcomes.append(("rejected", time.monotonic() - started))
                    barrier.wait(timeout=2)
            except Exception as error:
                outcomes.append((type(error).__name__, 0))
        x, y = self.root / "x", self.root / "y"
        threads = [threading.Thread(target=worker, args=(x, y)),
                   threading.Thread(target=worker, args=(y, x))]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(timeout=5)
            self.assertFalse(thread.is_alive(), "cross-resource nesting deadlocked")
        self.assertEqual(2, len(outcomes))
        for status, seconds in outcomes:
            self.assertEqual("rejected", status)
            self.assertLess(seconds, .5, "nested scope waited instead of rejecting")

    def test_top_level_thread_contention_waits_and_then_proceeds(self):
        waiting = threading.Event()
        acquired = threading.Event()
        errors = []
        def worker():
            try:
                with locks.cache_locks([self.resource], self.directory,
                                       timeout=2, notify=lambda message: waiting.set()):
                    acquired.set()
            except Exception as error:
                errors.append(error)
        with locks.cache_locks([self.resource], self.directory):
            thread = threading.Thread(target=worker)
            thread.start()
            self.assertTrue(waiting.wait(timeout=1))
            self.assertFalse(acquired.is_set())
        thread.join(timeout=3)
        self.assertFalse(thread.is_alive())
        self.assertTrue(acquired.is_set())
        self.assertEqual([], errors)

    def test_sorted_multi_resource_and_timeout_validation(self):
        with locks.cache_locks([self.root / "z", self.root / "a"], self.directory):
            with locks.cache_locks([self.root / "a"], self.directory):
                pass
        for timeout in (-1, float('inf'), float('nan')):
            with self.assertRaises(ValueError):
                with locks.cache_locks([], self.directory, timeout=timeout):
                    pass




class ExtendedPathAliasTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "Windows path spellings")
    def test_extended_dos_and_unc_share_resource_identity(self):
        from unittest.mock import patch
        pairs = [("C:\\cache\\item", "\\\\?\\C:\\cache\\item"),
                 ("\\\\server\\share\\item", "\\\\?\\UNC\\server\\share\\item")]
        for ordinary, extended in pairs:
            with patch.object(locks.Path, "resolve", return_value=ordinary):
                expected = locks.canonical("ignored")
            with patch.object(locks.Path, "resolve", return_value=extended):
                self.assertEqual(locks.canonical("ignored"), expected)


if __name__ == "__main__":
    unittest.main()
