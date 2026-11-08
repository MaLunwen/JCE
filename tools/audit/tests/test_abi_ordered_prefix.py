#!/usr/bin/env python3
"""test_abi_ordered_prefix.py — tests for the ABI gate's ordered-prefix rule
and for its committed-tree mode.

THE DEFECT THESE GUARD
----------------------
check_abi_snapshot.py compared the whole normalised text of a struct or enum
and reported CHANGED whether a member was APPENDED or INSERTED IN THE MIDDLE.
Those are not the same event:

  append  -> every existing member keeps its index; an already-compiled
             consumer stays correct.
  insert  -> every later enumerator renumbers and every later struct member
             shifts; an already-compiled consumer reads the wrong field, or,
             for a table of function pointers, calls through the wrong slot.

engine/include/jce/middleware/script/jce_script.h declares JceScriptHost
"APPEND ONLY" precisely because of the second case.  Until this rule existed
nothing enforced that sentence — a human had to read the diff.

Run:
  python -m unittest discover -s tools/audit/tests -p "test_*.py" -v
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "tools" / "audit" / "check_abi_snapshot.py"


def load_module(source_transform=None):
    spec = importlib.util.spec_from_file_location(
        "jce_check_abi_snapshot", MODULE_PATH)
    mod = importlib.util.module_from_spec(spec)
    if source_transform is None:
        spec.loader.exec_module(mod)
    else:
        source = source_transform(MODULE_PATH.read_text(encoding="utf-8"))
        exec(compile(source, str(MODULE_PATH), "exec"), mod.__dict__)
    return mod


class OrderedPrefixRule(unittest.TestCase):
    """Every case goes through the real emitter: a synthetic snapshot line
    would test the classifier against a format only this file believes in."""

    def setUp(self):
        self.abi = load_module()

    def line(self, source: str, kind: str, name: str) -> str:
        decls = self.abi.parse_header_text(source, "probe.h")
        want = f"{kind} {name} "
        hits = [d for d in decls if d.startswith(want)]
        self.assertEqual(len(hits), 1,
                         f"emitter produced {len(hits)} {kind} {name} lines "
                         f"from this source; the test would be checking "
                         f"nothing. decls={decls}")
        return hits[0]

    # ---- enums ---------------------------------------------------------

    ENUM_BASE = ("typedef enum {\n"
                 "    JCE_A = 0,\n"
                 "    JCE_B,\n"
                 "    JCE_C\n"
                 "} JceProbe;\n")

    def test_enum_append_is_compatible(self):
        after = self.ENUM_BASE.replace("    JCE_C\n", "    JCE_C,\n    JCE_D\n")
        cat, why = self.abi.classify_change(
            self.line(self.ENUM_BASE, "enum", "JceProbe"),
            self.line(after, "enum", "JceProbe"))
        self.assertEqual(cat, "appended", why)
        self.assertIn("JCE_D", why)
        self.assertIn("[3]", why)

    def test_enum_middle_insertion_is_incompatible_and_named(self):
        after = self.ENUM_BASE.replace("    JCE_B,\n", "    JCE_B,\n    JCE_BB,\n")
        cat, why = self.abi.classify_change(
            self.line(self.ENUM_BASE, "enum", "JceProbe"),
            self.line(after, "enum", "JceProbe"))
        self.assertEqual(cat, "changed", why)
        self.assertIn("INSERTED at index 2", why)
        self.assertIn("JCE_BB", why)   # the member that was inserted
        self.assertIn("JCE_C", why)    # the member whose value it shifted

    def test_enum_reorder_is_incompatible(self):
        after = ("typedef enum {\n    JCE_A = 0,\n    JCE_C,\n    JCE_B\n"
                 "} JceProbe;\n")
        cat, why = self.abi.classify_change(
            self.line(self.ENUM_BASE, "enum", "JceProbe"),
            self.line(after, "enum", "JceProbe"))
        self.assertEqual(cat, "changed", why)
        self.assertIn("index 1", why)

    def test_enum_explicit_value_change_is_incompatible(self):
        after = self.ENUM_BASE.replace("JCE_A = 0", "JCE_A = 1")
        cat, why = self.abi.classify_change(
            self.line(self.ENUM_BASE, "enum", "JceProbe"),
            self.line(after, "enum", "JceProbe"))
        self.assertEqual(cat, "changed", why)
        self.assertIn("[0]", why)

    # ---- structs -------------------------------------------------------

    STRUCT_BASE = ("typedef struct {\n"
                   "    uint32_t version;\n"
                   "    uint32_t flags;\n"
                   "    float    scale;\n"
                   "} JceProbeRec;\n")

    def test_struct_append_is_compatible(self):
        after = self.STRUCT_BASE.replace("} JceProbeRec;",
                                         "    uint64_t extra;\n} JceProbeRec;")
        cat, why = self.abi.classify_change(
            self.line(self.STRUCT_BASE, "struct", "JceProbeRec"),
            self.line(after, "struct", "JceProbeRec"))
        self.assertEqual(cat, "appended", why)
        self.assertIn("uint64_t extra", why)

    def test_struct_middle_insertion_is_incompatible_and_named(self):
        after = self.STRUCT_BASE.replace("    float    scale;\n",
                                         "    uint32_t mode;\n"
                                         "    float    scale;\n")
        cat, why = self.abi.classify_change(
            self.line(self.STRUCT_BASE, "struct", "JceProbeRec"),
            self.line(after, "struct", "JceProbeRec"))
        self.assertEqual(cat, "changed", why)
        self.assertIn("INSERTED at index 2", why)
        self.assertIn("uint32_t mode", why)
        self.assertIn("float scale", why)

    def test_struct_member_rename_is_incompatible(self):
        after = self.STRUCT_BASE.replace("uint32_t flags", "uint32_t bits")
        cat, why = self.abi.classify_change(
            self.line(self.STRUCT_BASE, "struct", "JceProbeRec"),
            self.line(after, "struct", "JceProbeRec"))
        self.assertEqual(cat, "changed", why)
        self.assertIn("member [1] changed", why)

    def test_struct_trailing_member_removal_is_incompatible(self):
        after = self.STRUCT_BASE.replace("    float    scale;\n", "")
        cat, why = self.abi.classify_change(
            self.line(self.STRUCT_BASE, "struct", "JceProbeRec"),
            self.line(after, "struct", "JceProbeRec"))
        self.assertEqual(cat, "changed", why)
        self.assertIn("REMOVED", why)
        self.assertIn("[2]", why)

    def test_append_after_a_nested_anonymous_union_is_still_an_append(self):
        """The emitter joins struct fields with '; ' AFTER a depth-aware
        split, so the classifier's inverse split must be depth-aware too.  A
        naive line.split('; ') would tear this record's nested union into
        three members and report a pure append as an insertion."""
        base = ("typedef struct {\n"
                "    int32_t kind;\n"
                "    union {\n"
                "        float   f;\n"
                "        uint8_t raw[16];\n"
                "    } payload;\n"
                "} JceProbeTagged;\n")
        after = base.replace("} JceProbeTagged;",
                             "    uint32_t appended;\n} JceProbeTagged;")
        b = self.line(base, "struct", "JceProbeTagged")
        _path, members = self.abi.record_members(b)
        self.assertEqual(len(members), 2,
                         f"nested union was torn apart: {members}")
        cat, why = self.abi.classify_change(
            b, self.line(after, "struct", "JceProbeTagged"))
        self.assertEqual(cat, "appended", why)
        self.assertIn("[2] uint32_t appended", why)

    # ---- the categories that are NOT member-ordered ---------------------

    def test_same_record_in_a_different_header_is_moved_not_changed(self):
        decls_a = self.abi.parse_header_text(self.ENUM_BASE, "old/probe.h")
        decls_b = self.abi.parse_header_text(self.ENUM_BASE, "new/probe.h")
        cat, why = self.abi.classify_change(decls_a[0], decls_b[0])
        self.assertEqual(cat, "moved", why)
        self.assertIn("old/probe.h -> new/probe.h", why)

    def test_function_signature_change_has_no_member_order(self):
        before = self.abi.parse_header_text(
            "JCE_API void jce_probe(int a);\n", "probe.h")[0]
        after = self.abi.parse_header_text(
            "JCE_API void jce_probe(long a);\n", "probe.h")[0]
        self.assertEqual(self.abi.classify_change(before, after)[0], "changed")


class DiffSeverity(unittest.TestCase):
    def setUp(self):
        self.abi = load_module()

    def test_append_is_drift_but_not_fatal(self):
        before = self.abi.parse_header_text(
            "typedef enum { JCE_A = 0 } JceProbe;\n", "probe.h")
        after = self.abi.parse_header_text(
            "typedef enum { JCE_A = 0, JCE_B } JceProbe;\n", "probe.h")
        d = self.abi.diff_snapshot(before, after)
        self.assertEqual(d["fatal"], 0)
        self.assertEqual(d["drift"], 1)
        self.assertEqual(len(d["appended"]), 1)

    def test_insertion_is_fatal(self):
        before = self.abi.parse_header_text(
            "typedef enum { JCE_A = 0, JCE_B } JceProbe;\n", "probe.h")
        after = self.abi.parse_header_text(
            "typedef enum { JCE_A = 0, JCE_X, JCE_B } JceProbe;\n", "probe.h")
        d = self.abi.diff_snapshot(before, after)
        self.assertEqual(d["fatal"], 1)
        self.assertEqual(len(d["changed"]), 1)


LOCATION_WIRE = '    b_loc = LOCATION_LINE_RE.fullmatch(before)\n    a_loc = LOCATION_LINE_RE.fullmatch(after)\n    if (b_loc is not None and a_loc is not None\n            and b_loc.group("declaration") == a_loc.group("declaration")\n            and b_loc.group("path") != a_loc.group("path")):\n        return "moved", (f"declared in {b_loc.group(\'path\')} -> "\n                         f"{a_loc.group(\'path\')}")\n\n'


class HeaderRelocation(unittest.TestCase):
    """Actual emitter, full diff and CLI; location alone never edits an ABI."""

    FN = "JCE_API uint32_t jce_probe(uint8_t *out, uint32_t cap);\n"
    FNPTR = "typedef void (*JceProbeFn)(const uint8_t *data, uint32_t size);\n"
    OPAQUE = "typedef struct JceProbe JceProbe;\n"

    def setUp(self):
        self.abi = load_module()

    def moved(self, source):
        before = self.abi.parse_header_text(source, "old/probe.h")
        after = self.abi.parse_header_text(source, "new/probe.h")
        self.assertEqual(len(before), 1, before)
        self.assertEqual(len(after), 1, after)
        d = self.abi.diff_snapshot(before, after)
        self.assertEqual(d["fatal"], 0, d)
        self.assertEqual(len(d["moved"]), 1, d)
        self.assertEqual(d["drift"], 1, d)  # committed mode still rejects drift
        self.assertIn("old/probe.h -> new/probe.h", d["moved"][0][1])

    def test_function_relocation_is_moved(self):
        self.moved(self.FN)

    def test_function_pointer_relocation_is_moved(self):
        self.moved(self.FNPTR)

    def test_opaque_relocation_is_moved(self):
        self.moved(self.OPAQUE)

    def test_incompatible_edit_cannot_hide_behind_relocation(self):
        cases = [
            (self.FN, self.FN.replace("uint32_t jce_probe", "uint64_t jce_probe")),
            (self.FN, self.FN.replace("uint32_t cap", "uint64_t cap")),
            (self.FNPTR, self.FNPTR.replace("uint32_t size", "uint64_t size")),
            (self.OPAQUE, self.OPAQUE.replace("struct JceProbe", "struct JceOther")),
            ("typedef enum { A = 0, B } JceEnum;\n",
             "typedef enum { A = 1, B } JceEnum;\n"),
            ("typedef struct { uint8_t bytes[4]; int a; } JceRec;\n",
             "typedef struct { uint8_t bytes[8]; int a; } JceRec;\n"),
            ("typedef struct { int a; int b; } JceRec;\n",
             "typedef struct { int b; int a; } JceRec;\n"),
            ("typedef struct { int a; int b; } JceRec;\n",
             "typedef struct { int a; int x; int b; } JceRec;\n"),
            ("typedef struct { int a; int b; } JceRec;\n",
             "typedef struct { int a; } JceRec;\n"),
        ]
        for before, after in cases:
            with self.subTest(after=after):
                d = self.abi.diff_snapshot(
                    self.abi.parse_header_text(before, "old/probe.h"),
                    self.abi.parse_header_text(after, "new/probe.h"))
                self.assertEqual(d["fatal"], 1, d)
                self.assertEqual(len(d["changed"]), 1, d)
                self.assertEqual(d["moved"], [], d)

    def test_declaration_removal_stays_fatal(self):
        before = self.abi.parse_header_text(self.FN + self.FNPTR, "old/probe.h")
        after = self.abi.parse_header_text(self.FNPTR, "new/probe.h")
        d = self.abi.diff_snapshot(before, after)
        self.assertEqual(d["removed"], ["fn jce_probe"])
        self.assertEqual(d["fatal"], 1)
        self.assertEqual(len(d["moved"]), 1)

    def test_worktree_cli_reports_moves_and_rejects_real_change(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            baseline = self.abi.parse_header_text(
                self.FN + self.FNPTR + self.OPAQUE, "old/probe.h")
            snapshot = root / "baseline.txt"
            snapshot.write_text("\n".join(baseline) + "\n", encoding="utf-8", newline="\n")
            header = root / "new" / "probe.h"
            header.parent.mkdir()
            source = self.FN + self.FNPTR + self.OPAQUE
            added = "JCE_API bool jce_added(void);\n"
            with mock.patch.multiple(self.abi, PUBLIC_ROOT=root, SNAPSHOT=snapshot), \
                    mock.patch("sys.argv", [str(MODULE_PATH), "--json"]):
                for source_now, expected_rc, moves, changes in (
                        (source + added, 0, 3, 0),
                        (source.replace("uint32_t cap", "uint64_t cap") + added,
                         1, 2, 1)):
                    header.write_text(source_now, encoding="utf-8", newline="\n")
                    out = io.StringIO()
                    with contextlib.redirect_stdout(out):
                        rc = self.abi.main()
                    report = json.loads(out.getvalue())
                    self.assertEqual(rc, expected_rc, report)
                    self.assertEqual(report["headers"], 1)
                    self.assertEqual(len(report["moved"]), moves)
                    self.assertEqual(len(report["changed"]), changes)
                    self.assertEqual(len(report["added"]), 1)
                    self.assertEqual(report["removed"], [])
            self.assertEqual(snapshot.read_text(encoding="utf-8"),
                             "\n".join(baseline) + "\n")

    def test_omitted_location_wire_fails_ordinary_controls(self):
        wire = LOCATION_WIRE

        def omit(source):
            self.assertEqual(source.count(wire), 1)
            return source.replace(wire, "", 1)

        broken = load_module(omit)
        suite = unittest.TestSuite()
        for name in ("test_function_relocation_is_moved",
                     "test_function_pointer_relocation_is_moved",
                     "test_opaque_relocation_is_moved"):
            case = HeaderRelocation(name)
            case.setUp = lambda case=case: setattr(case, "abi", broken)
            suite.addTest(case)
        output = io.StringIO()
        result = unittest.TextTestRunner(stream=output, verbosity=2).run(suite)
        self.assertEqual(result.testsRun, 3)
        self.assertEqual(len(result.failures), 3, output.getvalue())
        self.assertEqual(result.errors, [], output.getvalue())
        self.assertEqual(result.skipped, [])
        self.assertFalse(result.wasSuccessful())


class CommittedTreeReader(unittest.TestCase):
    """The --committed mode reads blobs out of the object database.  If that
    read silently returned nothing, every future run would print OK."""

    def setUp(self):
        self.abi = load_module()

    def test_head_yields_headers_and_declarations(self):
        if not self.abi.git_work_tree():
            self.skipTest("not a git work tree")
        decls, nfiles = self.abi.build_snapshot_from_commit("HEAD")
        self.assertGreater(nfiles, 100,
                           "read almost no public headers out of HEAD")
        self.assertGreater(len(decls), 1000,
                           "read headers but parsed almost no declarations")

    def test_committed_reader_agrees_with_the_working_tree_on_clean_files(self):
        """A per-file cross-check: for a header that is NOT dirty, the blob at
        HEAD and the file on disk must produce identical declarations.  This is
        what proves the tar path and the os.walk path parse the same way."""
        if not self.abi.git_work_tree():
            self.skipTest("not a git work tree")
        dirty = {ln[3:].strip().strip('"')
                 for ln in (self.abi.dirty_public_headers() or [])}
        committed, _ = self.abi.build_snapshot_from_commit("HEAD")
        worktree, _ = self.abi.build_snapshot()
        # Only compare declarations whose recorded header is clean.
        def clean_only(lines):
            out = []
            for ln in lines:
                rel = ln.rsplit("[", 1)[-1].rstrip("]")
                if f"{self.abi.PUBLIC_REL}/{rel}" not in dirty:
                    out.append(ln)
            return out
        a, b = clean_only(committed), clean_only(worktree)
        self.assertGreater(len(a), 1000, "compared almost nothing")
        self.assertEqual(a, b)


if __name__ == "__main__":
    unittest.main()
