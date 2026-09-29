#!/usr/bin/env python3
"""test_emit_python.py — the Python backend's STATIC gate.

Four properties that the cross-language differential cannot reach, because it
compares two RUNNING surfaces and these are facts about the emitted files and
about the backend's own validator.  Everything else about this binding is
accepted by `differential.py`, which is the real oracle; this file exists only
where that oracle is structurally blind:

  * `_generated.pyi` is never imported at runtime, so a stub that disagreed
    with the module would pass every differential case ever written — and
    byte-identity against a fresh emit cannot see it either, because BOTH files
    are emitted from the same run: `emit_stub` and `emit_module` drifting apart
    keeps both artefacts "fresh".  Comparing the two artefacts TO EACH OTHER is
    the only thing that fails.
  * the optional-argument ordering rule is a property of the SIGNATURE.
    Breaking it makes `_generated.py` a SyntaxError, and an import that dies
    reports less than a red assertion — the crash-reports-less-than-a-red-test
    hazard this repository has measured three times.
  * `PythonBackend.validate()` cannot fail against a clean tree, so nothing
    would notice it had stopped checking (the same hole the seam's own M8
    mutation found in the Lua backend's condition 5).
  * "no C compilation, pip install and it works" is the transport decision this
    backend was chosen for, and it was written in three comments and enforced
    by none.

These four names are the ones `tools/scriptgen/emit_python.py` cites.  Every
other "what fails when this is violated" in that file names a `check_*` inside
`differential.py`, which runs as `test_jce_script_python_differential`.

Run: python tests/scripting/python/test_emit_python.py
ctest: test_jce_script_python_emitter (no build required — pure Python)
"""

from __future__ import annotations

import ast
import json
import re
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools" / "scriptgen"))

import scriptgen_core as core                                # noqa: E402
import emit_python as ep                                     # noqa: E402

GEN_PY = REPO / "scripting/python/jce_script/_generated.py"
GEN_PYI = REPO / "scripting/python/jce_script/_generated.pyi"
INIT_PY = REPO / "scripting/python/jce_script/__init__.py"
CMAKE = REPO / "scripting/python/CMakeLists.txt"
PYPROJECT = REPO / "scripting/python/pyproject.toml"
API_JSON = REPO / "contracts/script-api.json"


def _api_json() -> dict:
    return json.loads(API_JSON.read_text(encoding="utf-8"))


def _classes(tree: ast.Module) -> dict[str, ast.ClassDef]:
    return {n.name: n for n in tree.body if isinstance(n, ast.ClassDef)}


def _methods(cls: ast.ClassDef) -> dict[str, ast.FunctionDef]:
    return {n.name: n for n in cls.body if isinstance(n, ast.FunctionDef)}


def _sig(fn: ast.FunctionDef) -> tuple:
    """(param names, per-param annotation text, has-default flags, return)."""
    args = fn.args.args
    pad = [None] * (len(args) - len(fn.args.defaults)) + list(fn.args.defaults)
    return (
        tuple(a.arg for a in args),
        tuple(ast.unparse(a.annotation) if a.annotation else None
              for a in args),
        tuple(d is not None for d in pad),
        ast.unparse(fn.returns) if fn.returns else None,
    )


class TestEmittedArtefacts(unittest.TestCase):
    """The two emitted Python files, checked against EACH OTHER."""

    @classmethod
    def setUpClass(cls) -> None:
        # PARSED IN setUp, NOT setUpClass-with-a-raise, and that is measured
        # rather than stylistic.  Breaking the optional-ordering rule in
        # emit_python.default_for emits a `_generated.py` that is a SyntaxError:
        # raising here aborts the whole class and prints `ERROR: setUpClass`,
        # which is the crash-reports-less-than-a-red-test shape this repository
        # has hit three times.  Held instead, and re-raised BY NAME from the
        # test whose rule it is (mutation M12 is what proved the difference).
        cls.mod = cls.stub = None
        cls.parse_error = None
        try:
            cls.mod = ast.parse(GEN_PY.read_text(encoding="utf-8"),
                                filename=str(GEN_PY))
            # A .pyi is Python grammar; ast.parse is also what makes a stub
            # with a defaulted argument before a non-defaulted one a failure
            # HERE rather than inside somebody's type checker.
            cls.stub = ast.parse(GEN_PYI.read_text(encoding="utf-8"),
                                 filename=str(GEN_PYI))
        except SyntaxError as exc:                          # noqa: PERF203
            cls.parse_error = exc

    def _parsed(self) -> None:
        if self.parse_error is not None:
            self.fail(
                f"{Path(self.parse_error.filename or '?').name} does not "
                f"parse: {self.parse_error.msg} at line "
                f"{self.parse_error.lineno}. The likeliest cause is the "
                f"optional-argument ordering rule: Python forbids a defaulted "
                f"parameter before a non-defaulted one, which is what "
                f"emit_python.default_for's every-later-parameter-optional "
                f"check exists to prevent.")

    def test_stub_declares_every_public_name(self) -> None:
        """Everything `_generated.py` publishes is declared in the stub, with
        the SAME signature.

        A stub SHADOWS its module for every type checker, so a name the stub
        omits is invisible to the editor completion that is most of this
        binding's value, and a signature the stub gets wrong is worse than no
        stub at all.  Byte-identity against a fresh emit cannot catch either:
        `emit_module` and `emit_stub` are two functions, and a change made to
        one alone re-emits both files happily."""
        self._parsed()
        mod_names = {n.name for n in self.mod.body
                     if isinstance(n, (ast.ClassDef, ast.FunctionDef))}
        mod_names |= {t.id for n in self.mod.body
                      if isinstance(n, ast.Assign)
                      for t in n.targets if isinstance(t, ast.Name)}
        stub_names = {n.name for n in self.stub.body
                      if isinstance(n, (ast.ClassDef, ast.FunctionDef))}
        stub_names |= {n.target.id for n in self.stub.body
                       if isinstance(n, ast.AnnAssign)
                       and isinstance(n.target, ast.Name)}

        published = set(json.loads(
            "[" + ast.unparse(next(
                n.value for n in self.mod.body if isinstance(n, ast.Assign)
                and any(getattr(t, "id", "") == "__all__" for t in n.targets)
            ))[1:-1].replace("'", '"') + "]"))
        # Everything jce_script/__init__.py re-exports counts as published too:
        # `_Entries` is underscored and is still part of the contract, because
        # open_host() constructs one.
        imported = {a.name for n in ast.parse(
            INIT_PY.read_text(encoding="utf-8")).body
            if isinstance(n, ast.ImportFrom) and n.module == "_generated"
            for a in n.names}

        for name in sorted(published | imported):
            with self.subTest(name=name):
                self.assertIn(name, mod_names,
                              f"{name} is exported but not defined in "
                              f"_generated.py")
                self.assertIn(name, stub_names,
                              f"{name} is public in _generated.py but absent "
                              f"from _generated.pyi — a stub shadows its "
                              f"module, so the name would be unknown to every "
                              f"type checker")

        mod_api, stub_api = _classes(self.mod)["Api"], _classes(self.stub)["Api"]
        mod_m, stub_m = _methods(mod_api), _methods(stub_api)
        self.assertEqual(sorted(mod_m), sorted(stub_m),
                         "Api's methods differ between _generated.py and "
                         "_generated.pyi")
        entry_count = len(_api_json()["expose"])
        # + __init__ and the `handle` property, which are the class's own two
        # non-entry members.  Stated as a number so an entry that stopped being
        # emitted cannot hide behind "the two lists agree with each other".
        self.assertEqual(len(mod_m), entry_count + 2,
                         f"Api carries {len(mod_m)} methods, the manifest "
                         f"exposes {entry_count} entries plus __init__ and "
                         f"`handle`")
        for name in sorted(mod_m):
            with self.subTest(method=name):
                self.assertEqual(
                    _sig(mod_m[name]), _sig(stub_m[name]),
                    f"Api.{name} has a different signature in the stub than "
                    f"in the module")

    def test_optional_before_a_required_parameter_has_no_default(self) -> None:
        """An optional argument gets a Python default ONLY when every argument
        after it is optional too.

        Derived from `script-api.json`, never from the emitter: the rule is a
        property of the published contract, and reading it back out of
        `emit_python.default_for` would be the self-consistency this campaign
        has caught three times.  `gas_apply` is the case that forces it — its
        optional `op` is followed by a REQUIRED `magnitude`, and Python forbids
        a defaulted parameter before a non-defaulted one.  The Lua spelling of
        the same call is nil IN PLACE, which is why `op` still accepts None."""
        self._parsed()
        api = _api_json()
        mod_m = _methods(_classes(self.mod)["Api"])
        checked = 0
        for e in api["expose"]:
            bind = e.get("bind_args") or {}
            opt = e.get("optional") or {}
            params = [p for p in e["params"] if p["name"] not in bind]
            fn = mod_m[ep.py_name(e["name"])]
            names, _anns, defaults, _ret = _sig(fn)
            self.assertEqual(names[0], "self")
            self.assertEqual([ep.py_name(p["name"]) for p in params],
                             list(names[1:]),
                             f"{e['name']}: the emitted parameter list does "
                             f"not match script-api.json")
            for i, p in enumerate(params):
                tail_all_optional = all(q["name"] in opt
                                        for q in params[i + 1:])
                want = p["name"] in opt and tail_all_optional
                self.assertEqual(
                    defaults[i + 1], want,
                    f"{e['name']}.{p['name']}: default present="
                    f"{defaults[i + 1]}, contract says {want} "
                    f"(optional={p['name'] in opt}, every later parameter "
                    f"optional={tail_all_optional})")
                checked += 1
        self.assertGreater(checked, 100,
                           "the rule was checked over too few parameters to "
                           "be checking anything")
        # The forcing case, named so a manifest that stopped producing it is
        # visible rather than silently making this test vacuous.
        _n, _a, d, _r = _sig(mod_m["gas_apply"])
        self.assertEqual(d, (False, False, False, False, False, True),
                         "gas_apply no longer has an optional `op` followed by "
                         "a required `magnitude` — the case this rule exists "
                         "for is gone, so the rule is now untested by example")

    def test_the_wheel_needs_no_compiler(self) -> None:
        """`pip install jce-script` needs no compiler — and THAT is the claim.

        THIS TEST WAS SHARPENED WHEN THE VM SHIM LANDED, AND THE REASON IS
        MEASURED RATHER THAN CONVENIENT.  It used to forbid `add_library(` in
        scripting/python/CMakeLists.txt outright.  That was a PROXY for the
        real property, and the proxy became wrong: the call-UP direction
        (`JceScriptVM`) cannot be pure Python, because the vtable is C function
        pointers the ENGINE calls and there is no interpreter in the engine's
        process until C calls `Py_InitializeFromConfig`.  The shim is therefore
        structurally required, and a rule that forbade it would have been
        satisfied by moving the target one directory down — the shape of a gate
        that reports green where nobody looks.

        So the rule names the property instead of a spelling:

          * the WHEEL is `py3-none-any` — no extension module, no dependency,
            and no compiled target anywhere under scripting/python may name a
            file inside `jce_script/`;
          * `_generated.py` imports only the standard library;
          * exactly ONE compiled target exists here, it is the VM shim, and its
            sources live under `src/`.

        A cffi rewrite, a vendored accelerator inside the package, or a second
        C target all fail this, by name."""
        self._parsed()
        # Comments stripped on BOTH files: every one of these phrases appears
        # in prose explaining why it is absent, so a substring search over the
        # raw text would fail on the explanation rather than on the thing.
        # Measured, not guessed — it is how this test first went red.
        def code_of(path: Path) -> str:
            return "\n".join(ln.split("#", 1)[0]
                             for ln in path.read_text(encoding="utf-8")
                                            .splitlines())

        cmakes = sorted((REPO / "scripting/python").rglob("CMakeLists.txt"))
        self.assertTrue(cmakes, "scripting/python has no CMakeLists.txt")
        targets = []
        for cm in cmakes:
            code = code_of(cm)
            for m in re.finditer(r"add_(library|executable)\s*\(([^)]*)\)",
                                 code, re.S):
                words = m.group(2).split()
                name = words[0] if words else "<unnamed>"
                sources = [w for w in words[1:]
                           if w not in ("STATIC", "SHARED", "MODULE",
                                        "INTERFACE", "OBJECT", "WIN32",
                                        "MACOSX_BUNDLE", "EXCLUDE_FROM_ALL")]
                targets.append((name, " ".join(sources)))
                for src in sources:
                    self.assertNotIn(
                        "jce_script/", src,
                        f"{cm.relative_to(REPO)} compiles {src}, which is "
                        f"inside the wheel. The wheel must stay py3-none-any: "
                        f"a compiled file in the package makes `pip install` "
                        f"need a toolchain, which is the transport decision "
                        f"in emit_python.py's docstring")

        self.assertEqual(
            [t[0] for t in targets], ["jce_script_vm_python"],
            f"scripting/python declares compiled target(s) "
            f"{[t[0] for t in targets]}. Exactly one is expected — the VM "
            f"shim — and a second means either the binding grew a compiler or "
            f"a target appeared without this rule being revisited")
        self.assertNotIn("jce_script/", targets[0][1],
                         "the VM shim compiles a file from inside the wheel")
        self.assertIn("src/", targets[0][1],
                      "the VM shim's sources are expected under src/")

        proj = code_of(PYPROJECT)
        self.assertIn("dependencies = []", proj,
                      "pyproject.toml declares dependencies — a pure-ctypes "
                      "binding has none, and cffi would bring a C extension "
                      "back through the back door")
        self.assertNotIn("ext-modules", proj,
                         "pyproject.toml builds an extension module, so the "
                         "wheel is no longer py3-none-any and `pip install` "
                         "needs a compiler")

        # And the module itself imports only the standard library.
        imported = set()
        for n in ast.walk(self.mod):
            if isinstance(n, ast.Import):
                imported |= {a.name.split(".")[0] for a in n.names}
            elif isinstance(n, ast.ImportFrom) and n.level == 0 and n.module:
                imported.add(n.module.split(".")[0])
        self.assertEqual(imported, {"ctypes", "typing", "__future__"},
                         "_generated.py imports something outside the "
                         "standard library")


class TestScriptVM(unittest.TestCase):
    """The call-UP half: jce_script/vm.py and the C shim beside it.

    Seven properties the lifecycle differential cannot reach.  Each is a
    TWO-FILE AGREEMENT that neither file can be wrong about alone — which is
    exactly what a running comparison is blind to: both halves agree with each
    other, both are wrong, and every case still passes.

    NO BUILD REQUIRED, deliberately.  jce_script_vm_python is skipped on a
    machine with no embeddable CPython, and so is the lifecycle differential.
    If these checks lived there too, the backend would be ungated precisely
    where it cannot be exercised."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.vm_py = (REPO / "scripting/python/jce_script/vm.py").read_text(
            encoding="utf-8")
        cls.shim_h = (REPO / "scripting/python/src/jce_script_vm_python.h"
                      ).read_text(encoding="utf-8")
        cls.shim_c = (REPO / "scripting/python/src/jce_script_vm_python.c"
                      ).read_text(encoding="utf-8")
        cls.tree = ast.parse(cls.vm_py, filename="vm.py")

    def _const(self, name: str):
        """A module-level constant, read from the SOURCE and never imported.

        Importing vm.py would run `from . import open_host`, which pulls in
        ctypes and the whole binding — and this class must work on a machine
        with no jce_script_api built at all.  `frozenset({...})` is unwrapped
        because ast.literal_eval does not evaluate calls."""
        for n in self.tree.body:
            if isinstance(n, ast.Assign) and any(
                    isinstance(t, ast.Name) and t.id == name
                    for t in n.targets):
                node = n.value
                if (isinstance(node, ast.Call)
                        and isinstance(node.func, ast.Name)
                        and node.func.id in ("frozenset", "set", "tuple",
                                             "list")
                        and len(node.args) == 1):
                    node = node.args[0]
                return ast.literal_eval(node)
        self.fail(f"vm.py does not define {name}")

    def test_the_vm_protocol_matches_the_shim(self) -> None:
        """The engine binary and the pip package ship separately.

        A skew is therefore a REAL configuration, not a hypothetical, and
        without this number its symptom is a TypeError raised inside a
        lifecycle handler and blamed on the user's script."""
        m = re.search(r"#define\s+JCE_PY_VM_PROTOCOL\s+(\d+)", self.shim_h)
        self.assertIsNotNone(
            m, "JCE_PY_VM_PROTOCOL is not defined in "
               "scripting/python/src/jce_script_vm_python.h")
        self.assertEqual(
            int(m.group(1)), self._const("PROTOCOL"),
            "jce_script.vm.PROTOCOL and JCE_PY_VM_PROTOCOL disagree. Bump "
            "BOTH in one commit, or a package installed from PyPI and an "
            "engine built from this tree refuse each other at create — which "
            "is the intended behaviour, and this test is what stops it "
            "happening by accident")

    def test_the_coroutine_cap_matches_the_engine(self) -> None:
        """`MAX_COROUTINES` is COPIED from an engine-private header.

        Copied and not imported, because vm.py imports nothing from C — so
        nothing but this test can notice the copy going stale, and a stale copy
        means one runtime schedules coroutines the other silently drops."""
        internal = (REPO / "engine/src/middleware/script/jce_script_internal.h"
                    ).read_text(encoding="utf-8")
        m = re.search(r"#define\s+JCE_SCRIPT_MAX_COROUTINES\s+(\d+)",
                      internal)
        self.assertIsNotNone(m, "JCE_SCRIPT_MAX_COROUTINES is gone from "
                                "jce_script_internal.h")
        self.assertEqual(
            int(m.group(1)), self._const("MAX_COROUTINES"),
            "jce_script.vm.MAX_COROUTINES no longer mirrors the engine's "
            "JCE_SCRIPT_MAX_COROUTINES")

    def test_the_disabled_notice_matches_the_engine(self) -> None:
        """The one error-related line the differentials compare BYTE FOR BYTE.

        THE FAILING-CALLBACK RULE (jce_script.h) requires every backend to
        announce a disable in one published wording.  The error line above it
        cannot be compared across languages — Lua writes `chunk:12: boom` and
        Python writes `RuntimeError: boom` — so this notice is the only part of
        the whole episode that a cross-language differential can compare
        exactly, and it is therefore the only part that catches a backend which
        logged *something* instead of doing the thing.

        vm.py cannot include a C header, so the agreement is this test.  A
        reworded macro that vm.py did not follow would otherwise show up as a
        stream mismatch inside the lifecycle differential, attributed to the
        Python VM — a real failure with the wrong name on it."""
        header = (REPO / "engine/include/jce/middleware/script/jce_script.h"
                  ).read_text(encoding="utf-8")
        m = re.search(
            r"#define\s+JCE_SCRIPT_DISABLED_NOTICE_FMT\s*((?:\\\n|.)*?)\n(?!.*\\\n)",
            header)
        self.assertIsNotNone(
            m, "JCE_SCRIPT_DISABLED_NOTICE_FMT is gone from jce_script.h. It "
               "is the published wording of the disable notice; without it no "
               "backend has an authority to agree with.")
        # Splice the continued line back together and concatenate the C string
        # literals, which is what the preprocessor does.
        pieces = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1).replace("\\\n", ""))
        self.assertTrue(pieces, f"unparseable macro body: {m.group(1)!r}")
        engine = "".join(pieces)
        self.assertEqual(
            engine, self._const("DISABLED_NOTICE"),
            "jce_script.vm.DISABLED_NOTICE and the engine's "
            "JCE_SCRIPT_DISABLED_NOTICE_FMT no longer render the same line. "
            "Every JceScriptVM backend must write the SAME sentence when it "
            "disables a callback, or the three lifecycle differentials compare "
            "two wordings and blame the backend.")

    def test_the_disablable_handlers_match_the_published_rule(self) -> None:
        """WHICH callbacks participate is decided by the header, not by vm.py.

        jce_script.h's FAILING-CALLBACK RULE lists the participating handlers
        on one indented line; that list is the contract every backend obeys.
        A fifth fixed-name handler added to the rule and not to `DISABLABLE`
        would leave Python spamming where Lua went quiet — a divergence the
        lifecycle differential WOULD catch, but only for handlers it happens to
        drive, and only after a build."""
        header = (REPO / "engine/include/jce/middleware/script/jce_script.h"
                  ).read_text(encoding="utf-8")
        m = re.search(
            r"\*\s+(on_start(?:\s+on_[a-z_]+)+)\s*\n", header)
        self.assertIsNotNone(
            m, "the FAILING-CALLBACK RULE in jce_script.h no longer lists its "
               "participating handlers on one line beginning with on_start")
        published = set(m.group(1).split())
        self.assertEqual(
            published, set(self._const("DISABLABLE")),
            "jce_script.vm.DISABLABLE and the handler list published by "
            "jce_script.h's FAILING-CALLBACK RULE disagree")

    def test_scriptvm_has_a_method_for_every_vtable_slot(self) -> None:
        """The slot list is PARSED, not listed here.

        A 19th slot appended to JceScriptVM is a method missing from ScriptVM,
        and without this test the backend would simply never receive it — the
        silent shape the vtable exists to prevent."""
        header = (REPO / "engine/include/jce/middleware/script/"
                         "jce_script_vm.h").read_text(encoding="utf-8")
        body = re.search(r"struct JceScriptVM \{(.*?)\n\};", header, re.S)
        self.assertIsNotNone(body, "struct JceScriptVM not found")
        clean = re.sub(r"/\*.*?\*/", "", body.group(1), flags=re.S)
        slots = re.findall(
            r"\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*\(", clean)
        self.assertGreaterEqual(len(slots), 18, f"parsed only {slots}")

        cls = next((n for n in self.tree.body
                    if isinstance(n, ast.ClassDef) and n.name == "ScriptVM"),
                   None)
        self.assertIsNotNone(cls, "vm.py has no class ScriptVM")
        methods = {m.name for m in cls.body if isinstance(m, ast.FunctionDef)}

        # create_sized and destroy map to the OBJECT's own lifetime rather than
        # to a dispatch; named here so the mapping is written down instead of
        # inferred.
        mapped = {"create_sized": "open", "destroy": "close"}
        for slot in slots:
            want = mapped.get(slot, slot)
            self.assertIn(
                want, methods,
                f"JceScriptVM slot '{slot}' has no ScriptVM.{want}(). The C "
                f"shim calls these BY NAME, so a slot with no method is a "
                f"dispatch that reaches Python and raises AttributeError once "
                f"per frame, forever")

    def test_the_c_shim_owns_no_scripting_surface(self) -> None:
        """The shim marshals the lifecycle and NOTHING of the `jce` table.

        The 71-entry surface a Python script calls is the generated ctypes
        binding, opened over the same JceScriptHost.  An entry implemented in
        the shim would be a second definition of something a manifest already
        defines, and two definitions drift — this repository's standing failure
        mode."""
        man = _api_json()
        code = re.sub(r"/\*.*?\*/", "", self.shim_c, flags=re.S)
        code = "\n".join(ln for ln in code.splitlines()
                         if not ln.strip().startswith("*"))
        offenders = []
        for entry in man["expose"]:
            # `s->host.<member>` is the shim reaching into the host table
            # itself, which is what a surface implementation looks like here.
            if re.search(r"host\.%s\b" % re.escape(entry["vtable"]), code):
                offenders.append(entry["name"])
        # read_file and log are NOT in `expose`: read_file is how
        # jce_script_instantiate loads a file in BOTH languages, and log is
        # hand_written precisely because it is VM machinery.
        self.assertEqual(
            offenders, [],
            f"the C shim reaches host members {offenders} directly. Those "
            f"belong to the generated binding, which the shim bridges with "
            f"jce_script.open_host(&s->host, sizeof s->host)")

    def test_every_vm_owned_entry_is_hand_written_in_the_manifest(self) -> None:
        """`VM_OWNED` may only claim entries the C ABI does not export.

        If one ever moves OUT of `hand_written` into the generated surface,
        vm.py's own implementation would silently shadow the generated one and
        the two would drift with nothing comparing them."""
        man = _api_json()
        hand = {e["name"] for e in man["hand_written"]}
        owned = set(self._const("VM_OWNED"))
        self.assertTrue(
            owned <= hand,
            f"vm.py implements {sorted(owned - hand)}, which the manifest no "
            f"longer lists as hand_written — the generated binding exposes "
            f"them now and vm.py's copy would shadow it")


class TestBackendValidate(unittest.TestCase):
    """PythonBackend.validate() — the one condition this backend owns."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.members = core.parse_host_members(
            core.HEADER.read_text(encoding="utf-8"))
        cls.man = core.load_manifest()

    def test_a_missing_c_abi_declaration_is_named(self) -> None:
        """Every symbol the ctypes module resolves must be declared in the
        COMMITTED C ABI header, and the failure must NAME it.

        This condition cannot fail against a clean tree, so without this test
        it could be deleted, emptied or made to return `[]` and the generator
        would go on printing OK — which is exactly the hole the seam's own M8
        mutation found in the Lua backend's condition 5.  The synthetic entry
        below is the only way to observe it firing."""
        self.assertEqual(
            ep.BACKEND.validate(self.members, self.man, ""), [],
            "the real manifest must validate clean, or the negative case "
            "below proves nothing")

        bogus = dict(self.man)
        bogus["expose"] = list(self.man["expose"]) + [
            {"name": "no_such_entry_point", "vtable": "log",
             "shape": "void_call", "since": 1}]
        problems = ep.BACKEND.validate(self.members, bogus, "")
        self.assertTrue(problems, "validate() accepted an entry the C ABI "
                                  "header does not declare")
        self.assertTrue(
            any("jce_script_api_no_such_entry_point" in p for p in problems),
            f"validate() failed but did not name the missing symbol: "
            f"{problems}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
