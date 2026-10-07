"""Inline definitions resolve; their undeclared callees must remain findings."""
import unittest
from tools.lint.check_shipped_main_declarations import CALL_RE, defined_functions, strip_comments_and_strings


class ShippedDeclarations(unittest.TestCase):
    def test_static_and_sdk_inline_definitions_without_promoting_calls(self):
        source = """
static void jce_local(void) { jce_missing(); }
JCE_INLINE const int *jce_observe(void) { jce_missing(); return 0; }
/* JCE_INLINE int jce_comment(void); */
void app(void) { jce_observe(); jce_other_missing(); }
"""
        own = defined_functions(source)
        called = set(CALL_RE.findall(strip_comments_and_strings(source)))
        self.assertEqual(own, {"jce_local", "jce_observe"})
        self.assertEqual(called - own, {"jce_missing", "jce_other_missing"})


if __name__ == "__main__":
    unittest.main()
