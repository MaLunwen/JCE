#!/usr/bin/env python3
"""test_script_c_abi_gate.py — the C ABI backend's own gate tests.

ci.yml already runs `python -m unittest discover -s tools/audit/tests`, whose
comment states the principle: "a gate fix that is itself unguarded is not a
fix."  Every condition in gen_script_c_abi.validate() is proved here to be able
to FAIL — this repository has shipped gates whose failure path had never once
been executed.

The emitter assertions are the other half: the C test
(tests/scripting/c_abi/test_jce_script_api_abi.c) proves the built DLL behaves,
but only against the manifest as it is today.  These assert the RULES that
produced it, so a rule silently dropped is red even when today's output happens
to look the same.

Run:
  python -m unittest discover -s tools/audit/tests -p "test_*.py" -v
"""

from __future__ import annotations

import copy
import importlib
import json
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]


def load_module():
    sys.path.insert(0, str(REPO_ROOT / "tools" / "audit"))
    sys.path.insert(0, str(REPO_ROOT / "tools" / "scriptgen"))
    return importlib.import_module("gen_script_c_abi")


class CAbiGateTest(unittest.TestCase):
    def setUp(self):
        self.m = load_module()
        self.api = self.m.load_api()

    # ── the export set ────────────────────────────────────────────────
    def test_the_export_set_is_the_manifest_entries_plus_three_meta(self):
        names = self.m.export_names(self.api)
        self.assertEqual(len(names),
                         len(self.api["expose"]) + len(self.m.META))
        self.assertEqual(len(names), len(set(names)), "duplicate export name")
        for e in self.api["expose"]:
            self.assertIn("jce_script_api_" + e["name"], names)
        for meta in self.m.META:
            self.assertIn("jce_script_api_" + meta, names)

    def test_no_hand_written_entry_reaches_the_export_set(self):
        """They are excluded as a class — three are Lua-VM machinery and two
        carry the sandbox policy that lives in jce_script.c."""
        names = set(self.m.export_names(self.api))
        for e in self.api["hand_written"]:
            self.assertNotIn("jce_script_api_" + e["name"], names)

    # ── the gate's conditions can each fail ───────────────────────────
    def test_a_declared_totals_mismatch_fails(self):
        api = copy.deepcopy(self.api)
        api["expose"].pop()
        problems = self.m.validate(api)
        self.assertTrue(any("declared_totals.expose" in p for p in problems),
                        problems)

    def test_a_manifest_entry_named_like_a_meta_entry_point_fails(self):
        api = copy.deepcopy(self.api)
        clone = copy.deepcopy(api["expose"][0])
        clone["name"] = "version"          # would shadow jce_script_api_version
        api["expose"].append(clone)
        api["declared_totals"]["expose"] += 1
        problems = self.m.validate(api)
        self.assertTrue(any("declared twice" in p for p in problems), problems)

    def test_an_out_param_missing_from_the_signature_fails(self):
        api = copy.deepcopy(self.api)
        for e in api["expose"]:
            if e["out_params"]:
                e["out_params"][0]["name"] = "not_a_parameter"
                break
        problems = self.m.validate(api)
        self.assertTrue(any("not a parameter of its own c_signature" in p
                            for p in problems), problems)

    def test_a_hand_written_entry_without_a_reason_fails(self):
        api = copy.deepcopy(self.api)
        api["hand_written"][0].pop("reason", None)
        problems = self.m.validate(api)
        self.assertTrue(any("has no `reason`" in p for p in problems), problems)

    def test_a_signature_whose_first_parameter_is_not_user_fails(self):
        api = copy.deepcopy(self.api)
        api["expose"][0]["c_signature"] = "bool (*)(int e)"
        problems = self.m.validate(api)
        self.assertTrue(any("void *user" in p for p in problems), problems)

    def test_the_real_manifest_is_clean(self):
        self.assertEqual(self.m.validate(self.api), [])

    # ── emitter rules ────────────────────────────────────────────────
    def test_every_emitted_forwarder_guards_its_host_member(self):
        """The guard is not optional: open() copies min(host_size, sizeof) over
        a zeroed table, so an absent member is NULL and calling it unguarded
        jumps through whatever followed a shorter caller's object."""
        c = self.m.emit_c(self.api)
        for e in self.api["expose"]:
            self.assertIn("!api || !api->host.%s" % e["vtable"], c,
                          "no guard emitted for " + e["name"])

    def test_bound_arguments_are_not_parameters_of_the_emitted_entry(self):
        """jump_pressed / sprint / attack_pressed are three entries over ONE
        host member.  Passing the button through would make them the same
        function and widen the surface past the scripting surface."""
        bound = [e for e in self.api["expose"] if e.get("bind_args")]
        self.assertTrue(bound, "the manifest no longer exercises bind_args")
        c = self.m.emit_c(self.api)
        for e in bound:
            ret, fn, decl, args = self.m._entry_decl(e)
            for name, value in e["bind_args"].items():
                self.assertNotIn(name, [self.m.param_name(d) for d in decl[1:]],
                                 f"{e['name']} still takes the bound {name}")
                self.assertTrue(any(str(value) in a for a in args),
                                f"{e['name']} does not pass {name}={value}")
            self.assertIn(f"jce_script_api_{e['name']}(JceScriptApi *api)", c)

    def test_absent_values_are_not_the_zero_of_the_type(self):
        """Two entries where zero is a WRONG answer: `tr` degrades to key
        passthrough, and music_request_transition reports a negative playhead
        on a miss."""
        for e in self.api["expose"]:
            av = e.get("absent_value")
            if av is None:
                continue
            ret, _, _, _ = self.m._entry_decl(e)
            line = self.m._absent_return(e, ret)
            if isinstance(av, dict):
                self.assertEqual(line, "return %s;" % av["param"])
            else:
                self.assertNotIn(line, ("return 0;", "return NULL;",
                                        "return 0.0f;", "return false;"))

    def test_a_clamped_return_is_clamped_in_the_emitted_body(self):
        clamped = [e for e in self.api["expose"] if "clamp_min" in e]
        self.assertTrue(clamped, "the manifest no longer exercises clamp_min")
        c = self.m.emit_c(self.api)
        for e in clamped:
            self.assertIn("if (v < %s)" % e["clamp_min"], c)

    def test_owned_strings_are_released_before_returning(self):
        """No free() obligation may cross the ABI: the forwarder copies into
        the caller's buffer and releases the host's string itself."""
        owned = [e for e in self.api["expose"]
                 if e["shape"] == "owned_string_release"]
        self.assertTrue(owned, "the manifest no longer exercises this shape")
        c = self.m.emit_c(self.api)
        for e in owned:
            self.assertIn("api->host.%s(api->host.user, s);" % e["release"], c)

    def test_the_def_and_the_expected_export_table_carry_the_same_names(self):
        """The .def is what the linker obeys; the .gen.h is what the C test
        compares against.  They are emitted from one list and must stay so."""
        names = self.m.export_names(self.api)
        d = self.m.emit_def(self.api)
        h = self.m.emit_exports_h(self.api)
        for n in names:
            self.assertIn("    %s\n" % n, d)
            self.assertIn('    "%s",' % n, h)
        self.assertIn("#define JCE_SCRIPT_API_EXPORT_COUNT %d" % len(names), h)


if __name__ == "__main__":
    unittest.main()
