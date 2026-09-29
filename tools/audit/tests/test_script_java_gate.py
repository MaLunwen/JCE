#!/usr/bin/env python3
"""test_script_java_gate.py — the Java/JNI backend's own gate tests.

ci.yml already runs `python -m unittest discover -s tools/audit/tests`, so this
file joins CI by existing.  emit_java.py's docstring named it before it was
written; a docstring naming a test that does not exist is the "comment stating
a contract nothing enforces" this campaign keeps finding, so here it is.

WHAT IS TESTED HERE, AND WHAT IS TESTED SOMEWHERE ELSE
------------------------------------------------------
tests/scripting/java/ runs the cross-language DIFFERENTIAL: the same
manifest-derived cases through the Lua bindings and through the Java surface,
comparing arity, per-slot type and value, and the host-call trace.  That proves
the two surfaces AGREE.  It cannot prove anything about code it never executes,
and three of the properties below are exactly that:

  * the JNI local-reference discipline.  MEASURED, not assumed: a mutation that
    leaks 200,000 local references inside one native frame runs the entire
    differential GREEN under `java -Xcheck:jni`.  -Xcheck:jni catches reference
    MISUSE (a double DeleteLocalRef makes it abort with "Bad global or local ref
    passed to JNI" — also measured) but it does not report accumulation.  So the
    discipline is enforced statically, over the emitted shim, by
    test_no_native_creates_more_than_one_object and its two companions.
  * GetStringUTFChars / ReleaseStringUTFChars pairing.  A missed release is a
    native memory leak that no functional comparison can see.
  * native-declaration parity.  A native declared in Java with no JNIEXPORT
    behind it is an UnsatisfiedLinkError at the first call — far from the cause.

THE NAME RULE IS CHECKED AGAINST A SECOND SOURCE, ON PURPOSE.  The expectation
is built from contracts/script-api.json (the published, language-neutral
contract) with a snake->camel transform written independently in this file, and
compared against the emitted Java.  Deriving it from emit_java.camel() would be
self-consistency: the emitter would agree with itself whatever it did.

Run:
  python -m unittest discover -s tools/audit/tests -p "test_*.py" -v
"""

from __future__ import annotations

import copy
import importlib
import json
import re
import sys
import unittest
from pathlib import Path

QUOTE_CHARS = chr(34) + chr(39)   # " and ', built to survive any quoting layer

REPO_ROOT = Path(__file__).resolve().parents[3]

API_JSON = REPO_ROOT / "contracts/script-api.json"
JAVA_SRC = REPO_ROOT / "scripting/java/src/main/java/com/jce/script/JceScript.java"
JNI_C = REPO_ROOT / "scripting/java/native/jce_script_jni.gen.c"

JNI_PREFIX = "Java_com_jce_script_JceScript_"

# Every JNI entry point that hands back a NEW object reference.  The shim's
# whole local-reference argument is that it calls at most one of these per
# native and returns what it made; anything else on this list appearing in the
# emitted shim is a design change that has to be argued, not a typo.
_JNI_MAKES_OBJECT = (
    "NewStringUTF", "NewString", "NewObject", "NewObjectA", "NewObjectV",
    "AllocObject", "NewLocalRef", "NewGlobalRef", "NewWeakGlobalRef",
    "NewDirectByteBuffer", "NewObjectArray", "NewBooleanArray", "NewByteArray",
    "NewCharArray", "NewShortArray", "NewIntArray", "NewLongArray",
    "NewFloatArray", "NewDoubleArray", "GetObjectArrayElement",
    "GetObjectField", "GetStaticObjectField", "CallObjectMethod",
    "CallStaticObjectMethod", "ToReflectedMethod", "ToReflectedField",
    "FindClass", "GetObjectClass", "GetSuperclass",
)

# A reference that outlives the native call.  There is no free() for these on
# return, so one of them in a per-frame binding is an unbounded leak.
_JNI_MAKES_LASTING_REF = ("NewGlobalRef", "NewWeakGlobalRef")


def load_emit_java():
    sys.path.insert(0, str(REPO_ROOT / "tools" / "audit"))
    sys.path.insert(0, str(REPO_ROOT / "tools" / "scriptgen"))
    return importlib.import_module("emit_java")


def strip_c_comments(text: str) -> str:
    """The repository's one C comment/string stripper, not a fourth copy.

    tools/audit/cdecl.py owns it; scriptgen_core.py imports the same one.  A
    private regex here would be the two-sided contract cdecl.py exists to end —
    and it would matter: the shim's own banner contains the word `while`, so a
    loop check that did not strip comments would fail on prose."""
    sys.path.insert(0, str(REPO_ROOT / "tools" / "audit"))
    from cdecl import strip_comments_and_strings
    return strip_comments_and_strings(text)


def camel_independently(snake: str) -> str:
    """snake_case -> lowerCamelCase, written HERE and not imported.

    The point of a differential expectation is that it comes from somewhere
    other than the thing under test; the same applies to a name rule."""
    head, *rest = snake.split("_")
    return head + "".join(part[:1].upper() + part[1:] for part in rest)


def jni_calls(body: str) -> list[str]:
    """The JNI entry points one function body invokes, one entry per SITE.

    Counted through `(*env)->NAME(`, not with `body.count("NewString")`:
    "NewString" is a substring of "NewStringUTF", so a substring count scores
    one NewStringUTF call as two object creations and the gate fails on
    correct code.  That is what it did on first run."""
    return re.findall(r"->\s*(\w+)\s*\(", body)


def split_jni_functions(c_text: str) -> dict[str, str]:
    """{native name: body}, from the comment-stripped shim.

    Brace-counted rather than regex-matched to the closing brace: every
    JNIEXPORT here is top level, so the body ends at the first brace that
    returns the depth to zero."""
    text = strip_c_comments(c_text)
    out: dict[str, str] = {}
    for m in re.finditer(re.escape(JNI_PREFIX) + r"(\w+)\s*\(", text):
        name = m.group(1)
        i = text.find("{", m.end())
        if i < 0:
            continue
        depth = 0
        for j in range(i, len(text)):
            if text[j] == "{":
                depth += 1
            elif text[j] == "}":
                depth -= 1
                if depth == 0:
                    out[name] = text[i:j + 1]
                    break
    return out


class JavaSurfaceFilesTest(unittest.TestCase):
    """Every assertion below reads the COMMITTED emitted files, which
    gen_script_bindings.py's check mode has already proved byte-identical to a
    fresh emit.  So a rule broken here is a rule the emitter broke."""

    @classmethod
    def setUpClass(cls):
        cls.api = json.loads(API_JSON.read_text(encoding="utf-8"))
        cls.java = JAVA_SRC.read_text(encoding="utf-8")
        cls.jni = JNI_C.read_text(encoding="utf-8")
        cls.funcs = split_jni_functions(cls.jni)

    # ── the name rule, against the neutral contract ───────────────────
    def test_every_manifest_name_appears_as_its_derived_camelCase_method(self):
        want = {camel_independently(e["name"]) for e in self.api["expose"]}
        have = set(re.findall(r"^    public [\w\[\]., ]+? (\w+)\(", self.java,
                              re.M))
        # The lifecycle methods are not manifest entries and are named by hand
        # in the surface class; everything else must be a derived name.
        lifecycle = {"open", "close", "nativeHandle", "libraryApiVersion"}
        self.assertEqual(want, have - lifecycle,
                         "the Java method set is not the manifest's names "
                         "camelCased — a hand-maintained mapping is the defect "
                         "this generator exists to prevent")

    def test_no_hand_written_entry_leaked_onto_the_java_surface(self):
        have = set(re.findall(r"^    public [\w\[\]., ]+? (\w+)\(", self.java,
                              re.M))
        for e in self.api["hand_written"]:
            self.assertNotIn(camel_independently(e["name"]), have,
                             f"{e['name']} is hand-written in the engine and "
                             f"excluded from the C ABI as a class")

    # ── native parity: declared in Java, implemented in C ─────────────
    def test_every_native_declared_in_java_is_implemented_in_the_shim(self):
        declared = set(re.findall(r"private static native [\w\[\]]+ (\w+)\(",
                                  self.java))
        implemented = set(self.funcs)
        self.assertEqual(declared, implemented,
                         "a native declared with no JNIEXPORT behind it is an "
                         "UnsatisfiedLinkError at the first call, reported far "
                         "from its cause")

    def test_every_native_and_its_shim_agree_on_the_argument_count(self):
        java_args = {}
        for name, args in re.findall(
                r"private static native [\w\[\]]+ (\w+)\(([^;]*?)\);",
                self.java, re.S):
            args = args.strip()
            java_args[name] = 0 if not args else len(args.split(","))
        stripped = strip_c_comments(self.jni)
        for name, n in sorted(java_args.items()):
            m = re.search(re.escape(JNI_PREFIX) + re.escape(name) +
                          r"\s*\(([^)]*)\)", stripped)
            self.assertIsNotNone(m, name)
            # env and cls are the JNI calling convention, not arguments.
            c_n = len([p for p in m.group(1).split(",") if p.strip()]) - 2
            self.assertEqual(n, c_n,
                             f"{name}: Java declares {n} argument(s), the shim "
                             f"takes {c_n} — the JVM matches by name only and "
                             f"would corrupt the stack, not refuse the call")

    # ── the local-reference discipline, enforced where it is provable ──
    def test_no_native_creates_more_than_one_object(self):
        """One object per call, returned immediately, is what makes a leak
        impossible without any DeleteLocalRef in the shim.

        NOT redundant with -Xcheck:jni: measured, that checker runs GREEN over
        the whole differential with 200,000 local references leaked inside one
        native frame."""
        for name, body in sorted(self.funcs.items()):
            n = sum(1 for c in jni_calls(body) if c in _JNI_MAKES_OBJECT)
            self.assertLessEqual(
                n, 1,
                f"{name} creates {n} object references in one native frame. "
                f"The shim's discipline is at most one, returned immediately; "
                f"more than one needs an explicit DeleteLocalRef and a reason.")

    def test_no_native_creates_a_reference_that_outlives_the_call(self):
        for name, body in sorted(self.funcs.items()):
            made = set(jni_calls(body)) & set(_JNI_MAKES_LASTING_REF)
            for call in sorted(made):
                self.fail(
                    f"{name} calls {call}; a global reference is not freed when "
                    f"the native returns, so one per frame is an unbounded leak")

    def test_the_shim_contains_no_loop(self):
        """`at most one object per call` is a STATIC count only while nothing
        iterates.  A loop is not forbidden forever — it is forbidden until
        someone writes the DeleteLocalRef that makes it safe, and changes this
        test to say so."""
        stripped = strip_c_comments(self.jni)
        for kw in ("for", "while", "goto"):
            hits = re.findall(r"\b" + kw + r"\b", stripped)
            self.assertEqual(
                [], hits,
                f"the emitted shim contains `{kw}`. Per-element object creation "
                f"in a loop is the JNI local-reference leak, and the count in "
                f"test_no_native_creates_more_than_one_object stops being "
                f"static the moment one appears.")

    def test_every_GetStringUTFChars_is_released_on_the_one_return_path(self):
        for name, body in sorted(self.funcs.items()):
            calls = jni_calls(body)
            gets = calls.count("GetStringUTFChars")
            rels = calls.count("ReleaseStringUTFChars")
            self.assertEqual(
                gets, rels,
                f"{name} takes {gets} string argument(s) and releases {rels}; "
                f"an unreleased GetStringUTFChars is a native memory leak no "
                f"result comparison can see")
            if gets:
                # AT MOST one, not exactly one: a void_call native has no
                # return statement at all, which is equally single-exit.
                # Asserting exactly one reddens every void entry that takes a
                # string -- nAnimSetBool was the first, on this test's first
                # run. What is forbidden is a SECOND exit, which would leave
                # the releases below it unexecuted.
                self.assertLessEqual(
                    len(re.findall(r"\breturn\b", body)), 1,
                    f"{name} has more than one return. The releases are "
                    f"emitted once, at the end of the body, so a second exit "
                    f"would skip every one of them")

    def test_every_string_argument_is_null_checked_before_it_is_read(self):
        """A Java caller may pass null for any String parameter.  Handing NULL
        to GetStringUTFChars is undefined behaviour, not an exception."""
        for name, body in sorted(self.funcs.items()):
            lines = body.splitlines()
            for i, ln in enumerate(lines):
                m = re.search(r"GetStringUTFChars\(env, (j_\w+),", ln)
                if not m or "Release" in ln:
                    continue
                jparam = m.group(1)
                # The guard is the line that OPENS the block, and its spelling
                # is `if (j_x)` for the first string argument and
                # `if (ok && j_x)` from the second onward -- the second form
                # exists because a failed first read leaves an exception
                # pending. Matching the literal `if (j_x)` missed the second
                # form the moment it was introduced, which is how this
                # assertion proved it was reading the real text.
                guard = lines[i - 1] if i else ""
                self.assertRegex(
                    guard, r"if \((?:ok && )?" + re.escape(jparam) + r"\)",
                    f"{name} reads {jparam} without checking it for null "
                    f"first; the guard line was {guard!r}")


class JavaBackendConditionsTest(unittest.TestCase):
    """emit_java.validate()'s own conditions, each proved able to FAIL.

    The emitter's docstring calls these "conditions of this backend, not
    comments"; a condition whose failure path has never been executed is a
    comment with an if-statement around it."""

    def setUp(self):
        sys.path.insert(0, str(REPO_ROOT / "tools" / "scriptgen"))
        import scriptgen_core as core
        self.core = core
        self.m = load_emit_java()
        self.members = core.parse_host_members(
            core.HEADER.read_text(encoding="utf-8"))
        self.man = core.load_manifest()
        self.c_text = core.SCRIPT_C.read_text(encoding="utf-8")

    def run_validate(self, man):
        return self.m.BACKEND.validate(self.members, man, self.c_text)

    def test_the_manifest_as_committed_passes_every_condition(self):
        self.assertEqual([], self.run_validate(self.man))

    def test_a_modifier_this_backend_does_not_render_fails_J1(self):
        """MODIFIERS is a frozenset, so the tenth modifier is injected by
        rebinding the module global validate() reads — not by mutating it."""
        man = copy.deepcopy(self.man)
        man["expose"][0]["a_tenth_modifier"] = True
        original = self.m.MODIFIERS
        self.m.MODIFIERS = frozenset(original | {"a_tenth_modifier"})
        try:
            problems = self.run_validate(man)
        finally:
            self.m.MODIFIERS = original
        self.assertTrue(any("a_tenth_modifier" in p for p in problems),
                        problems)

    def test_two_entries_that_camel_to_one_name_fail_J2(self):
        """`touch_1` and `touch1` are both legal lower_snake_case and both
        camel to `touch1`.  camelCase is NOT injective over snake_case, which
        is why this condition exists rather than being argued away."""
        man = copy.deepcopy(self.man)
        clone = copy.deepcopy(man["expose"][0])
        man["expose"][0]["name"] = "touch_1"
        clone["name"] = "touch1"
        man["expose"].append(clone)
        problems = self.run_validate(man)
        self.assertTrue(
            any("touch1" in p and "camel" in p for p in problems), problems)

    def test_a_reserved_word_name_fails_J2(self):
        man = copy.deepcopy(self.man)
        man["expose"][0]["name"] = "final"
        problems = self.run_validate(man)
        self.assertTrue(any("reserved word" in p for p in problems), problems)

    def test_a_name_that_is_not_snake_case_fails_J2(self):
        man = copy.deepcopy(self.man)
        man["expose"][0]["name"] = "GetPosition"
        problems = self.run_validate(man)
        self.assertTrue(any("lower_snake_case" in p for p in problems),
                        problems)

    def test_an_empty_path_component_is_a_hard_failure_not_a_dropped_segment(self):
        """`get__position` and `get_position` would both camel to
        getPosition and one would silently overwrite the other."""
        with self.assertRaises(SystemExit):
            self.m.camel("get__position")

    def test_a_constant_of_an_undecided_kind_fails_J4(self):
        man = copy.deepcopy(self.man)
        man["constants"][0]["kind"] = "some_new_kind"
        problems = self.run_validate(man)
        self.assertTrue(any("some_new_kind" in p for p in problems), problems)


class JavaBuildIndependenceTest(unittest.TestCase):
    """The JVM must not become an engine build dependency.

    scripting/CMakeLists.txt add_subdirectory()s every backend on EVERY
    configure of the project, so the guard has to come before any find_path —
    otherwise a JDK becomes a configure-time input on every machine."""

    def test_the_jdk_probe_is_behind_the_option_guard(self):
        # Comments stripped FIRST: this file's own banner explains the rule and
        # names `find_path` while doing so, and searching the raw text finds
        # the prose at offset 775 rather than the call at 1686.
        raw = (REPO_ROOT / "scripting/java/CMakeLists.txt").read_text(
            encoding="utf-8")
        cml = "\n".join(ln.split("#", 1)[0] for ln in raw.splitlines())
        guard = cml.index("if(NOT JCE_BUILD_SCRIPT_JAVA)")
        ret = cml.index("return()", guard)
        for probe in ("find_path", "find_package", "find_program"):
            i = cml.find(probe)
            if i >= 0:
                self.assertGreater(
                    i, ret,
                    f"{probe} runs before the JCE_BUILD_SCRIPT_JAVA guard, so "
                    f"configuring the ENGINE would probe for a JDK")

    # The four spellings by which the engine's build could acquire a JDK.
    # Named exactly: a bare "JNI" hits engine/CMakeLists.txt's PRE-EXISTING
    # JCE_BUILD_JNI (the Android platform layer's option, nothing to do with
    # this backend) and would report a violation that predates scripting/.
    _REACHES_INTO_JAVA = ("scripting/java", "jce_script_java",
                          "JCE_BUILD_SCRIPT_JAVA", "JCE_JNI_INCLUDE_DIR")

    @staticmethod
    def _code_only(text):
        """CMake code with comments removed.

        The check is a text match, and a text match cannot tell a build rule
        from a sentence describing one.  The root CMakeLists.txt carries a
        comment naming tests/scripting/java/vm/CMakeLists.txt as one of the
        two stale-glob failures measured on 2026-08-16 -- prose about a build
        that happened, not a build edge -- and matching it reported the engine
        reaching into Java when nothing does.

        A `#` inside a quoted string is not a comment, so quotes are tracked
        rather than cutting at the first `#`.  Everything the gate forbids is
        an identifier or a path, so stripping comments cannot hide a real
        violation: a rule cannot live in a comment and still take effect.
        """
        out = []
        for line in text.splitlines():
            q, cut = None, len(line)
            for i, ch in enumerate(line):
                if q:
                    if ch == q:
                        q = None
                elif ch in QUOTE_CHARS:
                    q = ch
                elif ch == '#':
                    cut = i
                    break
            out.append(line[:cut])
        return chr(10).join(out)

    def assert_free_of_java(self, text, where):
        code = self._code_only(text)
        for spelling in self._REACHES_INTO_JAVA:
            self.assertNotIn(spelling, code, where)
    def test_a_comment_naming_java_is_not_a_violation(self):
        """The false positive this stripper exists for, pinned so it cannot
        come back as a mystery: prose is not a build edge."""
        text = ("# tests/scripting/java/vm/CMakeLists.txt was rebuilt stale" + chr(10)
                + "add_subdirectory(engine)" + chr(10))
        self.assert_free_of_java(text, "<synthetic-comment>")

    def test_a_hash_inside_a_string_does_not_hide_a_violation(self):
        """The stripper must not cut at a `#` that is inside quotes, or a rule
        written after one would become invisible to the gate."""
        text = ('set(X "a#b")' + chr(10)
                + 'add_subdirectory(scripting/java)' + chr(10))
        with self.assertRaises(AssertionError):
            self.assert_free_of_java(text, "<synthetic-hash-in-string>")

    def test_the_reaches_into_java_check_fires_on_a_line_that_does(self):
        """A positive control, because the real inputs are files this backend
        does not own and must not mutate even briefly: three agents share this
        worktree and a transient edit to engine/CMakeLists.txt could be
        committed by one of them. An assertion over four `assertNotIn`s that
        has never been seen failing is one more test that cannot fail."""
        with self.assertRaises(AssertionError):
            self.assert_free_of_java(
                "add_subdirectory(scripting/java)", "<synthetic>")
        with self.assertRaises(AssertionError):
            self.assert_free_of_java(
                "target_link_libraries(jce_core PRIVATE jce_script_java)",
                "<synthetic>")

    def test_nothing_under_engine_or_the_root_build_reaches_into_java(self):
        for rel in ("engine/CMakeLists.txt", "CMakeLists.txt",
                    "tests/CMakeLists.txt"):
            self.assert_free_of_java(
                (REPO_ROOT / rel).read_text(encoding="utf-8"), rel)


if __name__ == "__main__":
    unittest.main()
