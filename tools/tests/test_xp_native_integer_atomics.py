"""Pure source/defined-arithmetic controls; no compiler or native code runs.

The separate C99 fixture consumes the actual header for Root-owned compiler,
instruction/import and XP execution validation. These tests do not simulate
concurrency or certify target code generation.
"""
import re
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "engine/src/os/platform/jce_native_atomic_integer_internal.h"
CONTROL = ROOT / "tests/compatibility/xp_flecs/atomic_control.c"


def body(source, name):
    found = re.search(r"\b" + name + r"\([^{};]*\)\s*\{", source)
    if not found:
        raise AssertionError("actual function missing: " + name)
    start, depth = found.end(), 1
    for offset in range(start, len(source)):
        depth += (source[offset] == "{") - (source[offset] == "}")
        if not depth:
            return source[start:offset]
    raise AssertionError("unterminated actual function: " + name)


def from_bits(source, width, bits):
    """Evaluate only the actual pure C return-expression arithmetic."""
    actual = body(source, "jce_native_atomic_i" + str(width) + "_from_bits")
    expression = re.search(r"return\s+(.+?);", actual, re.S).group(1)
    expression = re.sub(r"\((?:u?int(?:32|64)_t)\)", "", expression)
    expression = re.sub(r"UINT(?:32|64)_C\(([0-9]+)\)", r"\1", expression)
    if not re.fullmatch(r"[a-zA-Z0-9_\s()+<>=?:-]+", expression):
        raise AssertionError("unexpected conversion expression")
    condition, result = expression.split("?", 1)
    positive, negative = result.split(":", 1)
    scope = {"bits": bits, "INT32_MIN": -(1 << 31), "INT32_MAX": (1 << 31) - 1,
             "INT64_MIN": -(1 << 63), "INT64_MAX": (1 << 63) - 1}
    branch = positive if eval(condition.strip(), {"__builtins__": {}}, scope) else negative
    return eval(branch.strip(), {"__builtins__": {}}, scope)


def check_backend_wires(source):
    """Assert actual owner wires, also used unchanged against ordinary mutants."""
    msvc, gnu = source.split("#elif JCE_COMPILER_GCC || JCE_COMPILER_CLANG", 1)
    for width in (32, 64):
        actual = body(gnu, "jce_native_atomic_i" + str(width) + "_add")
        assert "__atomic_add_fetch(" in actual
        assert "__ATOMIC_SEQ_CST" in actual
        assert "jce_native_atomic_i" + str(width) + "_from_bits(post)" in actual
        assert "__atomic_always_lock_free(sizeof(JceNativeAtomicI" + str(width) + "Bits), 0)" in gnu
    assert "__may_alias__" in gnu
    assert "__ATOMIC_ACQUIRE" in body(gnu, "jce_native_atomic_word_load_acquire")
    assert "__ATOMIC_RELEASE" in body(gnu, "jce_native_atomic_word_store_release")
    cas = body(gnu, "jce_native_atomic_word_compare_exchange_acq_rel")
    assert "false," in cas and "__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE" in cas
    assert "jce_native_atomic_word_load(word)" in body(msvc, "jce_native_atomic_word_load_acquire")
    assert "jce_native_atomic_word_store(word, value)" in body(msvc, "jce_native_atomic_word_store_release")
    assert "jce_native_atomic_word_compare_exchange(word, expected, desired)" in body(msvc, "jce_native_atomic_word_compare_exchange_acq_rel")


class NativeIntegerAtomicSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = HEADER.read_text(encoding="utf-8")
        cls.control = CONTROL.read_text(encoding="utf-8")

    def test_actual_unsigned_conversion_keeps_all_signed_edges_representable(self):
        for width in (32, 64):
            high = 1 << (width - 1)
            for value in (0, 1, high - 1, high, high + 1, (1 << width) - 2, (1 << width) - 1):
                with self.subTest(width=width, bits=value):
                    expected = value if value < high else value - (1 << width)
                    self.assertEqual(from_bits(self.source, width, value), expected)

    def test_post_add_vectors_wrap_without_signed_or_double_precision_loss(self):
        for width in (32, 64):
            limit = 1 << (width - 1)
            vectors = [(0, 1, 1), (1, -1, 0), (limit - 1, 1, -limit),
                       (-limit, -1, limit - 1), (-limit, -limit, 0), (limit - 1, -limit, -1)]
            if width == 64:
                vectors += [(9007199254740993, 1, 9007199254740994),
                            (9007199254740994, -2, 9007199254740992)]
            for previous, delta, expected in vectors:
                with self.subTest(width=width, previous=previous, delta=delta):
                    unsigned_post = (previous + delta) & ((1 << width) - 1)
                    self.assertEqual(from_bits(self.source, width, unsigned_post), expected)

    def test_actual_backend_and_publication_wires(self):
        check_backend_wires(self.source)

    def test_prevalue_and_relaxed_publication_mutants_fail_ordinary_assertions(self):
        for old, replacement in (("__atomic_add_fetch(", "__atomic_fetch_add("),
                                 ("__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE", "__ATOMIC_RELAXED, __ATOMIC_RELAXED"),
                                 ("__atomic_always_lock_free(sizeof(JceNativeAtomicI64Bits), 0)", "1")):
            with self.subTest(old=old):
                self.assertIn(old, self.source)
                with self.assertRaises(AssertionError):
                    check_backend_wires(self.source.replace(old, replacement))

    def test_bad_signed_wrap_conversion_fails_independent_edge_vector(self):
        mutated = self.source.replace("INT32_MIN + (int32_t)", "INT32_MAX + (int32_t)")
        self.assertNotEqual(mutated, self.source)
        with self.assertRaises(AssertionError):
            self.assertEqual(from_bits(mutated, 32, 1 << 31), -(1 << 31))

    def test_msvc_x86_64bit_path_is_intrinsic_cas_loop_with_unsigned_post(self):
        msvc = self.source.split("#elif JCE_COMPILER_GCC || JCE_COMPILER_CLANG", 1)[0]
        actual = body(msvc, "jce_native_atomic_i64_add")
        self.assertEqual(actual.count("_InterlockedCompareExchange64("), 2)
        self.assertIn("(uint64_t)observed + (uint64_t)delta", actual)
        self.assertIn("if (previous == observed) return (int64_t)desired;", actual)
        self.assertNotIn("_InterlockedExchangeAdd64", msvc)
        self.assertIn("#pragma intrinsic(_InterlockedCompareExchange64)", msvc)

    def test_no_allocator_os_api_or_silent_fallback_and_alignment_is_explicit(self):
        self.assertIn('#include "jce_native_atomic_word_internal.h"', self.source)
        for name in ("malloc", "calloc", "free", "LoadLibrary", "InterlockedExchangeAdd64", "__atomic_is_lock_free"):
            self.assertNotRegex(self.source, r"\b" + name + r"\s*\(")
        self.assertIn('#error "JCE native integer MSVC architecture', self.source)
        self.assertIn('#error "JCE native integer atomics require', self.source)
        self.assertIn("objects aligned to 4/8 bytes", self.source)
        self.assertIn("__declspec(align(8))", self.source)
        self.assertIn("__attribute__((aligned(8)))", self.source)

    def test_real_header_fixture_has_post_wrap_precision_and_state_assertions(self):
        self.assertIn('#include "os/platform/jce_native_atomic_integer_internal.h"', self.control)
        for text in ("INT32_MAX", "INT32_MIN", "INT64_MAX", "INT64_MIN", "9007199254740993",
                     "jce_native_atomic_i32_add", "jce_native_atomic_i64_add",
                     "jce_native_atomic_word_load_acquire", "jce_native_atomic_word_store_release",
                     "jce_native_atomic_word_compare_exchange_acq_rel", "CHECK_ATOMIC(value32 ==", "CHECK_ATOMIC(value64 =="):
            self.assertIn(text, self.control)
        self.assertIn("int main(int argc, char **argv)", self.control)
        self.assertIn("JCE_XP_FLECS_ATOMICS_OK", self.control)
        self.assertIn("JCE_XP_FLECS_ATOMICS_ASSERT_FAILED", self.control)
        self.assertNotIn("#define jce_native_atomic_", self.control)


if __name__ == "__main__":
    unittest.main()
