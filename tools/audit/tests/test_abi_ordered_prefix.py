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

import importlib.util
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "tools" / "audit" / "check_abi_snapshot.py"


def load_module():
    spec = importlib.util.spec_from_file_location(
        "jce_check_abi_snapshot", MODULE_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
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
