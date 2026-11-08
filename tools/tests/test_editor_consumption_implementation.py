"""Exercise explicit implementation-call closure without API declaration inflation."""
from pathlib import Path
import tempfile
import unittest
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lint.check_editor_consumption import included_implementation_identifiers

class ImplementationConsumptionTests(unittest.TestCase):
    def test_explicit_host_calls_are_reached(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); (root/'policy.inc.h').write_text('static void policy(void){jce_runtime_pause_audio(0);}',encoding='utf-8')
            self.assertIn('jce_runtime_pause_audio',included_implementation_identifiers('#include <jce/policy.inc.h>',root))
    def test_public_declaration_is_not_consumption(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); (root/'api.h').write_text('JCE_API void jce_unconsumed(void);',encoding='utf-8')
            self.assertNotIn('jce_unconsumed',included_implementation_identifiers('#include <jce/api.h>',root))
            (root/'policy.inc.h').write_text('JCE_API void jce_unconsumed(void);',encoding='utf-8')
            self.assertNotIn('jce_unconsumed',included_implementation_identifiers('#include <jce/policy.inc.h>',root))
    def test_transitive_calls_and_cycle(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            (root/'a.inc.h').write_text('#include <jce/b.inc.h>\nstatic void a(void){jce_first();}',encoding='utf-8')
            (root/'b.inc.h').write_text('#include <jce/a.inc.h>\nstatic void b(void){jce_second();}',encoding='utf-8')
            self.assertEqual(included_implementation_identifiers('#include <jce/a.inc.h>',root),{'jce_first','jce_second'})
    def test_missing_and_escaping_paths_cannot_claim_calls(self):
        with tempfile.TemporaryDirectory() as tmp:
            outer=Path(tmp); root=outer/'include'; root.mkdir()
            (outer/'outside.inc.h').write_text('static void f(void){jce_outside();}',encoding='utf-8')
            self.assertEqual(included_implementation_identifiers('#include <jce/../outside.inc.h>\n#include <jce/missing.inc.h>',root),set())


    def test_commented_root_includes_cannot_supply_implementation_calls(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            (root/'policy.inc.h').write_text('static void f(void){jce_comment_only();}',encoding='utf-8')
            for source in ('/*\n#include <jce/policy.inc.h>\n*/',
                           '// #include <jce/policy.inc.h>\n',
                           '// continued comment\\\n#include <jce/policy.inc.h>\n',
                           '/* unterminated\n#include <jce/policy.inc.h>'):
                with self.subTest(source=source):
                    self.assertEqual(included_implementation_identifiers(source,root),set())
    def test_transitive_comments_are_ignored_and_quoted_comment_text_keeps_real_include(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            (root/'leaf.inc.h').write_text('static void f(void){jce_hidden_leaf();}',encoding='utf-8')
            (root/'live.inc.h').write_text('static void f(void){jce_live_leaf();}',encoding='utf-8')
            (root/'policy.inc.h').write_text(
                '/*\n#include <jce/leaf.inc.h>\n*/\n'
                '// #include <jce/leaf.inc.h>\n'
                'const char *url="http://example"; const char *marker="/*";\n'
                '#include /* real directive */ "jce/live.inc.h"\n',encoding='utf-8')
            self.assertEqual(included_implementation_identifiers('#include "jce/policy.inc.h"',root),
                             {'jce_live_leaf'})

if __name__=='__main__':unittest.main()
