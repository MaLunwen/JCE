"""JCE build entry consistency and XP libhevc condition-variable controls.

CPU-only checks do not prove full XP link, GUI start or guest execution.
"""
from __future__ import annotations
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]

def _driver():
    spec = importlib.util.spec_from_file_location("jce_v3_unified", ROOT / "tools/build/jce.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

class EntryConsistencyTests(unittest.TestCase):
    def test_all_main_windows_shims_go_through_jce(self):
        for name in ("build-editor", "build-host-tools", "build-project", "build-sdk",
                     "package-editor", "package-game"):
            path = ROOT / "scripts/windows" / (name + ".bat")
            self.assertIn('jce.py"', path.read_text(encoding="utf-8").lower(), name)
        for name in ("build-web", "build-android"):
            path = ROOT / "scripts" / (name + ".bat")
            self.assertIn('jce.py"', path.read_text(encoding="utf-8").lower(), name)
        for osdir in ("linux", "macos"):
            for name in ("build-editor", "build-host-tools", "build-project", "build-sdk",
                         "package-editor", "package-game"):
                path = ROOT / "scripts" / osdir / (name + ".sh")
                self.assertIn("scripts/jce.py", path.read_text(encoding="utf-8"), str(path))

    def test_platform_args_are_forwarded_without_being_reinterpreted(self):
        driver = _driver()
        for name, argv in (("web", [str(ROOT / "fixture SDK path"), "--dist", "--clean"]),
                           ("android", [str(ROOT / "fixture NDK path"), str(ROOT / "fixture SDK path"), "arm64", "--clean"])):
            parser = driver.build_parser()
            args, unknown = parser.parse_known_args([name, *argv])
            self.assertEqual(args.cmd, name)
            self.assertEqual(unknown, argv)

    def test_xp_stage_shares_heartbeat_configuration(self):
        spec = importlib.util.spec_from_file_location("jce_v3_xp", ROOT / "tools/build/xp_editor.py")
        xp = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(xp)
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"JCE_BUILD_PROGRESS_INTERVAL": "0.25"}):
            step = xp._Stages(Path(tmp), {"stages": []})
            self.assertEqual(step.heartbeat_seconds, 0.25)
            overridden = xp._Stages(Path(tmp), {"stages": []}, heartbeat_seconds=3)
            self.assertEqual(overridden.heartbeat_seconds, 3)

    def test_shared_runner_reports_failure_and_heartbeat(self):
        driver = _driver()
        out = io.StringIO()
        with patch.object(driver, "DRY_RUN", False), \
             patch.dict(os.environ, {"JCE_BUILD_PROGRESS_INTERVAL": "0.02"}), \
             contextlib.redirect_stdout(out):
            driver.run([sys.executable, "-c", "import time;time.sleep(.10)"])
        body = out.getvalue()
        self.assertIn("STEP BEGIN", body)
        self.assertIn("STEP RUNNING", body)
        self.assertIn("STEP END", body)
        with patch.object(driver, "DRY_RUN", False), \
             contextlib.redirect_stdout(out), self.assertRaises(SystemExit):
            driver.run([sys.executable, "-c", "import sys;sys.exit(7)"])
        self.assertIn("status=FAILED", out.getvalue())

    def test_dry_run_never_launches(self):
        driver = _driver()
        with patch.object(driver, "DRY_RUN", True), \
             patch.object(driver.subprocess, "run") as invoke:
            driver.run(["cmake", "--build", "X"])
            invoke.assert_not_called()

    def test_driver_parses_platform_entries(self):
        driver = _driver()
        parser = driver.build_parser()
        for name in ("web", "android"):
            self.assertEqual(parser.parse_args([name]).func, driver.cmd_platform_build)

class XpConditionVariableTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.path = ROOT / "engine/src/os/platform/codec_ports/jce_libhevc_threads_win32.c"
        cls.source = cls.path.read_text(encoding="utf-8")

    def test_private_waiter_and_no_native_xp_cv_import(self):
        self.assertIn("JCE_BUILD_CONSISTENCY_XP_V3", self.source)
        xp = self.source.split("#if JCE_LIBHEVC_USE_XP_COND",1)[1].split("#else /* Modern",1)[0]
        for symbol in ("CreateEventA", "SetEvent", "WaitForSingleObject", "EnterCriticalSection"):
            self.assertIn(symbol, xp)
        for symbol in ("InitializeConditionVariable", "SleepConditionVariableCS", "WakeConditionVariable"):
            self.assertNotIn(symbol, xp)

    @unittest.skipUnless(shutil.which("clang"), "clang unavailable for syntax-only stub probe")
    def test_xp_cond_compiles_without_vista_api_declarations(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "ihevc_typedefs.h").write_text("typedef unsigned int UWORD32; typedef int WORD32;\n")
            (root / "ithread.h").write_text("/* CPU syntax probe declarations */\n")
            (root / "windows.h").write_text("""#ifndef _FAKE_WIN
#define _FAKE_WIN
#include <stdint.h>
#define FALSE 0
#define INFINITE 0xffffffffU
#define WAIT_OBJECT_0 0U
#ifndef __stdcall
#define __stdcall
#endif
typedef void *HANDLE;
typedef unsigned long DWORD;
typedef long LONG;
typedef uintptr_t DWORD_PTR;
typedef int BOOL;
typedef struct {int unused;} CRITICAL_SECTION;
void InitializeCriticalSection(CRITICAL_SECTION *);
void DeleteCriticalSection(CRITICAL_SECTION *);
void EnterCriticalSection(CRITICAL_SECTION *);
void LeaveCriticalSection(CRITICAL_SECTION *);
DWORD WaitForSingleObject(HANDLE, DWORD);
BOOL CloseHandle(HANDLE);
BOOL SwitchToThread(void);
void Sleep(DWORD);
HANDLE CreateSemaphoreA(void*, LONG, LONG, const char*);
BOOL ReleaseSemaphore(HANDLE, LONG, LONG*);
DWORD_PTR SetThreadAffinityMask(HANDLE, DWORD_PTR);
HANDLE GetCurrentThread(void);
HANDLE CreateEventA(void*, BOOL, BOOL, const char*);
BOOL SetEvent(HANDLE);
#endif
""")
            (root / "process.h").write_text("""#include <stdint.h>
#ifndef __stdcall
#define __stdcall
#endif
uintptr_t _beginthreadex(void*, unsigned, unsigned (__stdcall *)(void*), void*, unsigned, unsigned*);
void _endthreadex(unsigned);
""")
            cmd = [shutil.which("clang"), "-std=c99", "-fsyntax-only", "-Werror=implicit-function-declaration",
                   "-D_WIN32", "-D_WIN32_WINNT=0x0502", "-I", str(root), str(self.path)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

if __name__ == "__main__":
    unittest.main()
