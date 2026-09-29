#!/usr/bin/env python3
"""test_abi_nested_struct.py — the ABI gate's own unit tests.

check_abi_snapshot.py extracted struct layouts with a regex whose body class
was `[^{}]*`.  That class cannot cross a nested brace, so a `typedef struct {
... struct { ... } member[N]; ... } Name;` matched NOTHING and the record was
silently absent from the snapshot.  Six public records were in that state, one
of them JceInputFrame -- the on-disk record/replay wire format.  A gate that is
blind to a record cannot fail when the record changes, and nobody could tell
the difference between "no ABI break" and "not looking".

A gate fix that is itself unguarded is not a fix, so these are the tests for it.

Run:
  python -m unittest discover -s tools/audit/tests -p "test_*.py" -v
"""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "tools" / "audit" / "check_abi_snapshot.py"


def load_module():
    """Import check_abi_snapshot.py by path (it is a script, not a package)."""
    spec = importlib.util.spec_from_file_location(
        "jce_check_abi_snapshot", MODULE_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class NestedStructVisibility(unittest.TestCase):
    def setUp(self):
        self.abi = load_module()
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        # parse_header() reports paths relative to PUBLIC_ROOT; point it at the
        # scratch dir so a synthetic header parses the way a real one does.
        self.abi.PUBLIC_ROOT = self.root

    def tearDown(self):
        self._tmp.cleanup()

    def parse(self, source):
        path = self.root / "probe.h"
        # newline="\n": Python's default text mode writes CRLF on Windows.
        path.write_text(source, encoding="utf-8", newline="\n")
        return self.abi.parse_header(path)

    def structs(self, decls):
        out = {}
        for line in decls:
            if line.startswith("struct "):
                out[line.split()[1]] = line
        return out

    def test_flat_struct_is_still_visible(self):
        """The fix must not lose what the regex already found."""
        decls = self.parse(
            "typedef struct JceFlat {\n"
            "    uint32_t version;\n"
            "    float    x, y;\n"
            "} JceFlat;\n")
        s = self.structs(decls)
        self.assertIn("JceFlat", s)
        self.assertIn("uint32_t version", s["JceFlat"])
        self.assertIn("float x, y", s["JceFlat"])

    def test_nested_anonymous_struct_is_visible(self):
        """The whole point: `[^{}]*` could not cross the inner brace."""
        decls = self.parse(
            "typedef struct JceNested {\n"
            "    uint32_t count;\n"
            "    struct {\n"
            "        uint32_t buttons;\n"
            "        float    axes[8];\n"
            "    } pads[4];\n"
            "} JceNested;\n")
        s = self.structs(decls)
        self.assertIn("JceNested", s)
        self.assertIn("uint32_t count", s["JceNested"])
        self.assertIn("pads[4]", s["JceNested"])

    def test_nested_anonymous_union_is_visible(self):
        """JceEvent's shape: a tagged union with no member name."""
        decls = self.parse(
            "typedef struct JceTagged {\n"
            "    int32_t kind;\n"
            "    union {\n"
            "        float   f;\n"
            "        uint8_t raw[16];\n"
            "    };\n"
            "} JceTagged;\n")
        s = self.structs(decls)
        self.assertIn("JceTagged", s)
        self.assertIn("uint8_t raw[16]", s["JceTagged"])

    def test_growing_a_nested_member_changes_the_declaration(self):
        """The failure the gate exists to catch: a field added INSIDE the
        nested member must produce a different snapshot line."""
        before = self.parse(
            "typedef struct JceNested {\n"
            "    uint32_t count;\n"
            "    struct { uint32_t buttons; } pads[4];\n"
            "} JceNested;\n")
        after = self.parse(
            "typedef struct JceNested {\n"
            "    uint32_t count;\n"
            "    struct { uint32_t buttons; uint8_t hats[4]; } pads[4];\n"
            "} JceNested;\n")
        self.assertNotEqual(self.structs(before)["JceNested"],
                            self.structs(after)["JceNested"])

    def test_unterminated_struct_does_not_hang_or_throw(self):
        """A header mid-edit must not take the whole audit down."""
        decls = self.parse("typedef struct JceBroken {\n    int a;\n")
        self.assertNotIn("JceBroken", self.structs(decls))


class RealPublicHeaders(unittest.TestCase):
    def test_jce_input_frame_is_in_the_snapshot(self):
        """JceInputFrame is the .jirc wire format and has never been guarded."""
        abi = load_module()
        decls, nfiles = abi.build_snapshot()
        self.assertGreater(nfiles, 0, "walked zero headers")
        names = {ln.split()[1] for ln in decls if ln.startswith("struct ")}
        self.assertIn("JceInputFrame", names)

    def test_jce_event_is_in_the_snapshot(self):
        """The tagged union an app's on_event() receives."""
        abi = load_module()
        decls, nfiles = abi.build_snapshot()
        self.assertGreater(nfiles, 0, "walked zero headers")
        names = {ln.split()[1] for ln in decls if ln.startswith("struct ")}
        self.assertIn("JceEvent", names)


class SnapshotWriterLineEndings(unittest.TestCase):
    """`--update` wrote the snapshot with `Path.write_text(..., encoding="utf-8")`
    and no `newline=` argument, so on Windows every regeneration silently
    re-encoded this LF-policy file (.gitattributes: `* text=auto eol=lf`) as
    CRLF. This asserts on the BYTES THE WRITER PRODUCES, not on the checked-out
    file's current state -- a machine where git already normalised the working
    copy back to LF would make a state-based assertion pass for the wrong
    reason."""

    def setUp(self):
        self.abi = load_module()
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        # --update prints "snapshot updated: <SNAPSHOT relative to REPO_ROOT>",
        # so REPO_ROOT must be an ancestor of the redirected SNAPSHOT path too.
        self.abi.REPO_ROOT = self.root
        self.abi.PUBLIC_ROOT = self.root
        self.abi.SNAPSHOT = self.root / "abi-snapshot.txt"
        # One tiny header is enough for build_snapshot() to have real content
        # to write, without walking the whole repo.
        (self.root / "probe.h").write_text(
            "typedef struct JceFlat {\n    uint32_t x;\n} JceFlat;\n",
            encoding="utf-8", newline="\n")

    def tearDown(self):
        self._tmp.cleanup()

    def test_update_does_not_write_crlf(self):
        argv = sys.argv
        sys.argv = ["check_abi_snapshot.py", "--update"]
        try:
            rc = self.abi.main()
        finally:
            sys.argv = argv
        self.assertEqual(rc, 0)
        data = self.abi.SNAPSHOT.read_bytes()
        self.assertGreater(len(data), 0)
        self.assertEqual(data.count(b"\r\n"), 0,
                         "--update must not reintroduce CRLF")


if __name__ == "__main__":
    unittest.main()
