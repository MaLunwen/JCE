"""Actual memory observation, split export and localized unavailable contracts.

These source controls retain real-source mutations; native snapshot publication
is exercised separately by tests/editor/test_jce_memory_snapshot.cpp.
"""
import json
import re
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[2]


def snapshot_assignments(source, values):
    body=source.split('bool jce_mem_profile_get_process_stats(',1)[1]
    output={}
    for field, expression in re.findall(r'out->(\w+)\s*=\s*([^;]+);',body):
        expression=expression.replace('(uint64_t)','').strip()
        if expression.isdecimal():output[field]=int(expression)
        elif expression in values:output[field]=values[expression]
        else:raise ValueError('unreviewed process snapshot expression')
    return output


def tagged_contract_errors(panel, snapshot):
    """Inspect the real split exporter and its panel caller without dumping UI."""
    errors=[]
    for call in ('jce_mem_profile_get_aggregate_totals(&st.total_cur, &st.total_peak);',
                 'jce_mem_profile_get_total(nullptr, &st.tag_peak_sum);'):
        if call not in panel:errors.append('tagged_observation_wire')
    if 'jce_mem_profile_get_total(&st.total_cur, &st.total_peak)' in panel:
        errors.append('aliased_tagged_peak')
    if 'jce_editor_memory_snapshot_publish(st,st.snapshot_path,sizeof st.snapshot_path)' not in panel:
        errors.append('publication_wire')
    if 'jce_editor_memory_snapshot_encode(snapshot,body,sizeof body,length)' not in snapshot:
        errors.append('encoding_wire')
    if 'inline bool jce_editor_memory_snapshot_encode(' not in snapshot:
        errors.append('encoding_owner')
    encoder=snapshot.split('inline bool jce_editor_memory_snapshot_encode(',1)[-1].split(
        'enum class JceEditorMemorySnapshotResult',1)[0]
    for field in ('voluntary-tagged','complete_process_coverage\\\":false',
                  'current_bytes','simultaneous_peak_bytes','tag_peak_sum_bytes'):
        if field not in encoder:errors.append('tagged_scope_or_field')
    values=re.findall(r'static_cast<unsigned long long>\(snapshot\.(total_cur|total_peak|tag_peak_sum)\)',encoder)
    if values!=['total_cur','total_peak','tag_peak_sum']:errors.append('tagged_value_order')
    fields=re.findall(r'\\"(current_bytes|simultaneous_peak_bytes|tag_peak_sum_bytes)\\":%llu',encoder)
    if fields!=['current_bytes','simultaneous_peak_bytes','tag_peak_sum_bytes']:errors.append('tagged_field_order')
    return errors


def unavailable_display_errors(panel, messages):
    errors=[]
    for field in ('reserved','committed'):
        pattern=(r'if\s*\(st\.process_valid\s*&&\s*st\.proc\.mimalloc_'+field+
            r'_bytes\)\s*format_mb\(buf,\s*sizeof\(buf\),\s*st\.proc\.mimalloc_'+field+
            r'_bytes\);\s*else\s*std::snprintf\(buf,\s*sizeof\s*buf,\s*"%s",\s*'
            r'jce_editor_i18n\("memoryProfiler\.unavailable"\)\);')
        if not re.search(pattern,panel):errors.append(field)
    if messages.get('memoryProfiler.unavailable')!='N/A':errors.append('unavailable_label')
    return errors


class MemorySnapshotContract(unittest.TestCase):
    def test_real_production_assignments_do_not_alias_process_or_peak(self):
        source=(ROOT/'engine/src/os/core/jce_mem_profile.c').read_text(encoding='utf-8')
        values={'cur_rss':1234567,'peak_rss':2345678,
                'cur_commit':3456789,'peak_commit':4567890}
        actual=snapshot_assignments(source,values)
        self.assertEqual(actual,{'process_rss_bytes':1234567,
            'process_committed_bytes':3456789,'mimalloc_reserved_bytes':0,
            'mimalloc_committed_bytes':0})
        for field,value in (('mimalloc_reserved_bytes','peak_commit'),
                            ('mimalloc_committed_bytes','cur_commit')):
            bad=re.sub(r'(out->'+field+r'\s*=)\s*0;',r'\1 (uint64_t)'+value+';',source)
            self.assertNotEqual(snapshot_assignments(bad,values),actual)

    def test_accounting_lock_has_no_fallible_creation_or_callback(self):
        source=(ROOT/'engine/src/os/core/jce_mem_profile.c').read_text(encoding='utf-8')
        self.assertIn('SDL_SpinLock lock;',source)
        self.assertNotIn('SDL_CreateMutex',source)
        self.assertNotIn('SDL_LockMutex',source)
        self.assertIn('SDL_ShouldInit(&g_profile.init)',source)
        self.assertIn('SDL_SetInitialized(&g_profile.init, true)',source)
        sections=re.findall(r'SDL_LockSpinlock\(&g_profile.lock\);(.*?)SDL_UnlockSpinlock\(&g_profile.lock\);',source,re.S)
        self.assertEqual(len(sections),8)
        # All actual critical sections must contain only bounded accounting;
        # profiler callbacks remain after unlock, even if profiling is enabled.
        for body in sections:
            for forbidden in ('JCE_PROFILE','malloc','calloc','realloc','LOG_',
                              'SDL_Wait','SDL_Create','mi_process_info'):
                self.assertNotIn(forbidden,body)
        # The no-OOM-path oracle must reject the old fallible initialization.
        def has_fallible_lock(text):
            return bool(re.search(r'\bSDL_(?:CreateMutex|LockMutex)\s*\(',text))
        self.assertFalse(has_fallible_lock(source))
        self.assertTrue(has_fallible_lock(source.replace(
            'if (SDL_ShouldInit(&g_profile.init)) {',
            'if (SDL_ShouldInit(&g_profile.init)) { g_profile.lock = SDL_CreateMutex();')))


    def editor_sources(self):
        panel=(ROOT/'editor/src/panels/jce_panel_memory_profiler.cpp').read_text(encoding='utf-8')
        snapshot=(ROOT/'editor/src/panels/jce_panel_memory_snapshot.h').read_text(encoding='utf-8')
        messages=json.loads((ROOT/'editor/resources/assets/i18n/en.json').read_text(encoding='utf-8'))
        return panel,snapshot,messages

    def test_editor_consumes_and_exports_distinct_tagged_peak_semantics(self):
        panel,snapshot,_=self.editor_sources()
        self.assertEqual(tagged_contract_errors(panel,snapshot),[])
        # Mutate the real exporter and panel wiring, not a parallel implementation.
        for old,new,reason in (
            ('static_cast<unsigned long long>(snapshot.total_peak)',
             'static_cast<unsigned long long>(snapshot.tag_peak_sum)','tagged_value_order'),
            ('static_cast<unsigned long long>(snapshot.tag_peak_sum)',
             'static_cast<unsigned long long>(snapshot.total_peak)','tagged_value_order')):
            self.assertEqual(snapshot.count(old),1,'expected one actual tagged exporter value')
            with self.subTest(reason=reason,new=new):
                self.assertIn(reason,tagged_contract_errors(panel,snapshot.replace(old,new,1)))
        actual='jce_editor_memory_snapshot_publish(st,st.snapshot_path,sizeof st.snapshot_path)'
        self.assertEqual(panel.count(actual),1,'expected one actual publication call')
        self.assertIn('publication_wire',tagged_contract_errors(panel.replace(actual,'old_publish(st)',1),snapshot))
        old='complete_process_coverage\\\":false'
        self.assertEqual(snapshot.count(old),1,'expected one actual partial-coverage declaration')
        self.assertIn('tagged_scope_or_field',tagged_contract_errors(
            panel,snapshot.replace(old,'complete_process_coverage\\\":true',1)))

    def test_unavailable_heap_display_and_public_documentation(self):
        header=(ROOT/'engine/include/jce/os/core/jce_mem_profile.h').read_text(encoding='utf-8')
        self.assertTrue('0 means unavailable, not an empty heap' in header,
                        'public unavailable-allocator contract must remain explicit')
        panel,_,messages=self.editor_sources()
        self.assertEqual(unavailable_display_errors(panel,messages),[])
        # An invented zero or a process-commit alias cannot pass as heap telemetry.
        for field in ('reserved','committed'):
            with self.subTest(field=field):
                old='st.proc.mimalloc_'+field+'_bytes'
                self.assertEqual(panel.count(old),2,'one availability guard and one format value')
                self.assertIn(field,unavailable_display_errors(
                    panel.replace(old,'st.proc.process_committed_bytes'),messages))
        mutated=dict(messages);mutated['memoryProfiler.unavailable']='0.00 MiB'
        self.assertIn('unavailable_label',unavailable_display_errors(panel,mutated))


if __name__=='__main__':unittest.main()
