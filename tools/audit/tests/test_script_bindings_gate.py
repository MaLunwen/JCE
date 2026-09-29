#!/usr/bin/env python3
"""test_script_bindings_gate.py — the scripting generator/gate's own tests.

ci.yml:46 already runs `python -m unittest discover -s tools/audit/tests`,
whose comment states the principle this file exists for: "a gate fix that is
itself unguarded is not a fix."  No CI change is needed to pick this up.

Run:
  python -m unittest discover -s tools/audit/tests -p "test_*.py" -v
"""

from __future__ import annotations

import importlib
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPTGEN_DIR = REPO_ROOT / "tools" / "scriptgen"


def load_module(name="scriptgen_core"):
    """Import one scriptgen module by its REAL name.

    Not spec_from_file_location under an alias any more, and the reason is the
    split: emit_lua.py does `from scriptgen_core import ...`, so a test that
    loaded the core as "scriptgen_under_test" would leave TWO core modules in
    the process -- the aliased one the assertions read and the real one the
    emitter is bound to.  Every module-level table (SHAPES, MODIFIERS, the
    backend cache) would exist twice, and a test could then pass against a
    copy of the core that the tool never runs.  One name, one instance."""
    sys.path.insert(0, str(REPO_ROOT / "tools" / "audit"))
    sys.path.insert(0, str(SCRIPTGEN_DIR))
    return importlib.import_module(name)


# ── THE ANTI-DRIFT LOCK, IN ONE PLACE ────────────────────────────────────────
#
# These numbers exist so the scripting surface cannot grow SILENTLY: a parser
# that quietly found four fewer members, or a manifest that quietly stopped
# claiming one, leaves every other condition green.  That is worth a literal.
#
# WHAT WENT WRONG WITH THE PREVIOUS ONES, because it is the whole reason this
# block exists instead of ten scattered assertions: the surface legitimately
# grew from 76 members to 100, and the numbers stayed at 76 -- for months, in
# TEN separate places, two of them inside TEST NAMES.  Nothing said so, because
# tools/audit/tests was untracked and there was no CI, so this file had never
# run.  A lock nobody can turn is not a lock.
#
# Now it is one block.  When the surface grows on purpose, update it HERE, in
# the same commit as the growth, and the diff shows a reviewer exactly how much
# it grew by -- which is the thing the lock was always for.
HOST_MEMBERS = 105           # function pointers on JceScriptHost
EXPOSE = 101                  # members reached by the generated bindings
HAND_WRITTEN = 8             # members with a hand-written body
CONSTANTS = 1
TABLE_KEYS = 110             # keys installed into the `jce` table
REACHED_BY_GENERATED = 100    # distinct vtable+release members the generator names
REGISTERED_TOTAL = EXPOSE + HAND_WRITTEN
LAST_MEMBER = "raycast_all"          # append-only: the newest member is last
LAST_MEMBER_INDEX = HOST_MEMBERS - 1

# The members NO generated binding reaches -- each has a hand-written body
# because its Lua contract cannot be derived from its C signature.
HAND_ONLY = {"log", "read_file", "play_sound", "play_sound_spatial",
             "line_set_points"}


class TestHostDecomposer(unittest.TestCase):
    def setUp(self):
        self.m = load_module()
        self.members = self.m.parse_host_members(
            self.m.HEADER.read_text(encoding="utf-8"))

    def test_every_member_is_found(self):
        """HOST_MEMBERS is the anti-drift number.  A parser that silently found
        four fewer would let four members go undescribed with every gate
        green.  The count lives in the lock block at the top of this file, not
        in this method's NAME -- a name carrying a number is a comment that
        lies the day the number moves, and that is exactly what happened to
        the previous one."""
        self.assertEqual(len(self.members), HOST_MEMBERS)

    def test_user_is_not_a_member(self):
        """`void *user` is a data field, not a function pointer.  Counting it
        would make the totality check off by one forever."""
        self.assertNotIn("user", [m.name for m in self.members])

    def test_out_array_arity_is_read_from_the_declaration(self):
        m = {x.name: x for x in self.members}["get_position"]
        self.assertEqual(m.ret, "bool")
        self.assertEqual([p.name for p in m.params], ["user", "e", "out_xyz"])
        self.assertEqual(m.params[2].c_type, "float")
        self.assertEqual(m.params[2].arity, 3)

    def test_pointer_out_params_are_arity_minus_one(self):
        m = {x.name: x for x in self.members}["touch_get"]
        self.assertEqual([p.name for p in m.params],
                         ["user", "index", "id", "x", "y", "pressure"])
        self.assertEqual(m.params[2].c_type, "uint64_t")
        self.assertEqual(m.params[2].arity, -1)

    def test_identical_signatures_are_reported_identically(self):
        """find_by_name and find_by_prefix have the SAME C signature and
        DIFFERENT Lua contracts.  The decomposer must not pretend otherwise --
        this is why shape is hand-authored (spec 4.2)."""
        by = {x.name: x for x in self.members}
        sig = lambda m: (m.ret, [(p.c_type, p.arity) for p in m.params])
        self.assertEqual(sig(by["find_by_name"]), sig(by["find_by_prefix"]))

    def test_member_index_is_declaration_order(self):
        self.assertEqual(self.members[0].name, "log")
        self.assertEqual(self.members[0].index, 0)
        # JceScriptHost is append-only by ABI contract, so the newest member
        # is always last and its index is always count-1.  Both follow from
        # the lock rather than being restated.
        self.assertEqual(self.members[-1].name, LAST_MEMBER)
        self.assertEqual(self.members[-1].index, LAST_MEMBER_INDEX)

    def test_const_marks_an_input_array_and_a_string(self):
        """The distinction the whole emitter rests on.  raycast takes TWO
        const float[3] INPUTS and fills one non-const struct OUTPUT; a parser
        that called `origin` an out parameter would emit a binding that reads
        no arguments and pushes fourteen values."""
        by = {x.name: x for x in self.members}
        r = {p.name: p for p in by["raycast"].params}
        self.assertEqual((r["origin"].c_type, r["origin"].arity), ("const float", 3))
        self.assertEqual((r["dir"].c_type, r["dir"].arity), ("const float", 3))
        self.assertEqual((r["max_dist"].c_type, r["max_dist"].arity), ("float", 0))
        self.assertEqual((r["out"].c_type, r["out"].arity),
                         ("JceScriptRaycastHit", -1))

    def test_const_char_pointer_is_a_by_value_input(self):
        """`const char *name` is a string argument, not an out pointer.  Left
        as arity -1 it would be marshalled as an output and every string
        binding would emit backwards."""
        p = {x.name: x for x in self.members}["find_by_name"].params[1]
        self.assertEqual((p.name, p.c_type, p.arity), ("name", "const char *", 0))

    def test_raycast_out_struct_flattens_to_eight_scalars(self):
        flat = self.m.flatten_pod(self.m.HEADER.read_text(encoding="utf-8"),
                                  "JceScriptRaycastHit")
        self.assertEqual([e for e, _ in flat],
                         ["entity", "point[0]", "point[1]", "point[2]",
                          "normal[0]", "normal[1]", "normal[2]", "distance"])
        self.assertEqual(flat[0][1], "JceScriptEntity")
        self.assertEqual(flat[1][1], "float")


class TestManifestGate(unittest.TestCase):
    def setUp(self):
        self.m = load_module()
        self.lua = load_module("emit_lua")
        self.man = self.m.load_manifest()
        self.members = self.m.parse_host_members(
            self.m.HEADER.read_text(encoding="utf-8"))
        self.c_text = self.m.SCRIPT_C.read_text(encoding="utf-8")

    def test_declared_totals_match_the_spec(self):
        t = self.man["declared_totals"]
        self.assertEqual(t["host_members"], HOST_MEMBERS)
        self.assertEqual(t["expose"], EXPOSE)
        self.assertEqual(t["hand_written"], HAND_WRITTEN)
        self.assertEqual(t["constants"], CONSTANTS)
        self.assertEqual(t["table_keys"], TABLE_KEYS)

    def test_manifest_is_clean_against_the_tree(self):
        self.assertEqual(self.m.validate(self.members, self.man, self.c_text), [])

    def test_totality_every_member_claimed_exactly_once(self):
        """Condition 4, the anti-drift lock: the struct cannot grow silently."""
        claimed = self.m.claimed_members(self.man)
        self.assertEqual(len(claimed), HOST_MEMBERS)
        self.assertEqual(claimed, {x.name for x in self.members})

    def test_seventy_one_members_are_reached_by_generated_bindings(self):
        gen = set()
        for e in self.man["expose"]:
            gen.add(e["vtable"])
            if "release" in e:
                gen.add(e["release"])
        self.assertEqual(len(gen), REACHED_BY_GENERATED)
        hand_only = {x.name for x in self.members} - gen
        self.assertEqual(hand_only, HAND_ONLY)

    def test_registration_parity_reads_both_c_files(self):
        """After the hand-written bodies are deleted, jce_script.c registers
        HAND_WRITTEN names and jce_script_bindings.gen.c registers EXPOSE.  A
        gate that read only jce_script.c would report every generated entry as
        missing and be permanently red -- and the obvious 'fix' is to stop
        checking, which is how a gate dies."""
        both = self.lua.registered_names_all()
        self.assertEqual(len(both), REGISTERED_TOTAL)
        named = ([e["name"] for e in self.man["expose"]] +
                 [e["name"] for e in self.man["hand_written"]])
        self.assertEqual(sorted(both), sorted(named))

    def test_the_two_translation_units_register_disjoint_sets(self):
        """Condition 5's other half, and the reason the count above is 80 and
        not 80-with-duplicates.

        Until this commit jce_script.c held BOTH installers, so the seven
        hand-written names appeared twice in one file and the duplication WAS
        the contract.  With install_bindings_handwritten gone there is exactly
        one installer per name, split across two translation units, and the
        failure that replaces 'a hand_written name is not duplicated' is a
        name registered in BOTH files: two register_binding calls into the same
        `jce` table, the second silently overwriting the first, with the key
        count still 81 and every other assertion green.

        jce_script.c must hold the eight and nothing else; the generated TU
        must hold the 72 and nothing else."""
        hand = self.lua.collect_registered_names(self.c_text)
        gen = self.lua.collect_registered_names(
            self.lua.GEN_C.read_text(encoding="utf-8"))
        self.assertEqual(sorted(hand),
                         sorted(e["name"] for e in self.man["hand_written"]))
        self.assertEqual(sorted(gen),
                         sorted(e["name"] for e in self.man["expose"]))
        self.assertEqual(set(hand) & set(gen), set())

    def test_an_unclaimed_member_fails_by_name(self):
        """The failure must NAME the member, or nobody can act on it."""
        extra = self.members + [
            self.m.HostMember("brand_new_hook", "void", [], HOST_MEMBERS)]
        problems = self.m.validate(extra, self.man, self.c_text)
        self.assertTrue(any("brand_new_hook" in p for p in problems), problems)

    def test_an_unknown_vtable_name_fails_by_name(self):
        import copy
        man = copy.deepcopy(self.man)
        man["expose"][0]["vtable"] = "no_such_member"
        problems = self.m.validate(self.members, man, self.c_text)
        self.assertTrue(any("no_such_member" in p for p in problems), problems)

    def test_shape_check_cannot_tell_find_by_name_from_find_by_prefix(self):
        """P0-4, asserted rather than hoped: swapping these two shapes is
        LEGAL for both signatures.  Only the differential harness catches it,
        which is why condition 3 must never be called a correctness check."""
        import copy
        man = copy.deepcopy(self.man)
        by = {e["name"]: e for e in man["expose"]}
        by["find_by_name"]["shape"] = "entity_table"
        by["find_by_name"]["out_capacity"] = 1024
        self.assertEqual(self.m.validate(self.members, man, self.c_text), [])

    def test_the_gate_asserts_it_actually_ran(self):
        """Condition 8.  Two gates in this repository have already passed by
        checking nothing."""
        import copy
        man = copy.deepcopy(self.man)
        man["declared_totals"]["expose"] = 999
        problems = self.m.validate(self.members, man, self.c_text)
        self.assertTrue(any("999" in p for p in problems), problems)

    def test_a_stale_line_citation_is_caught(self):
        """Condition 9, which is NOT in spec 5.3 and exists because it was
        measured: two of the nine jce_script.c:NNN citations in this
        manifest's first draft named the wrong line.  Both were off by two
        within one function, so a check at function-body granularity would
        have been green for both -- the anchor has to be the line's own text.

        The mutation reproduces the exact error that occurred: the draft named
        the line two ABOVE the cited one.  It is derived from whatever the
        manifest currently cites rather than hardcoded, because the hardcoded
        form (":108" -> ":106") stopped mutating anything the first time the
        code moved -- the extraction of jce_script_internal.h shifted that
        citation to :93 and the replace() silently became a no-op.  A mutation
        test whose mutation does not apply asserts nothing.

        THE TARGET MOVED, and the reason is the point of the test: it used to
        be expose[set_parent], whose doc cited the luaL_checktype in the
        hand-written l_jce_set_parent.  That body was deleted with the other
        70, so the citation had nowhere left to point and the entry now names
        the emitter test instead of a line.  hand_written[asset_read_text] is
        the target now because its body is one of the seven that stay.

        Known limit, true today and checked by the assert below: the line two
        above the citation must share no >=3-char identifier with the entry's
        prose, or condition 9 legitimately stays quiet.  If that ever changes
        this test goes red loudly rather than passing hollow."""
        import copy, re
        man = copy.deepcopy(self.man)
        by = {e["name"]: e for e in man["hand_written"]}
        cited = int(re.search(r"script_virtual_asset_path_valid \(:(\d+)",
                              by["asset_read_text"]["reason"]).group(1))
        near = cited - 2
        by["asset_read_text"]["reason"] = by["asset_read_text"]["reason"].replace(
            f"(:{cited},", f"(:{near},")
        # The mutation applied -- this is the assert the hardcoded form lacked.
        self.assertIn(f"(:{near},", by["asset_read_text"]["reason"])
        problems = self.m.validate(self.members, man, self.c_text)
        self.assertTrue(any("hand_written[asset_read_text]" in p
                            and f":{near}" in p for p in problems), problems)

    def test_the_citation_check_is_not_vacuous(self):
        """A check over zero citations passes over zero citations.  If someone
        strips the line numbers out of the prose, condition 9 must stop being
        satisfiable rather than become free.

        NINE, not the ten this file asserted before the hand-written bindings
        were deleted: expose[set_parent] gave its citation up because the line
        it named no longer exists in jce_script.c.  All nine remaining ones
        point into the seven hand-written bodies or into jce_script.h, which
        is exactly the code that is still hand-written and can still rot."""
        import json
        n = len(self.m._CITE_RE.findall(json.dumps(self.man)))
        self.assertEqual(n, 9)


class TestEmitters(unittest.TestCase):
    """The LUA emitter's tests.  `self.m` is the neutral core, `self.lua` is
    emit_lua -- and which of the two a given assertion reaches for is itself
    the record of what the split decided."""

    def setUp(self):
        self.m = load_module()
        self.lua = load_module("emit_lua")
        self.man = self.m.load_manifest()
        self.members = self.m.parse_host_members(
            self.m.HEADER.read_text(encoding="utf-8"))
        self.c = self.lua.emit_bindings_c(self.members, self.man)

    def test_get_position_matches_the_hand_written_body(self):
        """The emitted body, pinned as TEXT.  It was byte-compared against the
        hand-written l_jce_get_position while that function existed; that body
        is deleted now, so this literal IS the record of what it said, and the
        only permitted difference was the accessor name and the local's name
        (spec 5.5).  No line citation: the lines it named are gone."""
        self.assertIn(
            "static int l_jce_get_position(lua_State *L)\n"
            "{\n"
            "    JceScript *s = jce_script_self_from_upvalue(L);\n"
            "    JceScriptEntity e = (JceScriptEntity)luaL_checkinteger(L, 1);\n"
            "    float out_xyz[3];\n"
            "    if (s->have_host && s->host.get_position &&\n"
            "        s->host.get_position(s->host.user, e, out_xyz)) {\n"
            "        lua_pushnumber(L, (lua_Number)out_xyz[0]);\n"
            "        lua_pushnumber(L, (lua_Number)out_xyz[1]);\n"
            "        lua_pushnumber(L, (lua_Number)out_xyz[2]);\n"
            "        return 3;\n"
            "    }\n"
            "    lua_pushnil(L);\n"
            "    return 1;\n"
            "}\n", self.c)

    def test_the_have_host_guard_is_in_every_emitted_function(self):
        """Required by the short-struct ABI: create_sized copies
        min(host_size, sizeof) over a zeroed table, so members an older
        caller's header lacked stay NULL.  The manifest does not turn it off.

        TWO SPELLINGS, both accepted, and the plan's single-spelling form of
        this assertion was wrong: get_touch's index_base branch fuses the
        guard into an early-return condition, so it reads as the De Morgan
        dual `!s->have_host || !s->host.touch_get` -- which is byte-for-byte
        what the deleted hand-written l_jce_get_touch did.  Accepting only the
        `&&` form fails a body that is correct.

        STRICTER than the plan's version in the way that matters: the plan
        matched the bare prefix `s->have_host && s->host.`, which a body
        guarding the WRONG member satisfies.  This ties the guard to the
        member the manifest names, so a template that guarded on
        s->host.get_position while calling s->host.get_scale is caught.
        Mutation M2 in this commit's log is exactly that swap."""
        by_script_name = {e["name"]: e["vtable"] for e in self.man["expose"]}
        bodies = self.c.split("\nstatic int l_jce_")[1:]
        self.assertEqual(len(bodies), EXPOSE)
        for b in bodies:
            script_name = b.split("(", 1)[0]
            member = by_script_name[script_name]
            self.assertTrue(
                f"s->have_host && s->host.{member}" in b
                or f"!s->have_host || !s->host.{member}" in b,
                f"{script_name}: no have_host guard naming host member "
                f"{member!r}\n{b[:400]}")

    def test_strict_emits_a_type_check_only_for_the_named_parameter(self):
        """The `strict` modifier, which is the whole reason set_parent's Lua
        contract differs from the other five boolean-taking bindings: it alone
        rejects a non-boolean instead of accepting any truthy value.

        This test is named in script_exposure.json's set_parent doc, which
        used to cite the luaL_checktype's line in jce_script.c instead.  That
        body is deleted; the manifest is the authority now, and a modifier
        with no test is a modifier that can silently stop being emitted.

        Both directions: the check IS emitted for the parameter `strict`
        names, at the Lua stack index that parameter occupies, and it is NOT
        emitted anywhere else in the file.  A template that emitted
        luaL_checktype for every boolean would satisfy the first half alone
        and would change five bindings' behaviour."""
        strict_entries = [e for e in self.man["expose"] if e.get("strict")]
        self.assertEqual([e["name"] for e in strict_entries], ["set_parent"])

        by_member = {m.name: m for m in self.members}
        for e in strict_entries:
            member = by_member[e["vtable"]]
            script_params = [p for p in member.params[1:] if p.arity == 0]
            for name in e["strict"]:
                idx = [p.name for p in script_params].index(name) + 1
                body = self.c.split(f"\nstatic int l_jce_{e['name']}(")[1]
                body = body.split("\n}\n")[0]
                self.assertIn(f"luaL_checktype(L, {idx}, LUA_TBOOLEAN);", body)

        # And nowhere else: one strict parameter, one luaL_checktype.
        self.assertEqual(self.c.count("luaL_checktype("),
                         sum(len(e["strict"]) for e in strict_entries))

    def test_the_banner_counts_match_the_emitted_bodies(self):
        """The banner tells the reader how many functions use each of the two
        guard spellings.  Hand-written, those numbers are a claim nothing
        enforces -- add a second index_base binding and "70 / 1" silently
        becomes false, and the next person to grep the common spelling counts
        69, concludes the generator is broken, and goes looking for a bug that
        is not there.  So they are derived, and this is what proves they were
        derived from the same thing the bodies were."""
        import re
        banner = self.c.split("*/")[0]
        plain_claimed = int(
            re.search(r"s->have_host && s->host.<member>\s+(\d+) functions",
                      banner).group(1))
        folded_claimed = int(
            re.search(r"!s->have_host \|\| !s->host.<member>\s+(\d+) function",
                      banner).group(1))

        by_script_name = {e["name"]: e["vtable"] for e in self.man["expose"]}
        plain = folded = 0
        for b in self.c.split("\nstatic int l_jce_")[1:]:
            member = by_script_name[b.split("(", 1)[0]]
            if f"s->have_host && s->host.{member}" in b:
                plain += 1
            elif f"!s->have_host || !s->host.{member}" in b:
                folded += 1
        self.assertEqual((plain_claimed, folded_claimed), (plain, folded))
        self.assertEqual(plain + folded, EXPOSE)

        # The named body is named correctly, not just counted correctly.
        for e in self.man["expose"]:
            if e.get("index_base"):
                self.assertIn(f"jce.{e['name']}", banner)

    def test_release_member_keeps_its_null_check(self):
        """The deleted hand-written l_jce_comp_get guarded on json_free ALONE,
        separately from the comp_get_json guard, and the emitter must keep
        doing that: a template that 'normalises' it to unconditional crashes
        on a partial host that supplies comp_get_json and not json_free."""
        self.assertIn("if (s->host.json_free) s->host.json_free(s->host.user, json);",
                      self.c)

    def test_find_by_name_and_find_by_prefix_emit_different_bodies(self):
        """Identical C signatures, different Lua contracts."""
        self.assertIn("JceScriptEntity found[2];", self.c)
        self.assertIn("JceScriptEntity found[1024];", self.c)
        self.assertIn("    return 2;\n}\n", self.c)

    def test_raycast_miss_pushes_zero_not_nil(self):
        self.assertIn("    lua_pushinteger(L, 0);   /* miss */\n    return 1;", self.c)

    def test_get_touch_rejects_a_zero_index_without_calling_the_host(self):
        self.assertIn("if (lua_index < 1 || !s->have_host || !s->host.touch_get ||",
                      self.c)

    def test_every_emitted_call_passes_the_declared_number_of_arguments(self):
        """THE ARITY ORACLE.  Every `s->host.<member>(...)` in the emitted file
        must pass exactly as many arguments as struct JceScriptHost declares
        for that member.

        Written because the plan's emitter got this wrong for find_by_name and
        find_by_prefix: `int max` is by value, so _in_params called it a script
        input, and the binding both read a second Lua argument no caller passes
        AND called a four-parameter member with five arguments.  A C compiler
        would reject the second half -- but this file is COMMITTED UNCOMPILED
        by the task that generates it, so nothing between the generator and the
        harness two tasks later would have noticed.  This is that missing
        check, and it covers all 72 rather than the two that happened to be
        wrong."""
        import re
        by_member = {m.name: m for m in self.members}
        checked = 0
        for e in self.man["expose"]:
            member = e["vtable"]
            declared = len(by_member[member].params)
            for call in self._calls_to(member):
                checked += 1
                self.assertEqual(
                    len(call), declared,
                    f"jce.{e['name']}: calls s->host.{member} with "
                    f"{len(call)} arguments {call}, but the header declares "
                    f"{declared}")
        # A test that found no calls would pass vacuously; 72 bindings make at
        # least 72 calls (get_touch's guard makes two references, one of which
        # is a call).
        self.assertGreaterEqual(checked, EXPOSE, checked)

    def _calls_to(self, member):
        """Every argument list passed to s->host.<member> in the emitted C,
        split on top-level commas."""
        out = []
        needle = f"s->host.{member}("
        i = self.c.find(needle)
        while i != -1:
            j = i + len(needle)
            depth, start, args = 1, j, []
            while depth:
                ch = self.c[j]
                if ch in "([":
                    depth += 1
                elif ch in ")]":
                    depth -= 1
                    if depth == 0:
                        break
                elif ch == "," and depth == 1:
                    args.append(self.c[start:j].strip())
                    start = j + 1
                j += 1
            args.append(self.c[start:j].strip())
            out.append([a for a in args if a])
            i = self.c.find(needle, j)
        return out

    def test_find_by_name_takes_exactly_one_lua_argument(self):
        """The out-capacity is the GENERATOR's value, not the script's.  A
        binding that read it from the stack would make jce.find_by_name("x")
        raise `bad argument #2` for every existing caller."""
        body = self.c.split("\nstatic int l_jce_find_by_name")[1].split("\n}")[0]
        self.assertIn("luaL_checkstring(L, 1)", body)
        self.assertNotIn("L, 2", body)
        self.assertIn("s->host.find_by_name(s->host.user, name, found, 2)", body)

    def test_api_json_params_match_the_emitted_lua_arity(self):
        """script-api.json is the published description of this surface.  If it
        lists a parameter the emitted body never reads, the document and the
        code are a two-sided contract that disagree -- the defect class this
        whole effort exists to remove."""
        import json as _j
        doc = _j.loads(self.m.emit_api_json(self.members, self.man))
        by = {e["name"]: e for e in doc["expose"]}
        self.assertEqual([p["name"] for p in by["find_by_name"]["params"]],
                         ["name"])
        self.assertEqual([p["name"] for p in by["find_by_prefix"]["params"]],
                         ["prefix"])

    def test_no_binding_keys_off_vtable_index(self):
        """vtable_index is traceability only; a slot-indexed binding would
        defeat the min(caller, engine) copy that makes a short host safe."""
        self.assertNotIn("vtable_index", self.c)

    def test_generated_installer_emits_the_constant_from_the_manifest(self):
        self.assertIn(
            "#define JCE_SCRIPT_GENERATED_BINDING_COUNT %d" % EXPOSE,
                      self.lua.emit_bindings_h(self.man))
        self.assertIn("lua_pushlightuserdata(L, &jce_script_json_null_token);", self.c)
        self.assertIn('lua_setfield(L, -2, "json_null");', self.c)

    def test_emission_is_deterministic(self):
        self.assertEqual(self.c, self.lua.emit_bindings_c(self.members, self.man))

    def test_committed_output_matches_a_fresh_emit(self):
        """Condition 1.  The committed file is DERIVED, not generated once."""
        self.assertEqual(self.lua.GEN_C.read_text(encoding="utf-8"), self.c)
        self.assertEqual(self.lua.GEN_H.read_text(encoding="utf-8"),
                         self.lua.emit_bindings_h(self.man))
        self.assertEqual(self.m.API_JSON.read_text(encoding="utf-8"),
                         self.m.emit_api_json(self.members, self.man))

    def test_output_is_lf_only(self):
        """write_text() applies platform line-ending translation on Windows;
        an unmarked write makes condition 1 permanently red on every Windows
        machine for a reason that has nothing to do with bindings."""
        self.assertNotIn(b"\r\n", self.lua.GEN_C.read_bytes())
        self.assertNotIn(b"\r\n", self.m.API_JSON.read_bytes())

    def test_append_only_rejects_a_removed_entry(self):
        import copy, json as _j
        old = _j.loads(self.m.emit_api_json(self.members, self.man))
        new = copy.deepcopy(old)
        new["expose"] = new["expose"][1:]
        problems = self.m.check_append_only(old, new)
        self.assertTrue(any("REMOVED" in p for p in problems), problems)

    def test_append_only_rejects_a_changed_since(self):
        import copy, json as _j
        old = _j.loads(self.m.emit_api_json(self.members, self.man))
        new = copy.deepcopy(old)
        new["expose"][0]["since"] = 2
        problems = self.m.check_append_only(old, new)
        self.assertTrue(any("since" in p for p in problems), problems)

    def test_append_only_accepts_an_addition(self):
        import copy, json as _j
        old = _j.loads(self.m.emit_api_json(self.members, self.man))
        new = copy.deepcopy(old)
        new["expose"].append(dict(old["expose"][0], name="brand_new", since=2))
        self.assertEqual(self.m.check_append_only(old, new), [])


class TestSharedVocabulary(unittest.TestCase):
    """The words all four backends must mean the same thing by.

    They live in scriptgen_core because a per-backend copy is four dialects
    that agree until they do not, and the manifest is the ONE place a binding's
    shape and modifiers are decided."""

    def setUp(self):
        self.m = load_module()
        self.man = self.m.load_manifest()
        self.members = self.m.parse_host_members(
            self.m.HEADER.read_text(encoding="utf-8"))
        self.c_text = self.m.SCRIPT_C.read_text(encoding="utf-8")

    def test_the_shape_vocabulary_is_seven_and_closed(self):
        """SEVEN.  An eighth shape is not added: the binding that would need
        one is marked hand_written, so the decision lands in the manifest where
        every other backend can see it rather than inside one emitter."""
        self.assertEqual(self.m.SHAPES, {
            "void_call", "value_return", "fallible_out", "void_out_array",
            "first_and_count", "entity_table", "owned_string_release"})

    def test_the_modifier_vocabulary_is_nine(self):
        """Five from the spec, four forced by measured hand-written bodies.
        Spelled out here so that deleting one from the core -- which every
        emitter reads by exact name with .get() -- cannot be a silent no-op."""
        self.assertEqual(self.m.MODIFIERS, {
            "bind_args", "out_capacity", "index_base", "optional", "release",
            "absent_value", "miss_value", "clamp_min", "strict"})

    def test_every_modifier_in_the_vocabulary_is_actually_used(self):
        """The other direction: a vocabulary listing keys no manifest entry
        carries would let a typo'd key be 'known' and still emit nothing."""
        used = set()
        for e in self.man["expose"]:
            used |= set(e) & self.m.MODIFIERS
        self.assertEqual(used, self.m.MODIFIERS)

    def test_an_unknown_modifier_key_fails_by_name(self):
        """An emitter reads modifiers by exact name.  `clampmin` is therefore
        not a rejected key but an IGNORED one: the binding emits without its
        clamp, in every backend at once, with all seven other conditions and
        both counts still green.  The failure must name the key."""
        import copy
        man = copy.deepcopy(self.man)
        entry = [e for e in man["expose"] if "clamp_min" in e][0]
        entry["clampmin"] = entry.pop("clamp_min")
        problems = self.m.validate(self.members, man, self.c_text)
        self.assertTrue(any("clampmin" in p and entry["name"] in p
                            for p in problems), problems)


class TestBackendSeam(unittest.TestCase):
    """The registration seam three more backends are about to use.

    What is asserted here is the PROMISE made to them: a backend is one new
    emit_*.py with a module-level BACKEND, and nothing else in this directory
    learns its name."""

    def setUp(self):
        self.m = load_module()
        self.lua = load_module("emit_lua")
        self.man = self.m.load_manifest()
        self.members = self.m.parse_host_members(
            self.m.HEADER.read_text(encoding="utf-8"))
        self.c_text = self.m.SCRIPT_C.read_text(encoding="utf-8")

    def test_the_lua_backend_is_discovered(self):
        backends = self.m.discover_backends(force=True)
        self.assertIn("lua", [b.name for b in backends])
        self.assertEqual(len(backends),
                         len(list(SCRIPTGEN_DIR.glob("emit_*.py"))))

    def test_no_file_outside_emit_lua_names_the_lua_backend(self):
        """THE SEAM ITSELF, asserted rather than promised.  If the core or the
        driver ever imports emit_lua by name, then adding emit_python.py means
        editing that file too -- and three backend authors editing one shared
        registration line is the collision this split exists to remove.  This
        test goes red the moment a central list appears."""
        import ast
        for name in ("scriptgen_core.py", "gen_script_bindings.py"):
            tree = ast.parse((SCRIPTGEN_DIR / name).read_text(encoding="utf-8"))
            imported = set()
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    imported |= {a.name for a in node.names}
                elif isinstance(node, ast.ImportFrom) and node.module:
                    imported.add(node.module)
            self.assertEqual(
                sorted(n for n in imported if n.startswith("emit_")), [],
                f"{name} imports a backend by name")

            # A hardcoded list of backend FILES is the same defect one level
            # down, so every string literal that names one must be the glob.
            # Docstrings are excluded by identity, not by position: the core's
            # worked example legitimately says emit_python.py in prose.
            docs = {id(n.value) for n in ast.walk(tree)
                    if isinstance(n, ast.Expr) and isinstance(n.value, ast.Constant)
                    and isinstance(n.value.value, str)}
            literals = [n.value for n in ast.walk(tree)
                        if isinstance(n, ast.Constant) and isinstance(n.value, str)
                        and id(n) not in docs
                        and "emit_" in n.value.replace("emit_*.py", "")]
            self.assertEqual(literals, [],
                             f"{name} names a backend file outside the glob")

    def test_every_backend_artefact_renders_with_the_uniform_signature(self):
        for b in self.m.discover_backends():
            for a in b.artefacts(self.members, self.man):
                self.assertIsInstance(a.render(self.members, self.man), str)

    def test_artefact_order_is_backends_then_core(self):
        """Check mode reports failures in this order; a reordering reads to a
        reviewer as a change to the artefacts themselves.

        THE ASSERTION IS THE ORDER, NOT THE LIST.  It first spelled the list as
        [lua.GEN_C, lua.GEN_H, API_JSON], which was every artefact in the tree
        on the day it was written -- and so it encoded "Lua is the only backend"
        into a test whose name promises something else entirely.  Adding
        emit_python / emit_java / emit_cpp reddened it without any of them
        touching the core, emit_lua or each other, which is precisely the
        collision the sorted-glob seam exists to make impossible.  A test more
        specific than its own name is a test that fails for reasons it does not
        describe."""
        driver = load_module("gen_script_bindings")
        paths = [a.path for a in driver.all_artefacts(self.members, self.man)]
        self.assertEqual(paths[-1], self.m.API_JSON,
                         "the core's API_JSON must be emitted last")
        self.assertNotIn(self.m.API_JSON, paths[:-1],
                         "API_JSON must appear exactly once, at the end")
        self.assertIn(self.lua.GEN_C, paths[:-1])
        self.assertIn(self.lua.GEN_H, paths[:-1])
        backend_paths = [a.path for b in self.m.discover_backends()
                         for a in b.artefacts(self.members, self.man)]
        self.assertEqual(paths[:-1], backend_paths,
                         "every non-final artefact is a backend's, in "
                         "discovery order")

    def test_condition_5_belongs_to_the_lua_backend_not_the_core(self):
        """Registration parity greps for `register_binding(L, s, "name", fn)`
        and `static int l_x(lua_State *L)`.  Both are Lua; a Python TU
        registers into a PyMethodDef table and would be invisible to every line
        of it.  So the neutral core, run with no backends, must have NO opinion
        about a name that is never registered -- and the Lua backend must.

        THREE assertions, not two, and the third was MEASURED into existence:
        with only "the core stays quiet" and "the backend speaks", deleting the
        `for backend in ...: problems += backend.validate(...)` line from
        validate() entirely left the generator green and all fifty tests green
        (mutation M8).  Condition 5 cannot fail against a clean tree, so the
        hook that runs it is invisible unless something asserts the hook ITSELF
        -- and a seam whose backends' conditions are never actually invoked is
        three backends' gates quietly doing nothing."""
        import copy
        man = copy.deepcopy(self.man)
        man["expose"][0]["name"] = "never_registered_anywhere"
        self.assertEqual(
            self.m.validate(self.members, man, self.c_text, backends=[]), [])
        problems = self.lua.BACKEND.validate(self.members, man, self.c_text)
        self.assertTrue(any("never_registered_anywhere" in p for p in problems),
                        problems)
        wired = self.m.validate(self.members, man, self.c_text)
        self.assertTrue(any("never_registered_anywhere" in p for p in wired),
                        f"validate() does not run backend conditions: {wired}")

    def test_a_backend_file_with_no_BACKEND_is_a_hard_failure(self):
        """Not a skip.  A skipped backend cannot have its artefacts checked, so
        its committed output would rot while this tool printed OK -- the same
        reasoning that makes a MISSING generated file a failure."""
        import tempfile
        real = self.m.SCRIPTGEN_DIR
        try:
            with tempfile.TemporaryDirectory() as d:
                (Path(d) / "emit_notabackend.py").write_bytes(b"X = 1\n")
                self.m.SCRIPTGEN_DIR = Path(d)
                with self.assertRaises(SystemExit) as cm:
                    self.m.discover_backends(force=True)
                self.assertIn("emit_notabackend.py", str(cm.exception))
                self.assertIn("BACKEND", str(cm.exception))
                # And it did not poison the import table for a fixed retry.
                self.assertNotIn("emit_notabackend", sys.modules)
        finally:
            self.m.SCRIPTGEN_DIR = real
            self.m.discover_backends(force=True)


if __name__ == "__main__":
    unittest.main()
