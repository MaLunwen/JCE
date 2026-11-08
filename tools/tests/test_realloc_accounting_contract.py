"""Production realloc ordering controls; native failure witness is separate."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[2]


def valid_transaction(body):
    failure=body.find('if (!p) return NULL;')
    release=body.find('track_on_free_size(old_size);')
    allocate=body.find('jce__alloc_account_alloc(p, size);')
    call=body.find('void *p = mi_realloc(ptr, size);')
    snapshot=body.find('mi_usable_size(ptr)')
    return 0<=snapshot<call<failure<release<allocate and 'jce__alloc_account_free(ptr)' not in body


class ReallocAccountingContract(unittest.TestCase):
    def test_production_success_accounting_is_after_failure_guard(self):
        source=(ROOT/'engine/src/os/core/jce_memory.c').read_text(encoding='utf-8')
        body=source.split('void *jce__realloc_tracked(',1)[1].split('void jce__free_tracked',1)[0]
        self.assertTrue(valid_transaction(body))
        self.assertFalse(valid_transaction(body.replace('if (!p) return NULL;','')))
        self.assertFalse(valid_transaction(body.replace('void *p = mi_realloc(ptr, size);',
            'jce__alloc_account_free(ptr); void *p = mi_realloc(ptr, size);')))
        self.assertIn('jce__free_tracked(ptr);\n        return NULL;',body)
        self.assertIn('JCE_PROFILE_FREE((void *)old_address)',body)

    def test_default_frontend_reuses_one_transaction(self):
        source=(ROOT/'engine/src/os/core/jce_allocator.c').read_text(encoding='utf-8')
        body=source.split('static void *default_realloc(',1)[1].split('static void default_free',1)[0]
        self.assertIn('return jce__realloc_tracked(ptr, new_size);',body)
        self.assertNotIn('mi_realloc(',body)


if __name__=='__main__':unittest.main()
