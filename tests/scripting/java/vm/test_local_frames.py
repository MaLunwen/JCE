#!/usr/bin/env python3
"""
test_local_frames.py — the JNI disciplines that no runtime check can see.

THREE OF THE FOUR RULES HERE EXIST BECAUSE THE DYNAMIC ORACLE DOES NOT WORK.

  * Local references.  Measured by the batch-1 agent on JDK 21.0.9: a shim
    mutated to leak 200,000 local references inside ONE native frame runs a
    151-case differential GREEN under -Xcheck:jni, with no warning of any kind.
    So "no leak" cannot be asserted by running anything; it is held by
    STRUCTURE — every slot body inside PushLocalFrame / PopLocalFrame — and the
    structure is what is checked.

  * Slot ORDER.  `call_start` and `release` have the identical signature
    `void (JceScript *, JceScriptInstance)`.  Swapping them in the vtable
    compiles cleanly, passes the engine's signature pin, passes C4113, and
    produces a VM that runs on_destroy where on_start belongs.  The only thing
    that can catch it is that the initialiser's Nth entry must be named for the
    header's Nth slot — which is what this file checks, against
    jce_script_vm.h, never against a list spelled here.

  * The exception check.  Its absence is a wrong RETURN VALUE two dispatches
    later, not a crash; the differential's `named_throws` case sees it, and
    this makes sure a slot cannot quietly drop the call.

Run:  python tests/scripting/java/vm/test_local_frames.py
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
BACKEND = REPO_ROOT / "scripting/java/native/jce_script_vm_java.c"
VM_HEADER = REPO_ROOT / "engine/include/jce/middleware/script/jce_script_vm.h"

# JNI entry points that MAKE a local reference.  A reference created outside a
# frame outlives the call and the frame loop leaks one per dispatch.
REF_CREATING = (
    "NewStringUTF", "NewByteArray", "NewObject", "NewObjectArray",
    "NewDirectByteBuffer", "AllocObject", "FindClass", "ExceptionOccurred",
    "CallObjectMethod", "CallStaticObjectMethod", "GetObjectArrayElement",
    "GetObjectField", "GetStaticObjectField", "ToReflectedMethod",
    "ToReflectedField", "GetSuperclass", "GetObjectClass",
)

_FUNC_START = re.compile(r"^(?:static\s+)?[A-Za-z_][\w\s\*]*?\b(java_\w+)\s*\(")


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def vtable_slots() -> list[str]:
    """The slot names, in order, out of struct JceScriptVM itself."""
    text = read(VM_HEADER)
    start = text.index("struct JceScriptVM {")
    end = text.index("\n};", start)
    slots = re.findall(r"\(\*(\w+)\)\s*\(", text[start:end])
    if len(slots) < 10:
        raise SystemExit(f"only {len(slots)} slots parsed from {VM_HEADER}")
    return slots


def functions(text: str) -> dict[str, str]:
    """name -> body, for every top-level java_* function definition.

    Relies on one property of this file and states it: a function's closing
    brace is the only `}` in column 0."""
    out: dict[str, str] = {}
    name: str | None = None
    buf: list[str] = []
    for line in text.splitlines():
        if name is None:
            m = _FUNC_START.match(line)
            if m and not line.rstrip().endswith(";"):
                name = m.group(1)
                buf = [line]
            continue
        buf.append(line)
        if line == "}":
            out[name] = "\n".join(buf)
            name = None
    return out


class SlotOrderTest(unittest.TestCase):
    """The vtable is positional and two slots share a signature."""

    @classmethod
    def setUpClass(cls):
        cls.text = read(BACKEND)
        cls.slots = vtable_slots()
        block = re.search(r"static const JceScriptVM k_java_vm = \{(.*?)\n\};",
                          cls.text, re.S)
        assert block, "k_java_vm initialiser not found in " + str(BACKEND)
        entries = [e.strip() for e in block.group(1).split(",")]
        cls.entries = [e for e in entries if e and not e.startswith("/*")]

    def test_the_table_fills_exactly_the_slots_the_header_declares(self):
        # struct_size and language come first and are not slots.
        self.assertEqual(len(self.entries), len(self.slots) + 2,
                         f"k_java_vm has {len(self.entries)} initialisers for "
                         f"{len(self.slots)} slots + struct_size + language")

    def test_each_entry_is_named_for_the_slot_it_fills(self):
        """The one check that can see call_start and release swapped."""
        for i, slot in enumerate(self.slots):
            got = self.entries[i + 2]
            self.assertEqual(
                got, "java_" + slot,
                f"vtable position {i} is slot '{slot}' but is filled with "
                f"'{got}'. Two slots in JceScriptVM share the signature "
                f"void(JceScript *, JceScriptInstance), so a swap here "
                f"compiles, links and runs the wrong hook.")


class SlotBodyTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.text = read(BACKEND)
        cls.funcs = functions(cls.text)
        cls.slots = vtable_slots()

    def slot_bodies(self):
        for slot in self.slots:
            name = "java_" + slot
            self.assertIn(name, self.funcs,
                          f"slot '{slot}' has no java_{slot} definition")
            yield name, self.funcs[name]

    def test_every_slot_opens_and_closes_a_local_frame(self):
        for name, body in self.slot_bodies():
            # assertTrue, not assertIn: assertIn prints the whole haystack, and
            # a 40-line C function in the failure message buries the sentence
            # that says what is wrong.
            self.assertTrue("PushLocalFrame" in body,
                            f"{name} does not open a local frame")
            self.assertTrue("PopLocalFrame" in body,
                            f"{name} does not pop its local frame")

    def test_no_slot_creates_a_reference_outside_its_frame(self):
        for name, body in self.slot_bodies():
            # Checked before index()/rindex(), which would otherwise raise
            # ValueError and turn a NAMED failure into an ERROR — and an error
            # says less than a red test, which is a lesson this repository has
            # relearned four times.
            self.assertTrue("PushLocalFrame" in body and "PopLocalFrame" in body,
                            f"{name} has no local frame to check references "
                            f"against")
            first_push = body.index("PushLocalFrame")
            last_pop = body.rindex("PopLocalFrame")
            for call in REF_CREATING:
                for m in re.finditer(r"\b" + call + r"\s*\(", body):
                    self.assertTrue(
                        first_push < m.start() < last_pop,
                        f"{name} calls {call} outside its local frame — that "
                        f"reference outlives the dispatch, and -Xcheck:jni "
                        f"does not report it")

    def test_every_slot_checks_and_clears_pending_exceptions(self):
        """No exemptions.  A slot that skips this leaves an exception pending,
        and the NEXT slot then misbehaves in a way that looks unrelated.

        Two spellings are accepted and the second is not a loophole:
        `java_handler_threw` is the FAILING-CALLBACK-RULE wrapper the five
        fixed-name lifecycle slots use, and it is accepted only because
        `test_the_rule_wrapper_still_takes_the_exception` below proves it
        performs the take itself.  A slot that names neither still fails."""
        for name, body in self.slot_bodies():
            self.assertTrue(
                "java_take_exception" in body or "java_handler_threw" in body,
                f"{name} never checks for a pending exception")

    def test_the_rule_wrapper_still_takes_the_exception(self):
        """The one hop the check above is allowed to accept.

        `java_handler_threw` exists so the five fixed-name lifecycle slots can
        report AND disable in one line (jce_script.h's FAILING-CALLBACK RULE).
        It is accepted as a substitute for `java_take_exception` at the slot
        level, so it has to actually perform the take — and it has to stop
        early when there was no exception, or every clean dispatch would write
        a disable notice for a handler that did not fail."""
        body = self.funcs.get("java_handler_threw")
        self.assertIsNotNone(
            body, "java_handler_threw is gone but the slot-level check still "
                  "accepts its name; remove the acceptance in the same commit")
        self.assertIn(
            "java_take_exception", body,
            "java_handler_threw no longer checks or clears the pending "
            "exception, so every slot that calls it is now unguarded")
        self.assertIn(
            "return false", body,
            "java_handler_threw has no early return: it must do nothing when "
            "java_take_exception found no exception, or a clean dispatch "
            "disables the handler it just ran")

    def test_every_slot_but_create_enforces_the_owning_thread(self):
        """create_sized is the exemption and the reason is structural: it is
        the call that ESTABLISHES which thread owns the handle, so there is
        nothing for it to compare against yet."""
        for name, body in self.slot_bodies():
            if name == "java_create_sized":
                self.assertIn("jce_thread_current_id", body,
                              "create_sized must record the owning thread")
                continue
            self.assertIn("java_env(", body,
                          f"{name} dispatches without going through java_env, "
                          f"so it does not enforce the owning-thread rule")


class HelperTest(unittest.TestCase):
    """Two helpers make local references.  Neither gets an allowlist entry;
    each gets the check its own stated reason implies."""

    @classmethod
    def setUpClass(cls):
        cls.text = read(BACKEND)
        cls.funcs = functions(cls.text)

    def test_the_id_cache_deletes_every_reference_it_makes(self):
        body = self.funcs["java_cache_ids"]
        made = len(re.findall(r"\bFindClass\s*\(", body))
        freed = len(re.findall(r"\bDeleteLocalRef\s*\(", body))
        self.assertGreater(made, 0, "java_cache_ids finds no classes at all")
        self.assertEqual(
            made, freed,
            f"java_cache_ids creates {made} local reference(s) with FindClass "
            f"and deletes {freed}. It runs OUTSIDE any slot's frame, so what "
            f"it does not delete is never freed.")

    def test_the_exception_reporter_is_only_called_inside_a_frame(self):
        """`java_take_exception` calls ExceptionOccurred and CallObjectMethod
        and deletes neither; `java_handler_threw` adds a NewStringUTF.  All
        three references are correct only because every call site is inside
        its caller's PushLocalFrame/PopLocalFrame.

        Both helpers are checked in one pass and NEITHER is exempted from the
        rule — each is exempted only from being its own call site."""
        helpers = ("java_take_exception", "java_handler_threw")
        for name, body in self.funcs.items():
            if name in helpers:
                continue
            for helper in helpers:
                calls = list(re.finditer(r"\b" + helper + r"\s*\(", body))
                if not calls:
                    continue
                self.assertTrue(
                    "PushLocalFrame" in body and "PopLocalFrame" in body,
                    f"{name} calls {helper} with no frame open; the "
                    f"references it makes would never be freed")
                first_push = body.index("PushLocalFrame")
                last_pop = body.rindex("PopLocalFrame")
                for m in calls:
                    self.assertTrue(
                        first_push < m.start() < last_pop,
                        f"{name} calls {helper} outside its local frame")


if __name__ == "__main__":
    print(f"checking {BACKEND.relative_to(REPO_ROOT)}")
    sys.exit(0 if unittest.main(exit=False, verbosity=2).result.wasSuccessful()
             else 1)
