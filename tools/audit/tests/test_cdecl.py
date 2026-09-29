#!/usr/bin/env python3
"""test_cdecl.py — the shared C-declaration parser's own unit tests.

check_abi_snapshot.py and gen_script_bindings.py both walk `typedef struct
{ ... }` bodies and split parameter lists.  Two copies of that parser would
drift, and a drifted parser makes one gate blind while the other stays green
-- the exact two-sided-contract failure this whole effort exists to prevent.
So there is one copy, and these are its tests.

Run:
  python -m unittest discover -s tools/audit/tests -p "test_*.py" -v
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "tools" / "scriptgen" / "cdecl.py"


def load_module():
    spec = importlib.util.spec_from_file_location("cdecl_under_test", MODULE_PATH)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["cdecl_under_test"] = mod
    spec.loader.exec_module(mod)
    return mod


class TestCdecl(unittest.TestCase):
    def setUp(self):
        self.m = load_module()

    def test_named_tag_struct_body_is_found(self):
        """JceScriptHost is `typedef struct JceScriptHost {` -- a NAMED tag.
        A parser anchored on the anonymous form would yield nothing and every
        downstream count would be 0 while every gate printed OK."""
        text = "typedef struct JceScriptHost {\n  void *user;\n} JceScriptHost;"
        got = dict(self.m.iter_typedef_struct_bodies(text))
        self.assertIn("JceScriptHost", got)
        self.assertIn("void *user", got["JceScriptHost"])

    def test_function_pointer_member_survives_field_split(self):
        """A function-pointer member holds top-level commas inside its own
        parameter list; splitting on those would shred it into fragments."""
        body = (" bool (*get_position)(void *user, JceScriptEntity e, float out_xyz[3]);"
                " void (*set_position)(void *user, JceScriptEntity e, float x); ")
        fields = self.m.split_struct_fields(body)
        self.assertEqual(len(fields), 2)
        self.assertEqual(
            fields[0],
            "bool (*get_position)(void *user, JceScriptEntity e, float out_xyz[3])")

    def test_parameter_names_survive_inside_struct_fields(self):
        """split_struct_fields is verbatim modulo whitespace -- that is why
        generated locals can be named after the header's own parameter names."""
        body = " bool (*touch_get)(void *user, int index, uint64_t *id,\n float *x); "
        self.assertEqual(
            self.m.split_struct_fields(body)[0],
            "bool (*touch_get)(void *user, int index, uint64_t *id, float *x)")

    def test_split_params_keeps_array_suffix_with_its_type(self):
        parts = self.m.split_params("void *user, JceScriptEntity e, float out_xyz[3]")
        self.assertEqual(len(parts), 3)
        self.assertTrue(parts[2].endswith("[3]"), parts[2])

    def test_split_params_raw_keeps_names_and_array_suffix(self):
        """split_params drops parameter names and hoists the array suffix onto
        the type, so `float out_xyz[3]` comes back as `float[3]` -- which
        re-parses as a parameter NAMED "float" with an EMPTY type.  A binding
        generator fed that emits locals called `float`, and does it silently:
        the member count stays right.  split_params_raw is the split
        gen_script_bindings.py imports instead."""
        args = "void *user, JceScriptEntity e, float out_xyz[3]"
        self.assertEqual([p.strip() for p in self.m.split_params_raw(args)],
                         ["void *user", "JceScriptEntity e", "float out_xyz[3]"])
        self.assertEqual(self.m.split_params(args)[2], "float[3]")

    def test_split_params_is_split_params_raw_with_names_dropped(self):
        """One comma-walker, two normalisations.  If either consumer ever grows
        its own walker the two stop agreeing on where the commas are -- and the
        depth tracking (commas inside `(...)` and `[...]` are NOT separators) is
        precisely what a hand-rolled second walker gets wrong."""
        hard = ("void *user, void (*cb)(int a, int b), "
                "int lut[JCE_MAX(1, 2)], const char *name")
        self.assertEqual(len(self.m.split_params_raw(hard)), 4)
        self.assertEqual(len(self.m.split_params(hard)),
                         len(self.m.split_params_raw(hard)))

    def test_comments_are_blanked_not_deleted(self):
        text = "typedef struct { int a; /* } NotIt; */ int b; } Real;"
        got = dict(self.m.iter_typedef_struct_bodies(
            self.m.strip_comments_and_strings(text)))
        self.assertEqual(list(got), ["Real"])


if __name__ == "__main__":
    unittest.main()
