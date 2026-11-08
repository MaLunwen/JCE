"""Exact walker/capture port controls; no live Windows/process-memory proof."""
from pathlib import Path
import sys
import unittest
import ctypes
import io
import json
import tempfile
from contextlib import redirect_stdout
from unittest.mock import patch

if __package__ in (None, ""):
    sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.memory_commit_probe import (MEM_COMMIT,MEM_RESERVE,MEM_FREE,UINT64_END,
    capture,walk_regions,mapped_allocation_names,working_set_page_metadata)
from tools import memory_commit_probe as probe
from tools import windows_memory_reader as native
from tools.windows_memory_reader import ObservationError, WindowsReader, MAPPED_FILE_NAME_CHARS

PAGE=4096


def region(base, state=MEM_COMMIT, kind=0x20000, allocation=None, size=PAGE):
    return dict(base=base,size=size,state=state,type=kind,
                allocation_base=base if allocation is None else allocation)


class Port:
    def __init__(self):
        self.bounds=dict(minimum_address=0,maximum_address=3*PAGE-1,page_bytes=PAGE,pointer_bits=64)
        self.regions={0:region(0),PAGE:region(PAGE,MEM_RESERVE,allocation=0),
                      2*PAGE:region(2*PAGE,MEM_FREE)}
        self.closed=0; self.queries=[]; self.samples=0; self.identities=0; self.tick=0
        self.fail_query=None; self.fail_sample=False; self.mismatch=False; self.exit=False
    def __enter__(self): return self
    def __exit__(self,*_): self.closed+=1
    def alive(self): return not self.exit
    def identity(self):
        self.identities+=1
        return dict(pid=42,exe='C:/fixture/app.exe',creation_filetime_100ns=200 if self.mismatch and self.identities>1 else 100,alive=self.alive())
    def qpc(self):
        self.tick+=1; return dict(ticks=self.tick,frequency=1000)
    def sample(self):
        self.samples+=1
        if self.fail_sample: raise ObservationError('memory','denied',5)
        return dict(rss_bytes=20,private_commit_bytes=30,cpu_time_100ns=100,thread_count=None)
    def region(self,address):
        self.queries.append(address)
        if address==self.fail_query: raise ObservationError('VirtualQueryEx','failed',5)
        return self.regions[address]


class CommitWalkerTests(unittest.TestCase):
    def walk(self,regions,**options):
        return walk_regions(lambda address:regions[address],len(regions)*PAGE,clock=lambda:0,**options)
    def test_actual_walker_separates_types_reserve_and_allocation_groups(self):
        rows={0:region(0),PAGE:region(PAGE,allocation=0),2*PAGE:region(2*PAGE,MEM_RESERVE,allocation=0),3*PAGE:region(3*PAGE,kind=0x40000),4*PAGE:region(4*PAGE,kind=0x1000000),5*PAGE:region(5*PAGE,MEM_FREE)}
        result=self.walk(rows); total=result['full_totals']
        self.assertTrue(result['address_space_complete'])
        self.assertEqual(total['committed_by_type'],dict(MEM_PRIVATE=2*PAGE,MEM_MAPPED=PAGE,MEM_IMAGE=PAGE))
        self.assertEqual(total['reserved_bytes'],PAGE)
        group=next(g for g in total['allocation_groups'] if g['allocation_base']=='0x0')
        self.assertEqual((group['regions'],group['committed_bytes'],group['reserved_bytes']),(3,2*PAGE,PAGE))
    def test_free_undefined_fields_not_consumed_and_reserved_type_zero_explicit(self):
        rows={0:dict(base=0,size=PAGE,state=MEM_FREE),PAGE:region(PAGE,MEM_RESERVE,0)}
        result=self.walk(rows)
        self.assertTrue(result['address_space_complete'])
        self.assertEqual(result['full_totals']['reserved_by_type']['UNDEFINED_RESERVED'],PAGE)
    def test_invalid_region_never_promotes_prefix_to_full_totals(self):
        for change in (dict(size=0),dict(size=True),dict(base=1),dict(size=-1),dict(state=9),dict(type=0),dict(allocation_base=PAGE),dict(base=PAGE),dict(base=-1)):
            with self.subTest(change=change):
                value=region(0); value.update(change)
                result=walk_regions(lambda _:value,PAGE,clock=lambda:0)
                self.assertFalse(result['address_space_complete']); self.assertIsNone(result['full_totals'])
                self.assertEqual(result['observed_prefix']['committed_bytes'],0)
    def test_overflow_and_nonadvancing_are_real_failures(self):
        overflow=walk_regions(lambda _:region(0,size=UINT64_END+PAGE),PAGE,clock=lambda:0)
        self.assertIn('invalid region size',overflow['error']['reason'])
        rows={0:region(0),PAGE:region(0)}; result=self.walk(rows)
        self.assertIn('nonadvancing',result['error']['reason']); self.assertIsNone(result['full_totals'])
        self.assertEqual(result['observed_prefix']['committed_bytes'],PAGE)
        def actual_end_overflow(address):
            return region(0,MEM_FREE,size=UINT64_END-PAGE) if address==0 else region(UINT64_END-PAGE,size=2*PAGE)
        result=walk_regions(actual_end_overflow,UINT64_END,clock=lambda:0)
        self.assertIn('region end overflow',result['error']['reason']); self.assertIsNone(result['full_totals'])
    def test_query_failure_preserves_prior_observation_and_error(self):
        def query(address):
            if address: raise ObservationError('VirtualQueryEx','access denied',5)
            return region(0)
        result=walk_regions(query,2*PAGE,clock=lambda:0)
        self.assertEqual(result['query_calls'],2); self.assertEqual(result['error']['win32_error'],5)
        self.assertEqual(result['observed_prefix']['committed_bytes'],PAGE); self.assertIsNone(result['full_totals'])
    def test_exit_and_time_region_group_budgets_are_not_complete(self):
        rows={0:region(0),PAGE:region(PAGE)}
        for options in (dict(max_regions=1),dict(max_groups=1),dict(alive=lambda:False)):
            with self.subTest(options=list(options)):
                self.assertFalse(self.walk(rows,**options)['address_space_complete'])
        liveness=iter((True,False))
        result=walk_regions(lambda a:rows[a],2*PAGE,clock=lambda:0,alive=lambda:next(liveness))
        self.assertEqual(result['error']['stage'],'process_exit'); self.assertEqual(result['observed_prefix']['committed_bytes'],PAGE)
        times=iter((0,0,3))
        result=walk_regions(lambda a:rows[a],2*PAGE,clock=lambda:next(times),max_seconds=2)
        self.assertEqual(result['error']['stage'],'budget'); self.assertEqual(result['query_calls'],1)
    def test_bad_clock_and_bounds_do_not_loop(self):
        for end,options in ((0,{}),(PAGE+1,{}),(PAGE,dict(page_bytes=3)),(PAGE,dict(max_regions=0)),(PAGE,dict(max_seconds=float('nan')))):
            with self.subTest(end=end,options=options):
                with self.assertRaises(ValueError): walk_regions(lambda _:region(0),end,clock=lambda:0,**options)
        ticks=iter((1,0)); result=walk_regions(lambda _:region(0),PAGE,clock=lambda:next(ticks))
        self.assertEqual(result['error']['stage'],'clock')
    def test_documented_address_bound_clips_last_region(self):
        result=walk_regions(lambda _:region(0,size=2*PAGE),PAGE,clock=lambda:0)
        self.assertTrue(result['address_space_complete']); self.assertTrue(result['clipped_final_region'])
        self.assertEqual(result['full_totals']['committed_bytes'],PAGE)


class CapturePortTests(unittest.TestCase):
    def observe(self,port,**options):
        return capture(42,'C:/fixture/app.exe',reader_factory=lambda pid:port,**options)
    def test_same_handle_identity_sampling_and_qpc_bracket(self):
        port=Port(); result=self.observe(port,creation=100)
        self.assertEqual(result['status'],'OBSERVED_NONATOMIC_COMMIT_REGIONS')
        self.assertEqual(port.closed,1); self.assertEqual(port.samples,2)
        self.assertEqual(port.queries,[0,PAGE,2*PAGE]); self.assertEqual(result['identity_before'],result['identity_after'])
        self.assertLess(result['qpc_begin']['ticks'],result['qpc_end']['ticks'])
        self.assertTrue(result['caller_creation_witness'])
    def test_pid_exe_creation_and_exit_failure_close_once(self):
        for scenario in ('pid','exe','creation','exit'):
            with self.subTest(scenario=scenario):
                port=Port()
                if scenario=='pid': port.identity=lambda:dict(pid=43,exe='C:/fixture/app.exe',creation_filetime_100ns=100,alive=True)
                if scenario=='exe': port.identity=lambda:dict(pid=42,exe='C:/wrong.exe',creation_filetime_100ns=100,alive=True)
                if scenario=='exit': port.exit=True
                result=self.observe(port,creation=200 if scenario=='creation' else None)
                self.assertEqual(result['status'],'UNAVAILABLE'); self.assertEqual(port.queries,[]); self.assertEqual(port.closed,1)
    def test_identity_change_after_walk_cannot_certify_success(self):
        port=Port(); port.mismatch=True; result=self.observe(port)
        self.assertEqual(result['status'],'PARTIAL'); self.assertEqual(port.closed,1)
        self.assertIn('creation FILETIME',result['errors'][0]['reason'])
        self.assertIsNone(result['walk']['full_totals']); self.assertFalse(result['same_target_identity_verified'])
        self.assertIsNotNone(result['qpc_end'])
    def test_sample_and_region_failure_are_explicit_not_zero(self):
        port=Port(); port.fail_sample=True; result=self.observe(port)
        self.assertEqual(result['status'],'PARTIAL'); self.assertIsNone(result['os_before']); self.assertEqual(len(result['errors']),2)
        port=Port(); port.fail_query=PAGE; result=self.observe(port)
        self.assertEqual(result['status'],'PARTIAL'); self.assertIsNone(result['walk']['full_totals']); self.assertEqual(port.closed,1)
    def test_qpc_frequency_change_and_invalid_native_bounds_close_once(self):
        port=Port()
        def changing_qpc():
            port.tick+=1; return dict(ticks=port.tick,frequency=1000 if port.tick==1 else 2000)
        port.qpc=changing_qpc; result=self.observe(port)
        self.assertNotEqual(result['status'],'OBSERVED_NONATOMIC_COMMIT_REGIONS'); self.assertEqual(port.closed,1)
        port=Port(); port.bounds['pointer_bits']=32; result=self.observe(port)
        self.assertEqual(result['status'],'UNAVAILABLE'); self.assertEqual(port.queries,[]); self.assertEqual(port.closed,1)
    def test_invalid_sample_cannot_be_promoted_and_no_zero_is_fabricated(self):
        port=Port(); port.sample=lambda:dict(rss_bytes=-1,private_commit_bytes=True,cpu_time_100ns=100)
        result=self.observe(port)
        self.assertEqual(result['status'],'PARTIAL'); self.assertIsNone(result['os_before'])
        self.assertEqual([e['stage'] for e in result['errors']],['sample_before','sample_after'])
    def test_actual_native_close_method_submits_exactly_once_without_os_calls(self):
        class Api:
            count=0
            def CloseHandle(self,handle):
                self.count+=1; self.last=handle; return True
        reader=WindowsReader.__new__(WindowsReader); reader.api=Api(); reader.handle=123
        reader.close(); reader.close()
        self.assertEqual(reader.api.count,1); self.assertEqual(reader.api.last,123); self.assertIsNone(reader.handle)
    def test_input_admission_and_factory_failure_do_not_fake_measurement(self):
        for pid,exe in ((True,'C:/fixture/app.exe'),(0,'C:/fixture/app.exe'),(42,'relative.exe')):
            with self.assertRaises(ValueError): capture(pid,exe,reader_factory=lambda _:self.fail('must not open'))
        def unavailable(_): raise ObservationError('OpenProcess','denied',5)
        result=capture(42,'C:/fixture/app.exe',reader_factory=unavailable)
        self.assertEqual(result['status'],'UNAVAILABLE'); self.assertIsNone(result['walk'])
        self.assertEqual(result['errors'][0]['win32_error'],5)



class MappedNameTests(unittest.TestCase):
    @staticmethod
    def mapped_port():
        port=Port()
        port.regions={0:region(0,kind=0x40000),PAGE:region(PAGE,kind=0x40000,allocation=0),2*PAGE:region(2*PAGE)}
        port.names=[]
        def name(address):
            port.names.append(address)
            return r'\Device\HarddiskVolume3\cache\observed.nvph'
        port.mapped_file_name=name
        return port

    def observe(self,port,requested):
        seen=[]
        def factory(pid,**kwargs):
            seen.append((pid,kwargs))
            return port
        result=capture(42,'C:/fixture/app.exe',reader_factory=factory,mapped_file_names=requested)
        self.assertEqual(seen,[(42,{'mapped_file_names':True})] if requested else [(42,{})])
        self.assertEqual(port.closed,1)
        return result

    def test_opt_in_uses_same_port_and_preserves_actual_walk_totals(self):
        ordinary=self.mapped_port(); baseline=self.observe(ordinary,False)
        self.assertEqual(ordinary.names,[])
        self.assertEqual(baseline['mapped_file_metadata']['status'],'NOT_REQUESTED')
        named=self.mapped_port(); result=self.observe(named,True)
        self.assertEqual(result['status'],baseline['status'])
        self.assertEqual(result['walk'],baseline['walk'])
        self.assertEqual(named.names,[0])
        meta=result['mapped_file_metadata']
        self.assertEqual(meta['status'],'COMPLETE_MAPPED_NAMES')
        self.assertTrue(meta['same_target_identity_verified']);self.assertTrue(meta['qpc_window_verified'])
        self.assertEqual(meta['rows'][0]['committed_bytes'],2*PAGE)
        self.assertIn('Device',meta['rows'][0]['name'])
        self.assertIn('not heap',result['scope'].lower())
        self.assertEqual(result['handle_rights'],'PROCESS_QUERY_INFORMATION | SYNCHRONIZE; no VM_READ/VM_WRITE')

    def test_top32_complete_groups_are_ranked_without_claiming_unqueried_names(self):
        # One larger two-region mapped allocation; 33 single-region mapped
        # allocations plus a PRIVATE allocation. Complete real walker first.
        rows={0:region(0,kind=0x40000),PAGE:region(PAGE,kind=0x40000,allocation=0)}
        for index in range(2,35): rows[index*PAGE]=region(index*PAGE,kind=0x40000)
        rows[35*PAGE]=region(35*PAGE)
        walked=walk_regions(lambda address:rows[address],36*PAGE,clock=lambda:0)
        before=json.dumps(walked,sort_keys=True)
        seen=[]
        def query(address): seen.append(address); return r'\Device\fixture\file'+str(address)
        meta=mapped_allocation_names(walked,query,clock=lambda:0)
        self.assertEqual(len(seen),32); self.assertEqual(seen[0],0)
        self.assertNotIn(35*PAGE,seen)
        self.assertEqual(meta['total_mapped_groups'],34)
        self.assertEqual(meta['unqueried_group_count'],2)
        self.assertEqual(meta['unqueried_committed_bytes'],2*PAGE)
        self.assertEqual(meta['status'],'PARTIAL_MAPPED_NAMES');self.assertFalse(meta['name_coverage_complete'])
        self.assertEqual(json.dumps(walked,sort_keys=True),before)

    def test_incomplete_walk_no_mapped_groups_and_identity_change_stay_explicit(self):
        port=self.mapped_port();port.fail_query=PAGE
        result=self.observe(port,True)
        self.assertEqual(result['status'],'PARTIAL');self.assertEqual(port.names,[])
        self.assertEqual(result['mapped_file_metadata']['status'],'UNAVAILABLE')
        empty=Port(); result=self.observe(empty,True)
        self.assertEqual(result['mapped_file_metadata']['status'],'NOT_EXERCISED')
        port=self.mapped_port();port.mismatch=True
        result=self.observe(port,True)
        self.assertIsNone(result['walk']['full_totals'])
        self.assertEqual(result['mapped_file_metadata']['status'],'UNAVAILABLE')
        self.assertFalse(result['mapped_file_metadata']['name_coverage_complete'])
        self.assertIn('diagnostic only',result['mapped_file_metadata']['certificate_error'])

    def test_name_api_failure_and_malformed_response_never_become_heap_labels(self):
        for response in (None,True,'',r'\Device'+'\0bad','x'*MAPPED_FILE_NAME_CHARS):
            port=self.mapped_port();port.mapped_file_name=lambda _,value=response:value
            result=self.observe(port,True)
            self.assertEqual(result['status'],'OBSERVED_NONATOMIC_COMMIT_REGIONS')
            self.assertEqual(result['walk']['full_totals']['committed_by_type']['MEM_MAPPED'],2*PAGE)
            self.assertEqual(result['mapped_file_metadata']['status'],'UNAVAILABLE')
            self.assertIsNone(result['mapped_file_metadata']['rows'][0]['name'])
        port=self.mapped_port()
        def failed(_): raise ObservationError('K32GetMappedFileNameW','no mapped file at current address',1006)
        port.mapped_file_name=failed;result=self.observe(port,True)
        error=result['mapped_file_metadata']['rows'][0]['error']
        self.assertEqual(error['win32_error'],1006);self.assertEqual(result['errors'],[])
        self.assertEqual(result['mapped_file_metadata']['rows'][0]['status'],'UNAVAILABLE')
        self.assertFalse(result['mapped_file_metadata']['name_coverage_complete'])

    def test_metadata_time_and_liveness_stop_queries_without_prefix_certificate(self):
        rows={i*PAGE:region(i*PAGE,kind=0x40000) for i in range(3)}
        walked=walk_regions(lambda address:rows[address],3*PAGE,clock=lambda:0)
        seen=[]
        def query(address):seen.append(address);return r'\Device\file'
        ticks=iter((0,0,1))
        meta=mapped_allocation_names(walked,query,clock=lambda:next(ticks))
        self.assertEqual(seen,[0]); self.assertEqual(meta['unqueried_group_count'],2)
        self.assertEqual([row['status'] for row in meta['rows']],['OBSERVED','NOT_QUERIED','NOT_QUERIED'])
        self.assertFalse(meta['name_coverage_complete'])
        seen.clear();meta=mapped_allocation_names(walked,query,clock=lambda:0,alive=lambda:False)
        self.assertEqual(seen,[]);self.assertEqual(meta['status'],'UNAVAILABLE')
        self.assertTrue(walked['address_space_complete'])

    def test_actual_native_method_has_fixed_buffer_error_truncation_and_one_handle(self):
        reader=WindowsReader.__new__(WindowsReader)
        reader.handle=123;reader.mapped_file_names=True;reader._mapped_file_api_unavailable=None
        last=[0];calls=[]
        def set_error(value):last[0]=value
        def api(handle,address,buffer,capacity):
            calls.append((handle,address.value,capacity))
            buffer.value=r'\Device\fixture\file'
            return len(buffer.value)
        reader._mapped_file_api=api
        with patch.object(native.ctypes,'set_last_error',set_error,create=True),patch.object(native.ctypes,'get_last_error',lambda:last[0],create=True):
            self.assertEqual(reader.mapped_file_name(PAGE),r'\Device\fixture\file')
            self.assertEqual(calls,[(123,PAGE,MAPPED_FILE_NAME_CHARS)])
            def fail(handle,address,buffer,capacity):calls.append((handle,address.value,capacity));last[0]=1006;return 0
            reader._mapped_file_api=fail
            with self.assertRaises(ObservationError) as caught:reader.mapped_file_name(PAGE)
            self.assertEqual(caught.exception.code,1006)
            def truncate(handle,address,buffer,capacity):calls.append((handle,address.value,capacity));last[0]=122;return capacity
            reader._mapped_file_api=truncate
            with self.assertRaises(ObservationError) as caught:reader.mapped_file_name(PAGE)
            self.assertTrue(caught.exception.truncated);self.assertEqual(caught.exception.code,122)
            self.assertEqual(len(calls),3) # No unbounded retry or second handle.
            reader._mapped_file_api=None
            with self.assertRaisesRegex(ObservationError,'optional API unavailable') as caught:reader.mapped_file_name(PAGE)
            self.assertIsNone(caught.exception.code) # No native call: no invented Win32 error.
            reader.mapped_file_names=False
            with self.assertRaisesRegex(ObservationError,'not requested'):reader.mapped_file_name(PAGE)
            self.assertEqual(len(calls),3)

    @unittest.skipUnless(sys.platform=='win32','native Win64 structure layout required; typed API only, no process opened')
    def test_actual_constructor_preserves_minimum_rights_and_close_once(self):
        class Function:
            def __init__(self,call):self.call=call
            def __call__(self,*args):return self.call(*args)
        class API:
            def __init__(self):
                self.opened=[];self.closed=[];self.lookups=[]
                self.OpenProcess=Function(lambda mask,inherit,pid:self.opened.append((mask,inherit,pid)) or 123)
                self.CloseHandle=Function(lambda handle:self.closed.append(handle) or True)
                self.IsWow64Process2=Function(self.machine)
                self.GetSystemInfo=Function(self.system)
                self.QueryPerformanceFrequency=Function(self.frequency)
            def __getattr__(self,name):
                self.lookups.append(name)
                return Function(lambda *args:1)
            def machine(self,handle,process,native_machine):
                process._obj.value=0;native_machine._obj.value=0x8664;return True
            def system(self,info):
                info._obj.MinimumAddress=PAGE;info._obj.MaximumAddress=3*PAGE-1;info._obj.PageSize=PAGE
            def frequency(self,frequency):frequency._obj.value=1000;return True
        for enabled in (False,True):
            api=API()
            with patch.object(native.ctypes,'WinDLL',lambda *a,**k:api):
                with WindowsReader(42,mapped_file_names=enabled) as reader:
                    self.assertEqual(reader.handle,123)
                reader.close()
            self.assertEqual(api.opened,[(0x0400|0x00100000,False,42)])
            self.assertEqual(api.closed,[123])
            self.assertEqual('K32GetMappedFileNameW' in api.lookups,enabled)

    def test_actual_cli_flag_reaches_capture_without_new_child_or_fake_ctest(self):
        original=probe.capture
        with tempfile.TemporaryDirectory(prefix='jce-mapped-cli-') as folder:
            for enabled in (False,True):
                port=self.mapped_port(); seen=[]
                def factory(pid,**kwargs):seen.append(kwargs);return port
                def actual_capture(*args,**kwargs):return original(*args,reader_factory=factory,**kwargs)
                output=Path(folder)/str(enabled)
                argv=['memory_commit_probe.py','--pid','42','--exe','C:/fixture/app.exe','--out',str(output)]
                if enabled:argv.append('--mapped-file-names')
                with patch.object(sys,'argv',argv),patch.object(probe,'capture',actual_capture),redirect_stdout(io.StringIO()):
                    self.assertEqual(probe.main(),0)
                result=json.loads((output/'report.json').read_text(encoding='utf-8'))
                self.assertEqual(result['mapped_file_metadata']['requested'],enabled)
                self.assertEqual(seen,[{'mapped_file_names':True}] if enabled else [{}])
                self.assertEqual(port.closed,1)
        for value in (0,1,'yes',None):
            with self.assertRaises(ValueError):capture(42,'C:/fixture/app.exe',mapped_file_names=value,reader_factory=lambda _:self.fail('must not open'))


class PageMetadataTests(unittest.TestCase):
    @staticmethod
    def page_port():
        port=Port()
        port.bounds.update(minimum_address=0,maximum_address=6*PAGE-1)
        # One PRIVATE allocation has a reserved hole. IMAGE with Shared=0 must
        # remain walk-observed IMAGE, not be reclassified as a private heap.
        port.regions={0:region(0),PAGE:region(PAGE,MEM_RESERVE,allocation=0),
            2*PAGE:region(2*PAGE,allocation=0),3*PAGE:region(3*PAGE,kind=0x40000),
            4*PAGE:region(4*PAGE,kind=0x1000000),5*PAGE:region(5*PAGE,MEM_FREE)}
        words={0:1,2*PAGE:(1<<15)|(1<<22),3*PAGE:1|(1<<15),
            4*PAGE:1|(1<<22)|(1<<23)|(1<<63)}
        port.page_queries=[]
        # Consume the actual existing native method with a typed DLL port;
        # neither Windows DLL lookup nor an actual process handle is used.
        reader=WindowsReader.__new__(WindowsReader)
        reader.handle=123;reader.working_set_pages=True;reader.bounds=port.bounds
        def query(handle,buffer,size):
            addresses=[int(entry.VirtualAddress or 0) for entry in buffer]
            port.page_queries.append((handle,size,addresses))
            for entry,address in zip(buffer,addresses):entry.Flags=words[address]
            return True
        reader._working_set_api=query
        port.working_set_page_flags=reader.working_set_page_flags
        return port

    def observe(self,port,**options):
        seen=[]
        def factory(pid,**kwargs):seen.append((pid,kwargs));return port
        result=capture(42,'C:/fixture/app.exe',creation=100,reader_factory=factory,
            working_set_pages=True,**options)
        self.assertEqual(seen,[(42,{'working_set_pages':True})])
        self.assertEqual(port.closed,1)
        return result

    def assert_complete_consumer(self,owner=capture):
        port=self.page_port()
        result=owner(42,'C:/fixture/app.exe',creation=100,working_set_pages=True,
            reader_factory=lambda pid,**kwargs:port)
        meta=result['working_set_metadata']
        self.assertEqual(meta['status'],'OBSERVED_NONATOMIC_PAGE_METADATA')
        self.assertTrue(meta['metadata_complete'])
        self.assertTrue(meta['same_target_identity_verified'])
        self.assertTrue(meta['qpc_window_verified'])
        total=meta['observed']['totals']
        self.assertEqual((total['selected_pages'],total['attempted_pages'],total['observed_pages']),(4,4,4))
        self.assertEqual((total['valid_pages'],total['invalid_pages'],total['valid_page_bytes']),(3,1,3*PAGE))
        self.assertEqual((total['shareable_valid_pages'],total['unshared_valid_pages']),(1,2))
        self.assertEqual((total['locked_valid_pages'],total['large_valid_pages']),(1,1))
        self.assertEqual((total['unknown_attempted_pages'],total['unattempted_selected_pages']),(0,0))
        return port,result

    def test_actual_capture_classifies_flags_types_holes_and_raw_high_bits(self):
        port,result=self.assert_complete_consumer()
        meta=result['working_set_metadata']
        self.assertEqual(port.page_queries,[(123,64,[0,2*PAGE,3*PAGE,4*PAGE])])
        self.assertEqual(meta['observed']['by_walk_type']['MEM_PRIVATE']['invalid_pages'],1)
        self.assertEqual(meta['observed']['by_walk_type']['MEM_IMAGE']['unshared_valid_pages'],1)
        self.assertEqual(meta['observed']['allocation_groups'][0]['type'],'MEM_IMAGE')
        self.assertEqual(sum(row['pages'] for row in meta['raw_flags_histogram']),4)
        self.assertIn('0x8000000000c00001',[row['flags_hex'] for row in meta['raw_flags_histogram']])
        self.assertEqual(result['os_before']['rss_bytes'],20)
        self.assertEqual(result['os_after']['private_commit_bytes'],30)
        self.assertIn('not RSS',meta['scope'])
        self.assertEqual(result['handle_rights'],'PROCESS_QUERY_INFORMATION | SYNCHRONIZE; no VM_READ/VM_WRITE')

    def test_default_off_and_both_metadata_options_keep_one_factory_and_walk(self):
        for enabled in (False,True):
            port=self.page_port();seen=[]
            port.mapped_file_name=lambda _:r'\Device\fixture\mapped'
            def factory(pid,**kwargs):seen.append(kwargs);return port
            result=capture(42,'C:/fixture/app.exe',reader_factory=factory,
                working_set_pages=enabled,mapped_file_names=enabled)
            self.assertEqual(seen,[{'mapped_file_names':True,'working_set_pages':True}] if enabled else [{}])
            self.assertEqual(len(port.page_queries),int(enabled))
            self.assertEqual('committed_spans' in result['walk'],enabled)
            self.assertEqual(port.closed,1)
            if not enabled:self.assertEqual(result['working_set_metadata']['status'],'NOT_REQUESTED')

    def test_known_valid_zero_is_not_unknown_denied_or_unattempted(self):
        port=self.page_port();port.working_set_page_flags=lambda addresses:[0]*len(addresses)
        meta=self.observe(port)['working_set_metadata'];total=meta['observed']['totals']
        self.assertTrue(meta['metadata_complete'])
        self.assertEqual((total['valid_page_bytes'],total['invalid_pages'],total['unknown_attempted_pages']),(0,4,0))
        self.assertEqual(meta['raw_flags_histogram'][0]['flags_hex'],'0x0000000000000000')
        port=self.page_port()
        def denied(_):raise ObservationError('K32QueryWorkingSetEx','denied',5)
        port.working_set_page_flags=denied
        meta=self.observe(port)['working_set_metadata'];total=meta['observed']['totals']
        self.assertFalse(meta['metadata_complete']);self.assertEqual(meta['status'],'UNAVAILABLE')
        self.assertEqual((total['observed_pages'],total['invalid_pages'],total['unknown_attempted_pages']),(0,0,4))
        self.assertEqual(meta['errors'][0]['win32_error'],5)

    def test_page_limit_and_partial_walk_do_not_promote_selected_prefix(self):
        port=self.page_port();meta=self.observe(port,max_pages=3)['working_set_metadata']
        self.assertEqual(meta['status'],'PARTIAL_PAGE_METADATA')
        self.assertEqual(meta['unselected_observed_pages'],1)
        self.assertFalse(meta['selected_address_coverage_complete'])
        self.assertTrue(meta['page_query_coverage_complete'])
        port=self.page_port();result=self.observe(port,max_regions=2)
        meta=result['working_set_metadata']
        self.assertEqual(len(result['walk']['committed_spans']['rows']),1)
        self.assertEqual(result['walk']['committed_spans']['span_limit'],2)
        self.assertFalse(meta['walk_address_space_complete'])
        self.assertIsNone(meta['unobserved_address_space_pages'])
        self.assertEqual(meta['status'],'PARTIAL_PAGE_METADATA')
        self.assertIsNone(result['walk']['full_totals'])

    def large_walk(self,pages):
        return walk_regions(lambda _:region(0,size=pages*PAGE),pages*PAGE,
            clock=lambda:0,retain_committed_spans=True)

    def test_bounded_batches_and_exit_between_calls_retain_real_prefix(self):
        count=native.WORKING_SET_PAGE_LIMIT+3
        walked=self.large_walk(count);calls=[]
        def query(addresses):calls.append(list(addresses));return [1]*len(addresses)
        meta=working_set_page_metadata(walked,query,clock=lambda:0)
        self.assertTrue(meta['metadata_complete'])
        self.assertEqual([len(row) for row in calls],[native.WORKING_SET_PAGE_LIMIT,3])
        self.assertEqual(calls[0][-1]+PAGE,calls[1][0])
        alive=[True];calls.clear()
        def exits(addresses):calls.append(list(addresses));alive[0]=False;return [1]*len(addresses)
        meta=working_set_page_metadata(walked,exits,clock=lambda:0,alive=lambda:alive[0])
        self.assertEqual(meta['status'],'PARTIAL_PAGE_METADATA')
        self.assertEqual(meta['observed']['totals']['observed_pages'],native.WORKING_SET_PAGE_LIMIT)
        self.assertEqual(meta['observed']['totals']['unattempted_selected_pages'],3)
        self.assertEqual(len(calls),1)
        self.assertEqual(meta['errors'][0]['stage'],'process_exit')

    def test_inter_call_time_budget_and_identity_change_preserve_observations(self):
        walked=self.large_walk(native.WORKING_SET_PAGE_LIMIT+1);now=[0]
        def slow(addresses):now[0]=1;return [1]*len(addresses)
        meta=working_set_page_metadata(walked,slow,clock=lambda:now[0],max_seconds=0.5)
        self.assertTrue(meta['time_budget_exceeded']);self.assertFalse(meta['metadata_complete'])
        self.assertEqual(meta['query_calls'],1)
        self.assertEqual(meta['observed']['totals']['unattempted_selected_pages'],1)
        self.assertEqual(meta['errors'][0]['stage'],'page_budget')
        ticks=iter((0,1,1));calls=[]
        meta=working_set_page_metadata(self.large_walk(1),lambda addresses:calls.append(addresses),
            clock=lambda:next(ticks),max_seconds=0.5)
        self.assertEqual(calls,[])
        self.assertEqual(meta['observed']['totals']['unattempted_selected_pages'],1)
        self.assertEqual(meta['query_calls'],0)
        port=self.page_port();port.mismatch=True
        meta=self.observe(port)['working_set_metadata']
        self.assertEqual(meta['status'],'UNAVAILABLE');self.assertFalse(meta['metadata_complete'])
        self.assertEqual(meta['observed']['totals']['observed_pages'],4)
        self.assertFalse(meta['same_target_identity_verified'])

    def test_malformed_flags_are_unknown_whole_batch_not_measured_zero(self):
        for response in (None,[],[1]*3,[1,1,1,True],[1,1,1,-1],[1,1,1,1<<64]):
            port=self.page_port();port.working_set_page_flags=lambda _,value=response:value
            meta=self.observe(port)['working_set_metadata'];total=meta['observed']['totals']
            self.assertFalse(meta['metadata_complete'])
            self.assertEqual((total['observed_pages'],total['invalid_pages'],total['unknown_attempted_pages']),(0,0,4))
        walked=self.large_walk(1);walked['committed_spans']['rows'][0]['end_exclusive']='0x1'
        calls=[];meta=working_set_page_metadata(walked,lambda addresses:calls.append(addresses),clock=lambda:0)
        self.assertEqual(calls,[]);self.assertFalse(meta['metadata_complete'])

    def test_page_clock_failure_does_not_hide_a_simultaneous_native_error(self):
        walked=self.large_walk(1);now=[1]
        def denied(addresses):now[0]=0;raise ObservationError('K32QueryWorkingSetEx','denied',5)
        meta=working_set_page_metadata(walked,denied,clock=lambda:now[0])
        self.assertFalse(meta['metadata_complete'])
        self.assertTrue(any(error['win32_error']==5 for error in meta['errors']))
        self.assertTrue(any(error['stage']=='page_clock' for error in meta['errors']))
        self.assertEqual(meta['observed']['totals']['unknown_attempted_pages'],1)
        calls=[]
        meta=working_set_page_metadata(walked,lambda addresses:calls.append(addresses),clock=lambda:float('nan'))
        self.assertEqual(calls,[]);self.assertFalse(meta['metadata_complete'])

    def test_no_committed_spans_and_histogram_bound_are_explicit(self):
        walked=walk_regions(lambda _:region(0,MEM_FREE),PAGE,clock=lambda:0,retain_committed_spans=True)
        calls=[];meta=working_set_page_metadata(walked,lambda addresses:calls.append(addresses),clock=lambda:0)
        self.assertEqual(meta['status'],'NOT_EXERCISED');self.assertEqual(calls,[])
        walked=self.large_walk(3)
        with patch.object(probe,'PAGE_FLAGS_HISTOGRAM_LIMIT',1):
            meta=working_set_page_metadata(walked,lambda addresses:[1,0,3],clock=lambda:0)
        self.assertEqual(len(meta['raw_flags_histogram']),1)
        self.assertEqual(meta['histogram_unretained_pages'],2)
        self.assertTrue(meta['page_query_coverage_complete']);self.assertFalse(meta['raw_flags_coverage_complete'])
        self.assertEqual(meta['status'],'PARTIAL_PAGE_METADATA')

    def test_input_options_and_cli_reach_actual_capture_without_child(self):
        for options in (dict(working_set_pages=1),dict(max_pages=0),dict(max_pages=True),
                        dict(max_pages=1048577),dict(max_page_seconds=False),dict(max_page_seconds=float('nan'))):
            with self.subTest(options=options),self.assertRaises(ValueError):
                capture(42,'C:/fixture/app.exe',reader_factory=lambda _:self.fail('must not open'),**options)
        original=probe.capture
        with tempfile.TemporaryDirectory(prefix='jce-page-cli-') as folder:
            for limit,code in ((4,0),(1,2)):
                port=self.page_port();seen=[]
                def factory(pid,**kwargs):seen.append(kwargs);return port
                def actual_capture(*args,**kwargs):return original(*args,reader_factory=factory,**kwargs)
                output=Path(folder)/str(limit)
                argv=['memory_commit_probe.py','--pid','42','--exe','C:/fixture/app.exe',
                    '--out',str(output),'--working-set-pages','--page-limit',str(limit),'--page-seconds','0.5']
                with patch.object(sys,'argv',argv),patch.object(probe,'capture',actual_capture),redirect_stdout(io.StringIO()):
                    self.assertEqual(probe.main(),code)
                report=json.loads((output/'report.json').read_text(encoding='utf-8'))
                self.assertEqual(report['working_set_metadata']['max_pages'],limit)
                self.assertEqual(seen,[{'working_set_pages':True}]);self.assertEqual(port.closed,1)

    def test_whole_owner_missing_wire_and_invalid_zero_controls_fail_ordinary_assertions(self):
        source=Path(probe.__file__).read_text(encoding='utf-8')
        wire=('report["working_set_metadata"] = working_set_page_metadata(report["walk"],query,\n'
              '                        max_pages=max_pages,max_seconds=max_page_seconds,clock=clock,alive=reader.alive)')
        variants=((wire,'report["working_set_metadata"] = dict(requested=True,status="UNAVAILABLE",metadata_complete=False)'),
                  ('valid = bool(word & 1)','valid = True'))
        for needle,replacement in variants:
            self.assertEqual(source.count(needle),1)
            namespace=dict(probe.__dict__,__name__='jce_page_metadata_control')
            exec(compile(source.replace(needle,replacement,1),probe.__file__,'exec'),namespace)
            with self.subTest(control=needle),self.assertRaises(AssertionError):
                self.assert_complete_consumer(namespace['capture'])


class WorkingSetPagePortTests(unittest.TestCase):
    def reader(self):
        reader=WindowsReader.__new__(WindowsReader)
        reader.handle=123;reader.working_set_pages=True;reader._working_set_api_unavailable=None
        reader.bounds={'page_bytes':PAGE,'minimum_address':PAGE,'maximum_address':(1<<48)-1}
        return reader

    def test_actual_page_method_preserves_raw_flags_and_one_bounded_call(self):
        reader=self.reader();calls=[]
        words=[1,1 | (1<<15) | (7<<1),0,(1<<63) | 1]
        def query(handle,buffer,size):
            calls.append((handle,size,[int(x.VirtualAddress) for x in buffer]))
            for entry,flags in zip(buffer,words):entry.Flags=flags
            return True
        reader._working_set_api=query
        self.assertEqual(reader.working_set_page_flags([PAGE*n for n in range(1,5)]),words)
        self.assertEqual(calls,[(123,64,[PAGE*n for n in range(1,5)])])
        # An invalid-page word stays raw; optional Shared bits are not an RSS sum.
        self.assertEqual(reader.handle,123)

    def test_invalid_page_inputs_and_limits_never_enter_native_call(self):
        reader=self.reader();calls=[]
        reader._working_set_api=lambda *args:calls.append(args) or True
        for addresses in ([],[0],[True],[PAGE+1],[PAGE,PAGE],[2*PAGE,PAGE],
                          [1<<64],[PAGE]*(native.WORKING_SET_PAGE_LIMIT+1),iter([PAGE])):
            with self.subTest(addresses=type(addresses)),self.assertRaises(ValueError):
                reader.working_set_page_flags(addresses)
        self.assertEqual(calls,[])
        reader.bounds['page_bytes']=3
        with self.assertRaisesRegex(ObservationError,'bounds'):reader.working_set_page_flags([PAGE])
        self.assertEqual(calls,[])

    def test_page_off_missing_error_closed_and_mutated_response_stay_unavailable(self):
        reader=self.reader();calls=[]
        reader._working_set_api=lambda *args:calls.append(args) or False
        reader.working_set_pages=False
        with self.assertRaisesRegex(ObservationError,'not requested'):reader.working_set_page_flags([PAGE])
        reader.working_set_pages=True
        with patch.object(native.ctypes,'get_last_error',lambda:5,create=True):
            with self.assertRaises(ObservationError) as caught:reader.working_set_page_flags([PAGE])
        self.assertEqual(caught.exception.code,5);self.assertEqual(caught.exception.stage,'K32QueryWorkingSetEx')
        self.assertEqual(len(calls),1)
        reader._working_set_api=None
        with self.assertRaisesRegex(ObservationError,'optional API unavailable') as caught:reader.working_set_page_flags([PAGE])
        self.assertIsNone(caught.exception.code)
        def mutate(handle,buffer,size):buffer[0].VirtualAddress=2*PAGE;return True
        reader._working_set_api=mutate
        with self.assertRaisesRegex(ObservationError,'altered'):reader.working_set_page_flags([PAGE])
        reader.handle=None
        with self.assertRaisesRegex(ObservationError,'closed'):reader.working_set_page_flags([PAGE])

    @unittest.skipUnless(sys.platform=='win32','native Win64 typed API layout required')
    def test_page_constructor_opt_in_keeps_query_rights_and_default_off(self):
        class Function:
            def __init__(self,call):self.call=call
            def __call__(self,*args):return self.call(*args)
        class Api:
            def __init__(self):
                self.lookups=[];self.opened=[];self.closed=[]
                self.OpenProcess=Function(lambda mask,inherit,pid:self.opened.append((mask,inherit,pid)) or 123)
                self.CloseHandle=Function(lambda handle:self.closed.append(handle) or True)
                self.IsWow64Process2=Function(self.machine)
                self.GetSystemInfo=Function(self.system)
                self.QueryPerformanceFrequency=Function(lambda frequency:setattr(frequency._obj,'value',1000) or True)
            def __getattr__(self,name):self.lookups.append(name);return Function(lambda *args:True)
            def machine(self,handle,process,native_machine):
                process._obj.value=0;native_machine._obj.value=0x8664;return True
            def system(self,info):
                info._obj.MinimumAddress=PAGE;info._obj.MaximumAddress=3*PAGE-1;info._obj.PageSize=PAGE
        for enabled in (False,True):
            api=Api()
            with patch.object(native.ctypes,'WinDLL',lambda *args,**kwargs:api):
                with WindowsReader(42,working_set_pages=enabled) as reader:
                    self.assertEqual(reader.working_set_pages,enabled)
            self.assertEqual(api.opened,[(0x0400|0x00100000,False,42)])
            self.assertEqual(api.closed,[123])
            self.assertEqual('K32QueryWorkingSetEx' in api.lookups,enabled)
        for enabled in (0,1,None,'yes'):
            with self.assertRaises(ValueError):WindowsReader(42,working_set_pages=enabled)


if __name__=='__main__': unittest.main()
