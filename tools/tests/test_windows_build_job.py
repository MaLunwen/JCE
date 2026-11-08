"""Bounded normal-token Windows CPU controls for the opt-in build Job owner."""
from pathlib import Path
import ctypes, hashlib, importlib.util, json, os, subprocess, sys, tempfile, time, unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[2]
MODULE=ROOT/'tools/build/windows_build_job.py'


def system_binary(name):
    k=ctypes.WinDLL('kernel32',use_last_error=True)
    k.GetSystemDirectoryW.argtypes=[ctypes.c_wchar_p,ctypes.c_uint32];k.GetSystemDirectoryW.restype=ctypes.c_uint32
    buffer=ctypes.create_unicode_buffer(32768);length=k.GetSystemDirectoryW(buffer,len(buffer))
    assert 0<length<len(buffer)
    return Path(buffer.value)/name


def fixture():
    import ctypes.wintypes as w
    k=ctypes.WinDLL('kernel32',use_last_error=True)
    k.GetCurrentProcess.restype=w.HANDLE
    k.IsProcessInJob.argtypes=[w.HANDLE,w.HANDLE,ctypes.POINTER(w.BOOL)]
    member=w.BOOL();assert k.IsProcessInJob(k.GetCurrentProcess(),None,ctypes.byref(member))
    mode=sys.argv[2]
    print(json.dumps({'membership':bool(member.value),'argv':sys.argv[3:]}),flush=True)
    if mode=='spawn':
        child=subprocess.Popen([sys.executable,'-B',__file__,'--fixture','sleep'],creationflags=subprocess.CREATE_NO_WINDOW)
        print(json.dumps({'helper_pid':child.pid}),flush=True)
    elif mode=='watch':
        signal=Path(sys.argv[3]);ready=signal.with_suffix('.ready');spawned=signal.with_suffix('.spawned');ready.write_text('ready')
        until=time.monotonic()+10
        while not signal.exists() and time.monotonic()<until:time.sleep(.005)
        if signal.exists():
            child=subprocess.Popen([sys.executable,'-B',__file__,'--fixture','sleep'],creationflags=subprocess.CREATE_NO_WINDOW)
            spawned.write_text(str(child.pid))
        time.sleep(20)
    elif mode=='spawn_watch':
        signal=Path(sys.argv[3]);child=subprocess.Popen([sys.executable,'-B',__file__,'--fixture','watch',str(signal)],creationflags=subprocess.CREATE_NO_WINDOW)
        until=time.monotonic()+3
        while not signal.with_suffix('.ready').exists() and time.monotonic()<until:time.sleep(.005)
        assert signal.with_suffix('.ready').exists()
    elif mode=='sleep':time.sleep(20)
    elif mode=='late':
        child=subprocess.Popen([sys.executable,'-B',__file__,'--fixture','spawn'],creationflags=subprocess.CREATE_NO_WINDOW)
        print(json.dumps({'helper_pid':child.pid}),flush=True)
    elif mode=='nonzero':raise SystemExit(7)


@unittest.skipUnless(os.name=='nt','Windows kernel Job controls')
class WindowsBuildJobTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert not ctypes.WinDLL('shell32').IsUserAnAdmin(), 'CPU fixtures require ordinary token'
    def setUp(self):
        retained=os.environ.get('JCE_TEST_WINDOWS_BUILD_JOB_RECEIPTS')
        if retained:
            base=Path(retained).resolve();base.mkdir(parents=True,exist_ok=True)
            self.temp=None;self.directory=Path(tempfile.mkdtemp(prefix=self._testMethodName+'-',dir=base))
        else:
            base=ROOT/'build'/'tests'/'windows-build-job';base.mkdir(parents=True,exist_ok=True)
            self.temp=tempfile.TemporaryDirectory(prefix='jce-build-job-',dir=base)
            self.directory=Path(self.temp.name)
    def tearDown(self):
        if self.temp is not None:self.temp.cleanup()
    def owner(self):
        self.assertTrue(MODULE.is_file(),'explicit opt-in build Job owner is missing')
        spec=importlib.util.spec_from_file_location('tested_windows_build_job',MODULE)
        module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
        return module
    def run_case(self,mode='echo',helpers=(),**options):
        m=self.owner()
        command=[sys.executable,'-B',__file__,'--fixture',mode,'space and quote " value','中文路径']
        result=m.run_build_job(command,cwd=ROOT,output_dir=self.directory/'run',timeout_s=3,cleanup_timeout_s=3,allowed_helpers=helpers,**options)
        self.assertEqual(json.loads((self.directory/'run/receipt.json').read_bytes()),result)
        return m,result
    def helper(self,path=None,digest=None):
        p=Path(path or sys.executable).resolve()
        return {'path':str(p),'sha256':digest or hashlib.sha256(p.read_bytes()).hexdigest()}
    def helpers(self):
        return [self.helper(),self.helper(system_binary('conhost.exe'))]
    def test_command_executes_inside_job_unicode_argv_and_natural_return(self):
        _,r=self.run_case();self.assertEqual(r['status'],'PASS_NATURAL',r)
        row=json.loads((self.directory/'run/stdout.log').read_text().splitlines()[0])
        self.assertTrue(row['membership']);self.assertEqual(row['argv'],['space and quote " value','中文路径'])
        self.assertTrue(r['containment']['empty_verified']);self.assertFalse(r['planned_actions']);self.assertFalse(r['emergency_actions'])
    def test_default_rejects_surviving_helper_and_reaps_owned_job(self):
        _,r=self.run_case('spawn');self.assertEqual(r['status'],'FAIL')
        self.assertTrue(r['containment']['empty_verified']);self.assertFalse(r['planned_actions']);self.assertTrue(r['emergency_actions'])
    def test_explicit_image_hash_helper_planned_retirement(self):
        _,r=self.run_case('spawn',self.helpers());self.assertEqual(r['status'],'PASS_PLANNED_HELPER_CLEANUP',r)
        self.assertTrue(r['planned_actions']);self.assertFalse(r['emergency_actions']);self.assertTrue(r['containment']['empty_verified'])
        self.assertIs(type(r['planned_actions'][0]['creation_filetime']),int)
        self.assertGreater(r['planned_actions'][0]['creation_filetime'],2**53)
        self.assertTrue(r['planned_actions'][0]['in_owned_job'])
    def test_wrong_hash_refuses_before_execution(self):
        _,r=self.run_case('spawn',[self.helper(digest='0'*64)]);self.assertEqual(r['status'],'FAIL');self.assertIsNone(r['command']['pid'])
    def test_wrong_path_never_classifies_same_name_or_other_image(self):
        other=system_binary('cmd.exe')
        _,r=self.run_case('spawn',[self.helper(other)]);self.assertEqual(r['status'],'FAIL');self.assertFalse(r['planned_actions']);self.assertTrue(r['containment']['empty_verified'])
    def test_timeout_has_only_emergency_cleanup(self):
        _,r=self.run_case('sleep');self.assertEqual(r['status'],'FAIL');self.assertIn('timeout',r['failure']);self.assertFalse(r['planned_actions']);self.assertTrue(r['containment']['empty_verified'])
    def test_nonzero_never_uses_planned_policy(self):
        _,r=self.run_case('nonzero',[self.helper()]);self.assertEqual(r['status'],'FAIL');self.assertEqual(r['command']['returncode'],7)
    def test_external_sibling_never_adopted_or_killed(self):
        sibling=subprocess.Popen([sys.executable,'-B',__file__,'--fixture','sleep'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            _,r=self.run_case('spawn',self.helpers());self.assertEqual(r['status'],'PASS_PLANNED_HELPER_CLEANUP',r)
            self.assertIsNone(sibling.poll());self.assertNotIn(sibling.pid,[x['pid'] for x in r['processes']])
        finally:sibling.terminate();sibling.wait(timeout=3)
    def test_assignment_failure_does_not_execute_suspended_command(self):
        m=self.owner()
        with patch.object(m._Native,'assign',side_effect=OSError('injected assignment failure')):
            r=m.run_build_job([sys.executable,'-B',__file__,'--fixture','echo'],cwd=ROOT,output_dir=self.directory/'run',timeout_s=3,cleanup_timeout_s=3)
        self.assertEqual(r['status'],'FAIL');self.assertFalse((self.directory/'run/stdout.log').read_bytes());self.assertTrue(r['command']['reaped'])
    def test_resume_failure_reaps_assigned_suspended_child(self):
        m=self.owner()
        with patch.object(m._Native,'resume',side_effect=OSError('injected resume failure')):
            command=[sys.executable,'-B',__file__,'--fixture','echo']
            r=m.run_build_job(command,cwd=ROOT,output_dir=self.directory/'run',timeout_s=3,cleanup_timeout_s=3)
        self.assertEqual(r['status'],'FAIL');self.assertTrue(r['command']['reaped']);self.assertTrue(r['containment']['empty_verified'])
    def test_close_failure_denies_pass_after_real_handle_close(self):
        m=self.owner();real=m._Native.close
        def close(native,handle):
            real(native,handle)
            if not getattr(native,'_injected_close',False):
                native._injected_close=True;raise OSError('injected close outcome unverified')
        with patch.object(m._Native,'close',close):
            r=m.run_build_job([sys.executable,'-B',__file__,'--fixture','echo'],cwd=ROOT,output_dir=self.directory/'run',timeout_s=3,cleanup_timeout_s=3)
        self.assertEqual(r['status'],'FAIL');self.assertTrue(r['cleanup_errors']);self.assertTrue(r['containment']['empty_verified'])
    def test_query_failure_denies_pass_and_terminates_job(self):
        m=self.owner()
        with patch.object(m._Native,'snapshot',side_effect=OSError('injected snapshot failure')):
            r=m.run_build_job([sys.executable,'-B',__file__,'--fixture','sleep'],cwd=ROOT,output_dir=self.directory/'run',timeout_s=3,cleanup_timeout_s=1)
        self.assertEqual(r['status'],'FAIL');self.assertTrue(r['emergency_actions']);self.assertFalse(r['containment']['empty_verified']);self.assertTrue(r['command']['reaped'])
    def test_attribute_setup_failure_never_executes_and_closes_handles(self):
        m=self.owner()
        with patch.object(m._Native,'restrict_attributes',side_effect=OSError('injected attribute update failure')):
            r=m.run_build_job([sys.executable],cwd=ROOT,output_dir=self.directory/'run',timeout_s=2,cleanup_timeout_s=1)
        self.assertEqual(r['status'],'FAIL');self.assertIsNone(r['command']['pid'])
        self.assertIn('returned',r['attribute_cleanup']);self.assertTrue(all(x['returned'] for x in r['handle_closes']))
    def test_cancellation_reaps_contained_suspended_child(self):
        m=self.owner()
        with patch.object(m._Native,'resume',side_effect=KeyboardInterrupt()):
            r=m.run_build_job([sys.executable],cwd=ROOT,output_dir=self.directory/'run',timeout_s=2,cleanup_timeout_s=1)
        self.assertEqual(r['status'],'FAIL');self.assertIn('cancelled',r['failure']);self.assertTrue(r['command']['reaped']);self.assertTrue(r['containment']['empty_verified'])
    def test_changed_native_birth_cannot_authorize_planned_action(self):
        m=self.owner();real=m._Native.identity;counts={}
        def identity(native,handle):
            row=real(native,handle);counts[handle]=counts.get(handle,0)+1
            if counts[handle]>1:row['creation_filetime']+=1
            return row
        with patch.object(m._Native,'identity',identity):
            r=m.run_build_job([sys.executable,'-B',__file__,'--fixture','spawn'],cwd=ROOT,output_dir=self.directory/'run',timeout_s=3,cleanup_timeout_s=2,allowed_helpers=self.helpers())
        self.assertEqual(r['status'],'FAIL');self.assertFalse(r['planned_actions']);self.assertTrue(r['containment']['empty_verified'])
    def test_real_late_child_during_cleanup_never_becomes_success(self):
        m=self.owner();real=m._Native.terminate_process;signal=self.directory/'spawn.signal'
        def terminate(native,handle):
            if not getattr(native,'_late_signal',False):
                native._late_signal=True;signal.write_text('spawn now')
                until=time.monotonic()+2
                while not signal.with_suffix('.spawned').exists() and time.monotonic()<until:time.sleep(.005)
                self.assertTrue(signal.with_suffix('.spawned').exists(),'real contained helper failed to spawn late child')
            return real(native,handle)
        with patch.object(m._Native,'terminate_process',terminate):
            r=m.run_build_job([sys.executable,'-B',__file__,'--fixture','spawn_watch',str(signal)],cwd=ROOT,output_dir=self.directory/'run',timeout_s=5,cleanup_timeout_s=3,allowed_helpers=self.helpers())
        self.assertEqual(r['status'],'FAIL');self.assertIn('membership changed',r['failure']);self.assertTrue(r['containment']['empty_verified']);self.assertTrue(r['emergency_actions'])
    def test_output_budget_refuses_even_fast_command_zero(self):
        _,r=self.run_case(max_output_bytes=1);self.assertEqual(r['status'],'FAIL');self.assertIn('output byte budget',r['failure']);self.assertTrue(r['containment']['empty_verified'])
    def test_unsupported_backend_fails_closed_before_spawn(self):
        m=self.owner()
        with patch.object(m,'_Native',side_effect=OSError('unsupported Job API')):
            r=m.run_build_job([sys.executable],cwd=ROOT,output_dir=self.directory/'run',timeout_s=2,cleanup_timeout_s=1)
        self.assertEqual(r['status'],'FAIL');self.assertIsNone(r['command']['pid'])

    def test_known_shared_broker_policy_rejected_without_executing_msvc(self):
        m=self.owner();real=m._Native.image
        for broker in ('mspdbsrv.exe','vctip.exe'):
            def image(native,handle,broker=broker):
                row=real(native,handle);row['path']=str(Path(row['path']).with_name(broker));return row
            with patch.object(m._Native,'image',image):
                r=m.run_build_job([sys.executable],cwd=ROOT,output_dir=self.directory/broker,timeout_s=2,cleanup_timeout_s=1,allowed_helpers=[self.helper()])
            self.assertEqual(r['status'],'FAIL');self.assertIsNone(r['command']['pid']);self.assertIn('shared IPC broker',r['failure'])

    def test_fresh_output_and_explicit_helper_policy(self):
        m,r=self.run_case();before=(self.directory/'run/receipt.json').read_bytes()
        with self.assertRaises(FileExistsError):m.run_build_job([sys.executable],cwd=ROOT,output_dir=self.directory/'run')
        self.assertEqual(before,(self.directory/'run/receipt.json').read_bytes())

class WindowsBuildJobWorkspaceTests(unittest.TestCase):
    """Exercise the real fixture setup without launching Windows processes."""

    def setUp(self):
        base=ROOT/'build'/'tests'/'windows-build-job';base.mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(prefix='workspace-control-',dir=base)
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)/'repository'
        self.case=WindowsBuildJobTests('test_fresh_output_and_explicit_helper_policy')

    def test_default_creates_build_parent_and_cleans_only_its_fresh_directory(self):
        with patch.dict(os.environ,{},clear=True), patch.dict(globals(),ROOT=self.root):
            self.assertFalse(self.root.exists())
            self.case.setUp()
        self.addCleanup(self.case.tearDown)
        directory=self.case.directory
        base=self.root/'build'/'tests'/'windows-build-job'
        self.assertEqual(directory.parent,base)
        self.assertTrue(directory.name.startswith('jce-build-job-'))
        self.assertTrue(directory.is_dir())
        marker=base/'unrelated.txt';marker.write_text('keep',encoding='ascii')
        self.case.tearDown()
        self.assertFalse(directory.exists())
        self.assertEqual(marker.read_text(encoding='ascii'),'keep')
        self.assertFalse((self.root/'reports').exists())

    def test_explicit_receipt_root_is_honored_and_retained(self):
        retained=Path(self.temp.name)/'chosen receipts'/'nested'
        with patch.dict(os.environ,{'JCE_TEST_WINDOWS_BUILD_JOB_RECEIPTS':str(retained)}), \
             patch.dict(globals(),ROOT=self.root):
            self.assertFalse(retained.exists())
            self.case.setUp()
        self.addCleanup(self.case.tearDown)
        directory=self.case.directory
        self.assertEqual(directory.parent,retained.resolve())
        self.assertTrue(directory.name.startswith(self.case._testMethodName+'-'))
        self.assertIsNone(self.case.temp)
        marker=directory/'receipt.json';marker.write_text('{}',encoding='ascii')
        self.case.tearDown()
        self.assertEqual(marker.read_text(encoding='ascii'),'{}')
        self.assertFalse(self.root.exists())


if __name__=='__main__':
    if len(sys.argv)>1 and sys.argv[1]=='--fixture':fixture()
    else:unittest.main()
