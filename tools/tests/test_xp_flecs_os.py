"""Pure preparation controls for the whole-body Flecs OS CPU fixture.

No compiler/native process is launched by this module. Root's native fixture
owner consumes CASES/MUTATIONS or the generated manifest; actual C99 execution
is a separate requirement, never inferred from these source preparation tests.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BODY = Path("engine/src/middleware/scene/jce_scene_flecs_os.c")
HEADER = BODY.with_suffix(".h")
FIXTURE = Path("tests/compatibility/xp_flecs/bridge_control.c")
CASES = ("smoke", "install", "heap", "thread", "thread-fast", "rollback", "sync", "clock",
         "atomic", "foreign", "foreign-empty", "late-defaults", "refused-set", "contended")
MUTATIONS = {
    "omitted_result": ("ticket->result = ticket->callback(ticket->argument);",
                       "(void)ticket->callback(ticket->argument);", "thread",
                       "callback result survives Core join"),
    "missing_join": ("jce_thread_join(ticket->thread);", "(void)ticket->thread;", "thread",
                     "callback result survives Core join"),
    "old_time_units": ("return jce_time_ticks_ns();", "return jce_time_ticks_ms();", "clock",
                       "coarse monotonic nanosecond units"),
    "wrong_condition": ("jce_cond_broadcast((JceCondVar *)(uintptr_t)condition);",
                        "jce_cond_signal((JceCondVar *)(uintptr_t)condition);", "sync", "exact sync wiring"),
    "wrong_heap_counter": ("&ecs_os_api_calloc_count", "&ecs_os_api_malloc_count", "heap",
                           "Flecs attempt counters"),
    "missing_set": ("ecs_os_set_api(&api);", "(void)api;", "install", "prepare"),
}


def _sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_mutants(output, *, repo=ROOT):
    """Write only fresh generated first-party mutants below the selected build/."""
    repo = Path(repo).resolve()
    output = Path(output).absolute()
    for path in (output, *output.parents):
        if path.exists() and (path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400):
            raise ValueError("fixture output may not use a reparse/link path")
    output = output.resolve()
    if not output.is_relative_to(repo / "build") or output == repo / "build" or output.exists():
        raise ValueError("mutant output must be a fresh strict child of build/")
    source = (repo / BODY).read_text(encoding="utf-8")
    header = (repo / HEADER).as_posix()
    if any(character in header for character in ('"', "\n", "\r")):
        raise ValueError("fixture header path cannot be represented")
    include = '#include "jce_scene_flecs_os.h"'
    if source.count(include) != 1:
        raise ValueError("production private header wire changed")
    variants = {}
    for name, (before, after, case, label) in MUTATIONS.items():
        if source.count(before) != 1:
            raise ValueError("production mutation anchor changed: " + name)
        variants[name] = source.replace(before, after, 1).replace(include, '#include "' + header + '"', 1)
    output.mkdir(parents=True, exist_ok=False)
    manifest = {"schema": "jce.xp.flecs-bridge-controls.v1", "body_sha256": _sha(repo / BODY),
                "header_sha256": _sha(repo / HEADER), "fixture_sha256": _sha(repo / FIXTURE),
                "cases": list(CASES), "modern_case": "modern", "track_case": "track", "mutants": {},
                "native_execution": "NOT_EXECUTED", "scope": "typed CPU ports; no Flecs world, SDL thread or graphical evidence"}
    for name, value in variants.items():
        path = output / (name + ".c")
        path.write_text(value, encoding="utf-8", newline="\n")
        _before, _after, case, label = MUTATIONS[name]
        manifest["mutants"][name] = {"path": str(path), "sha256": _sha(path), "case": case,
                                     "expected_returncode": 4, "marker": "JCE_XP_FLECS_BRIDGE_ASSERT:" + label}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    return manifest


class FlecsOsFixturePreparationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name) / "fixture repo with spaces"
        for relative in (BODY, HEADER, FIXTURE):
            path = self.repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, path)
        self.output = self.repo / "build/validation/flecs-controls"

    def test_mutants_bind_actual_whole_body_and_expected_ordinary_failures(self):
        before = _sha(self.repo / BODY)
        manifest = write_mutants(self.output, repo=self.repo)
        self.assertEqual(manifest, json.loads((self.output / "manifest.json").read_text()))
        self.assertEqual(manifest["body_sha256"], before)
        self.assertEqual(_sha(self.repo / BODY), before)
        self.assertEqual(manifest["native_execution"], "NOT_EXECUTED")
        self.assertEqual(set(manifest["mutants"]), set(MUTATIONS))
        for name, record in manifest["mutants"].items():
            source = Path(record["path"]).read_text()
            self.assertIn(MUTATIONS[name][1], source)
            self.assertIn('#include "' + (self.repo / HEADER).as_posix() + '"', source)
            self.assertEqual(record["expected_returncode"], 4)
            self.assertTrue(record["marker"].startswith("JCE_XP_FLECS_BRIDGE_ASSERT:"))
            self.assertEqual(record["sha256"], _sha(Path(record["path"])))

    def test_output_refuses_existing_or_unbounded_destinations(self):
        self.output.mkdir(parents=True)
        sentinel = self.output / "preserve"
        sentinel.write_bytes(b"owned output stays")
        with self.assertRaisesRegex(ValueError, "fresh strict child"):
            write_mutants(self.output, repo=self.repo)
        self.assertEqual(sentinel.read_bytes(), b"owned output stays")
        with self.assertRaisesRegex(ValueError, "fresh strict child"):
            write_mutants(self.repo / "source-output", repo=self.repo)

    def test_changed_anchor_is_unavailable_and_produces_no_partial_mutants(self):
        path = self.repo / BODY
        path.write_text(path.read_text().replace("return jce_time_ticks_ns();", "return 1;"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "mutation anchor"):
            write_mutants(self.output, repo=self.repo)
        self.assertFalse(self.output.exists())

    def test_fixture_uses_whole_production_tu_and_real_dependency_headers(self):
        fixture = (ROOT / FIXTURE).read_text(encoding="utf-8")
        self.assertIn("#include JCE_XP_FLECS_BRIDGE_SOURCE", fixture)
        self.assertIn("#include <flecs.h>", fixture)
        self.assertIn('#include "os/platform/jce_native_atomic_integer_internal.h"', fixture)
        for case in CASES:
            self.assertIn('"' + case + '"', fixture)
        self.assertIn("JCE_XP_FLECS_BRIDGE_OK case=%s", fixture)
        self.assertIn("return 4;", fixture)


if __name__ == "__main__":
    if "--write-mutants" in sys.argv:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("--write-mutants", type=Path, required=True)
        args = parser.parse_args()
        write_mutants(args.write_mutants)
    else:
        unittest.main()
