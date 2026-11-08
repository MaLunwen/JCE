"""Explicit Windows BUILD-only Job owner; never installed into a default runner.

Trusted callers pass exact helper images, never project/inherited policy. A helper
termination is planned forced cleanup, not natural exit. Kernel Job containment
is separate from sampled per-process metadata; no complete ancestry claim.
Caller must exclusively own the complete process tree AND any service clients.
Shared IPC brokers are unsupported: image pins/Job membership do not prove IPC
exclusivity. Do not use this owner for real MSVC builds without separately
proved instance isolation; this version rejects MSPDBSRV/VCTIP helper policies.
"""
from __future__ import annotations
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import time

DWORD = c.c_uint32
HANDLE = c.c_void_p
SIZE_T = c.c_size_t


class _Filetime(c.Structure):
    _fields_ = [('low', DWORD), ('high', DWORD)]


class _Security(c.Structure):
    _fields_ = [('length', DWORD), ('descriptor', c.c_void_p), ('inherit', w.BOOL)]


class _Startup(c.Structure):
    _fields_ = [('cb', DWORD), ('reserved', w.LPWSTR), ('desktop', w.LPWSTR), ('title', w.LPWSTR),
                ('x', DWORD), ('y', DWORD), ('cx', DWORD), ('cy', DWORD), ('chars_x', DWORD),
                ('chars_y', DWORD), ('fill', DWORD), ('flags', DWORD), ('show', w.WORD),
                ('reserved_size', w.WORD), ('reserved_bytes', c.c_void_p),
                ('stdin', HANDLE), ('stdout', HANDLE), ('stderr', HANDLE)]


class _StartupEx(c.Structure):
    _fields_ = [('startup', _Startup), ('attributes', c.c_void_p)]


class _ProcessInfo(c.Structure):
    _fields_ = [('process', HANDLE), ('thread', HANDLE), ('pid', DWORD), ('tid', DWORD)]


class _BasicLimits(c.Structure):
    _fields_ = [('process_time', c.c_int64), ('job_time', c.c_int64), ('flags', DWORD),
                ('min_working', SIZE_T), ('max_working', SIZE_T), ('active_limit', DWORD),
                ('affinity', SIZE_T), ('priority', DWORD), ('scheduling', DWORD)]


class _IoCounters(c.Structure):
    _fields_ = [(name, c.c_uint64) for name in ('read_ops', 'write_ops', 'other_ops', 'read_bytes', 'write_bytes', 'other_bytes')]


class _ExtendedLimits(c.Structure):
    _fields_ = [('basic', _BasicLimits), ('io', _IoCounters), ('process_memory', SIZE_T),
                ('job_memory', SIZE_T), ('peak_process_memory', SIZE_T), ('peak_job_memory', SIZE_T)]


class _Accounting(c.Structure):
    _fields_ = [('user', c.c_int64), ('kernel', c.c_int64), ('period_user', c.c_int64),
                ('period_kernel', c.c_int64), ('page_faults', DWORD), ('total', DWORD),
                ('active', DWORD), ('terminated', DWORD)]


def _normal(path):
    value = str(path)
    if value.startswith('\\\\?\\UNC\\'):
        value = '\\\\' + value[8:]
    elif value.startswith('\\\\?\\'):
        value = value[4:]
    return os.path.normcase(os.path.normpath(value))


class _Native:
    """Small documented Win32 call port. Handles stay owned by run_build_job."""
    def __init__(self):
        if os.name != 'nt':
            raise OSError('Windows Job API unavailable; no fallback')
        self.k = c.WinDLL('kernel32', use_last_error=True)
        declarations = {
            'CreateJobObjectW': ([c.c_void_p, w.LPCWSTR], HANDLE),
            'SetInformationJobObject': ([HANDLE, c.c_int, c.c_void_p, DWORD], w.BOOL),
            'QueryInformationJobObject': ([HANDLE, c.c_int, c.c_void_p, DWORD, c.c_void_p], w.BOOL),
            'AssignProcessToJobObject': ([HANDLE, HANDLE], w.BOOL),
            'IsProcessInJob': ([HANDLE, HANDLE, c.POINTER(w.BOOL)], w.BOOL),
            'TerminateJobObject': ([HANDLE, w.UINT], w.BOOL),
            'CreateFileW': ([w.LPCWSTR, DWORD, DWORD, c.c_void_p, DWORD, DWORD, HANDLE], HANDLE),
            'GetFileSizeEx': ([HANDLE, c.POINTER(c.c_int64)], w.BOOL),
            'ReadFile': ([HANDLE, c.c_void_p, DWORD, c.POINTER(DWORD), c.c_void_p], w.BOOL),
            'GetFinalPathNameByHandleW': ([HANDLE, w.LPWSTR, DWORD, DWORD], DWORD),
            'InitializeProcThreadAttributeList': ([c.c_void_p, DWORD, DWORD, c.POINTER(SIZE_T)], w.BOOL),
            'UpdateProcThreadAttribute': ([c.c_void_p, DWORD, SIZE_T, c.c_void_p, SIZE_T, c.c_void_p, c.c_void_p], w.BOOL),
            'DeleteProcThreadAttributeList': ([c.c_void_p], None),
            'CreateProcessW': ([w.LPCWSTR, w.LPWSTR, c.c_void_p, c.c_void_p, w.BOOL, DWORD,
                                c.c_void_p, w.LPCWSTR, c.POINTER(_StartupEx), c.POINTER(_ProcessInfo)], w.BOOL),
            'ResumeThread': ([HANDLE], DWORD),
            'OpenProcess': ([DWORD, w.BOOL, DWORD], HANDLE),
            'GetProcessId': ([HANDLE], DWORD),
            'GetProcessTimes': ([HANDLE, c.POINTER(_Filetime), c.POINTER(_Filetime), c.POINTER(_Filetime), c.POINTER(_Filetime)], w.BOOL),
            'QueryFullProcessImageNameW': ([HANDLE, DWORD, w.LPWSTR, c.POINTER(DWORD)], w.BOOL),
            'GetExitCodeProcess': ([HANDLE, c.POINTER(DWORD)], w.BOOL),
            'WaitForSingleObject': ([HANDLE, DWORD], DWORD),
            'TerminateProcess': ([HANDLE, w.UINT], w.BOOL),
            'CloseHandle': ([HANDLE], w.BOOL),
        }
        for name, (args, result) in declarations.items():
            function = getattr(self.k, name)
            function.argtypes, function.restype = args, result

    @staticmethod
    def check(value, operation):
        if not value:
            error = c.get_last_error()
            raise OSError(error, operation + ': ' + c.FormatError(error))
        return value

    def close(self, handle):
        self.check(self.k.CloseHandle(handle), 'CloseHandle')

    def job(self, cap):
        handle = self.check(self.k.CreateJobObjectW(None, None), 'CreateJobObjectW')
        return handle

    def configure_job(self, job, cap):
        limits = _ExtendedLimits()
        limits.basic.flags = 0x2000 | 0x8  # KILL_ON_JOB_CLOSE | ACTIVE_PROCESS
        limits.basic.active_limit = cap  # Neither BREAKAWAY flag is enabled.
        self.check(self.k.SetInformationJobObject(job, 9, c.byref(limits), c.sizeof(limits)), 'SetInformationJobObject')

    def file(self, path, *, output=False, stdin=False):
        security = _Security(c.sizeof(_Security), None, bool(output or stdin))
        handle = self.k.CreateFileW(str(path), 0x40000000 if output else 0x80000000,
                                   1, c.byref(security), 1 if output else 3, 0x80, None)
        if handle is None or handle == c.c_void_p(-1).value:
            self.check(False, 'CreateFileW')
        return handle

    def size(self, handle):
        value = c.c_int64()
        self.check(self.k.GetFileSizeEx(handle, c.byref(value)), 'GetFileSizeEx')
        return value.value

    def image(self, handle, byte_cap=268435456):
        path = c.create_unicode_buffer(32768)
        count = self.k.GetFinalPathNameByHandleW(handle, path, len(path), 0)
        self.check(count, 'GetFinalPathNameByHandleW')
        if count >= len(path):
            raise OSError('image path exceeds bound')
        size = self.size(handle)
        if not 0 < size <= byte_cap:
            raise OSError('helper image byte budget')
        digest = hashlib.sha256()
        buffer = c.create_string_buffer(1048576)
        total = 0
        while True:
            read = DWORD()
            self.check(self.k.ReadFile(handle, buffer, len(buffer), c.byref(read), None), 'ReadFile(helper image)')
            if not read.value:
                break
            total += read.value
            if total > byte_cap:
                raise OSError('helper image byte budget')
            digest.update(buffer.raw[:read.value])
        if total != size or self.size(handle) != size:
            raise OSError('helper image changed during held file read')
        return dict(path=_normal(path.value), bytes=size, sha256=digest.hexdigest())

    def attributes(self, stdio):
        size = SIZE_T()
        self.k.InitializeProcThreadAttributeList(None, 1, 0, c.byref(size))
        if not 0 < size.value <= 65536:
            raise OSError('attribute size unavailable or out of bounds')
        storage = c.create_string_buffer(size.value)
        self.check(self.k.InitializeProcThreadAttributeList(storage, 1, 0, c.byref(size)), 'InitializeProcThreadAttributeList')
        return storage

    def restrict_attributes(self, attributes, stdio):
        handles = (HANDLE * len(stdio))(*stdio)
        attributes.handle_values = handles  # Attribute value storage lives through process creation.
        self.check(self.k.UpdateProcThreadAttribute(attributes, 0, 0x20002, handles,
                   c.sizeof(handles), None, None), 'UpdateProcThreadAttribute(HANDLE_LIST)')

    def delete_attributes(self, attributes):
        self.k.DeleteProcThreadAttributeList(attributes)

    def create(self, argv, cwd, environment, stdio, attributes):
        info = _StartupEx()
        info.startup.cb = c.sizeof(info)
        info.startup.flags = 0x100  # STARTF_USESTDHANDLES
        info.startup.stdin, info.startup.stdout, info.startup.stderr = stdio
        info.attributes = c.cast(attributes, c.c_void_p)
        result = _ProcessInfo()
        command = c.create_unicode_buffer(subprocess.list2cmdline(argv))
        block = c.create_unicode_buffer('\0'.join(k+'='+v for k,v in sorted(environment.items(), key=lambda x:x[0].upper()))+'\0\0')
        flags = 0x4 | 0x8000000 | 0x80000 | 0x400  # SUSPENDED, NO_WINDOW, EXTENDED_STARTUPINFO, UNICODE_ENV
        self.check(self.k.CreateProcessW(argv[0], command, None, None, True, flags,
                   block, str(cwd), c.byref(info), c.byref(result)), 'CreateProcessW')
        return result

    def assign(self, job, process):
        self.check(self.k.AssignProcessToJobObject(job, process), 'AssignProcessToJobObject')

    def resume(self, thread):
        result = self.k.ResumeThread(thread)
        if result != 1:
            raise OSError('ResumeThread did not resume exactly the owned initial suspension: '+str(result))

    def open_process(self, pid):
        # Never terminate by a PID. This handle must separately prove membership.
        return self.check(self.k.OpenProcess(0x1000 | 0x100000 | 1, False, pid), 'OpenProcess')

    def identity(self, process):
        pid = self.check(self.k.GetProcessId(process), 'GetProcessId')
        created, exited, kernel, user = (_Filetime() for _ in range(4))
        self.check(self.k.GetProcessTimes(process, c.byref(created), c.byref(exited), c.byref(kernel), c.byref(user)), 'GetProcessTimes')
        name = c.create_unicode_buffer(32768)
        size = DWORD(len(name))
        self.check(self.k.QueryFullProcessImageNameW(process, 0, name, c.byref(size)), 'QueryFullProcessImageNameW')
        return dict(pid=pid, creation_filetime=(created.high << 32) | created.low, image_path=_normal(name.value))

    def membership(self, process, job):
        result = w.BOOL()
        self.check(self.k.IsProcessInJob(process, job, c.byref(result)), 'IsProcessInJob')
        return bool(result.value)

    def exit_code(self, process):
        state = self.k.WaitForSingleObject(process, 0)
        if state == 0x102:
            return None
        if state != 0:
            self.check(False, 'WaitForSingleObject')
        code = DWORD()
        self.check(self.k.GetExitCodeProcess(process, c.byref(code)), 'GetExitCodeProcess')
        return code.value

    def terminate_process(self, process):
        self.check(self.k.TerminateProcess(process, 0xE0000001), 'TerminateProcess')

    def terminate_job(self, job):
        self.check(self.k.TerminateJobObject(job, 0xE0000001), 'TerminateJobObject')

    def snapshot(self, job, cap):
        storage = c.create_string_buffer(8 + c.sizeof(SIZE_T)*cap)
        self.check(self.k.QueryInformationJobObject(job, 3, storage, len(storage), None), 'QueryInformationJobObject(ProcessIdList)')
        assigned, returned = (DWORD*2).from_buffer(storage)
        if assigned > cap or returned > cap:
            raise OSError('live Job process budget')
        pids = list((SIZE_T*returned).from_buffer(storage, 8))
        accounting = _Accounting()
        self.check(self.k.QueryInformationJobObject(job, 1, c.byref(accounting), c.sizeof(accounting), None), 'QueryInformationJobObject(Accounting)')
        return dict(pids=pids, total_processes=accounting.total, active_processes=accounting.active,
                    terminated_processes=accounting.terminated)


def _atomic_receipt(path, value):
    temporary = path.with_name(path.name+'.new')
    with temporary.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, indent=2, ensure_ascii=True, allow_nan=False)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def run_build_job(argv, *, cwd, output_dir, timeout_s=1800, cleanup_timeout_s=10,
                  poll_s=.05, allowed_helpers=(), max_live_processes=64,
                  max_output_bytes=134217728, max_total_processes=8192):
    """Opt-in BUILD invocation only. Return an atomic receipt; CLI fails on FAIL.

    Bounds are cooperative OS-call/poll budgets, not hard real-time guarantees.
    Helper policy is caller-supplied, never read from project/environment config.
    Caller must establish exclusive process/service ownership before invoking;
    this owner cannot establish IPC client isolation. Known shared MSVC brokers
    are refused with no opt-out or caller-boolean isolation override.
    A fresh directory is mandatory. No default application runner is changed.
    """
    out = Path(output_dir).resolve()
    out.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    result = dict(schema=1, purpose='EXPLICIT_BUILD_ONLY', status='FAIL', failure=None,
        command=dict(argv=list(argv), pid=None, creation_filetime=None, returncode=None, assigned=False, resumed=False, reaped=False),
        budgets=dict(timeout_s=timeout_s, cleanup_timeout_s=cleanup_timeout_s, poll_s=poll_s,
                     max_live_processes=max_live_processes, max_total_processes=max_total_processes, max_output_bytes=max_output_bytes, natural_return_grace_s=.25),
        planned_actions=[], emergency_actions=[], processes=[], helper_pins=[], cleanup_errors=[], handle_closes=[],
        containment=dict(kind='fresh unnamed nonbreakaway Win32 Job', kill_on_job_close=False,
                         empty_verified=False, final_snapshot=None, snapshot_error=None),
        metadata_scope='Sampled held-handle PID/native creation FILETIME/image path and Job membership; no complete ETW parent/short-lived-process coverage.',
        metadata_gaps=[], metadata_gap_count=0, elapsed_s=None,
        helper_policy_scope='Exact explicit caller list only, including any caller-declared OS console host; no automatic helper additions.',
        security_scope='Kernel resource containment, not a security sandbox. Root requests CREATE_NO_WINDOW; descendant console/window behavior is not certified.',
        logical_service_exclusivity='Not established by Job or image identity; exclusive process tree and service clients are caller prerequisites. Shared brokers unsupported.',
        real_msvc_scope='Unsupported; no verified instance isolation or production integration. MSPDBSRV/VCTIP helper policies rejected; Source35/36 failures unchanged.')
    native = None
    handles = []
    attributes = None
    job = process = thread = None
    assigned = resumed = False
    known = {}
    output_handles = []
    helper_pins = {}
    final = None
    closed_handles = set()

    def own(role, handle):
        handles.append((role, handle))
        return handle

    def gap(reason, pid):
        result['metadata_gap_count'] += 1
        if len(result['metadata_gaps']) < 16:
            result['metadata_gaps'].append(dict(reason=reason, pid=pid))

    def observe(enforce_budget=True):
        nonlocal final
        snapshot = native.snapshot(job, max_live_processes)
        final = snapshot
        if not enforce_budget:
            return snapshot  # Emergency closure uses kernel accounting, not a failed operation budget.
        if snapshot['total_processes'] > max_total_processes:
            raise OSError('total Job process budget')
        for pid in snapshot['pids']:
            if pid in known:
                continue
            try:
                handle = own('observed_process', native.open_process(pid))
                if not native.membership(handle, job):
                    raise OSError('queried PID no longer belongs to owned Job; never adopted')
                identity = native.identity(handle)
                if identity['pid'] != pid:
                    raise OSError('held process PID mismatch')
                identity.update(in_owned_job=True, handle_scope='held until final close')
                known[pid] = (handle, identity)
                result['processes'].append(identity)
            except OSError as error:
                if getattr(error, 'winerror', None) == 87 or getattr(error, 'errno', None) == 87:
                    gap('exited before process handle/metadata observation', pid)
                    continue
                raise
        if sum(native.size(h) for h in output_handles) > max_output_bytes:
            raise OSError('output byte budget')
        return snapshot

    def wait_empty(deadline, fence=None, *, enforce_budget=True):
        empty_since = None
        while time.monotonic() <= deadline:
            snapshot = observe(enforce_budget)
            if fence is not None and snapshot['total_processes'] != fence:
                raise OSError('Job membership changed after parent return/cleanup fence')
            empty = not snapshot['pids'] and snapshot['active_processes'] == 0
            if empty:
                if empty_since is None:
                    empty_since = time.monotonic()
                if time.monotonic()-empty_since >= .15:
                    result['containment']['empty_verified'] = True
                    return
            else:
                empty_since = None
            time.sleep(poll_s)
        raise TimeoutError('bounded Job cleanup/quiescence timeout')

    try:
        if not all(type(v) in (int,float) and math.isfinite(v) and v > 0 for v in (timeout_s,cleanup_timeout_s,poll_s)):
            raise ValueError('positive finite time budgets required')
        if not .01 <= poll_s <= 1 or not all(type(v) is int and v > 0 for v in (max_live_processes,max_total_processes,max_output_bytes)) or max_live_processes > 1024:
            raise ValueError('invalid explicit budgets')
        if not isinstance(argv,(list,tuple)) or not isinstance(allowed_helpers,(list,tuple)) or len(allowed_helpers)>16:
            raise ValueError('explicit argv sequence and at most16 exact helper images required')
        argv = [str(value) for value in argv]
        if len(subprocess.list2cmdline(argv).encode('utf-16-le'))//2+1>32767:
            raise ValueError('Windows command line character budget')
        cwd = Path(cwd).resolve()
        if not argv or not Path(argv[0]).is_absolute() or not Path(argv[0]).is_file() or not cwd.is_dir() or any('\0' in arg for arg in argv):
            raise ValueError('absolute executable, existing cwd and NUL-free argv required')
        native = _Native()
        job = own('job', native.job(max_live_processes))
        native.configure_job(job, max_live_processes)
        result['containment']['kill_on_job_close'] = True
        for spec in allowed_helpers:
            if not isinstance(spec, dict) or set(spec) != {'path','sha256'} or not re.fullmatch('[0-9a-f]{64}',spec['sha256']):
                raise ValueError('helper policy requires only exact path and lowercase SHA256')
            path = Path(spec['path'])
            if not path.is_absolute() or not path.is_file():
                raise ValueError('absolute existing helper image required')
            handle = own('pinned_helper_image', native.file(path))
            image = native.image(handle)
            if Path(image['path']).name.casefold() in {'mspdbsrv.exe','vctip.exe'}:
                raise ValueError('shared IPC broker helper unsupported without verified instance isolation; no override')
            if image['sha256'] != spec['sha256'] or image['path'] in helper_pins:
                raise ValueError('helper image hash mismatch or duplicate canonical helper')
            helper_pins[image['path']] = image
            result['helper_pins'].append(image)
        stdin = own('stdin_NUL', native.file('NUL', stdin=True))
        stdout = own('stdout', native.file(out/'stdout.log', output=True))
        stderr = own('stderr', native.file(out/'stderr.log', output=True))
        output_handles = [stdout, stderr]
        stdio = [stdin, stdout, stderr]
        attributes = native.attributes(stdio)
        native.restrict_attributes(attributes, stdio)
        environment = dict(os.environ)
        if any('\0' in k or '\0' in v or '=' in k for k,v in environment.items()):
            raise ValueError('invalid inherited environment block')
        if time.monotonic()-started>=timeout_s:
            raise TimeoutError('build command timeout before spawn')
        info = native.create(argv, cwd, environment, stdio, attributes)
        process = own('command_process', info.process)
        thread = own('command_initial_thread', info.thread)
        result['command']['pid'] = info.pid
        native.assign(job, process)
        assigned = result['command']['assigned'] = True
        identity = native.identity(process)
        if not native.membership(process, job) or identity['pid'] != info.pid:
            raise OSError('initial held-handle membership/identity mismatch')
        result['command']['creation_filetime'] = identity['creation_filetime']
        identity.update(in_owned_job=True, handle_scope='original CreateProcessW handle')
        known[info.pid] = (process, identity)
        result['processes'].append(identity)
        native.resume(thread)
        resumed = result['command']['resumed'] = True
        deadline = started+timeout_s
        while native.exit_code(process) is None:
            observe()
            if time.monotonic() >= deadline:
                raise TimeoutError('build command timeout')
            time.sleep(poll_s)
        result['command']['returncode'] = native.exit_code(process)
        result['command']['reaped'] = True
        if result['command']['returncode'] != 0:
            raise OSError('build command nonzero exit')
        snapshot = observe()
        fence = snapshot['total_processes']
        cleanup_deadline = time.monotonic()+cleanup_timeout_s
        grace_end = min(cleanup_deadline,time.monotonic()+.25)
        while (snapshot['pids'] or snapshot['active_processes']) and time.monotonic()<grace_end:
            time.sleep(poll_s)
            snapshot = observe()
            if snapshot['total_processes']!=fence:
                raise OSError('Job membership changed after parent return/cleanup fence')
        # A bounded natural drain lets transient descendants exit; every survivor
        # after the grace still needs the exact explicit helper policy.
        # Validate EVERY current survivor before taking any planned action.
        survivors = []
        for pid in snapshot['pids']:
            if pid not in known:
                raise OSError('survivor metadata unavailable')
            handle, old = known[pid]
            if native.exit_code(handle) is not None:
                continue
            identity = native.identity(handle)
            if identity != {k:old[k] for k in ('pid','creation_filetime','image_path')} or not native.membership(handle,job):
                raise OSError('survivor held-handle identity or membership changed')
            path = identity['image_path']
            if path not in helper_pins:
                raise OSError('unknown surviving Job member; strict default')
            image_handle = own('survivor_image', native.file(path))
            image = native.image(image_handle)
            if image != helper_pins[path]:
                raise OSError('survivor exact held image path/hash mismatch')
            survivors.append((handle, {**identity, 'in_owned_job':True, 'image':image}))
        for handle, identity in survivors:
            if not native.membership(handle,job):
                raise OSError('helper left owned Job before planned action')
            if native.exit_code(handle) is None:
                native.terminate_process(handle)
                result['planned_actions'].append({**identity,'action':'TerminateProcess(held handle)', 'reason':'explicit planned exclusively owned helper teardown after command0'})
        wait_empty(cleanup_deadline, fence)
        result['status'] = 'PASS_PLANNED_HELPER_CLEANUP' if result['planned_actions'] else 'PASS_NATURAL'
    except BaseException as error:
        result['failure'] = ('cancelled: ' if isinstance(error,KeyboardInterrupt) else '') + repr(error)
    finally:
        if native is not None:
            if result['status'] == 'FAIL':
                if job is not None:
                    try:
                        native.terminate_job(job)
                        result['emergency_actions'].append(dict(action='TerminateJobObject',reason='failure containment',requested=True))
                    except BaseException as error:
                        result['cleanup_errors'].append('job termination: '+repr(error))
                if process is not None and not assigned:
                    try:
                        if native.exit_code(process) is None:
                            native.terminate_process(process)
                            result['emergency_actions'].append(dict(action='TerminateProcess(original unassigned suspended handle)',requested=True))
                    except BaseException as error:
                        result['cleanup_errors'].append('unassigned child cleanup: '+repr(error))
                if job is not None:
                    try:
                        wait_empty(time.monotonic()+cleanup_timeout_s,enforce_budget=False)
                    except BaseException as error:
                        result['containment']['snapshot_error'] = repr(error)
                if process is not None:
                    try:
                        until = time.monotonic()+cleanup_timeout_s
                        while native.exit_code(process) is None and time.monotonic() < until:
                            time.sleep(poll_s)
                        code = native.exit_code(process)
                        result['command']['reaped'] = code is not None
                        if result['command']['returncode'] is None:
                            result['command']['returncode'] = code
                    except BaseException as error:
                        result['cleanup_errors'].append('command reap: '+repr(error))
            result['containment']['final_snapshot'] = final
            if final:
                result['unobserved_short_lived_metadata_lower_bound'] = max(0,final['total_processes']-len(known))
            if attributes is not None:
                try:
                    native.delete_attributes(attributes)
                    result['attribute_cleanup'] = 'DeleteProcThreadAttributeList returned'
                except BaseException as error:
                    result['cleanup_errors'].append('attribute cleanup unverified: '+repr(error))
            # One close attempt per owned handle, even after an earlier failure.
            for role, handle in reversed(handles):
                if handle in closed_handles:
                    result['cleanup_errors'].append('duplicate owned handle registration: '+role)
                    continue
                closed_handles.add(handle)
                try:
                    native.close(handle)
                    result['handle_closes'].append(dict(role=role,returned=True))
                except BaseException as error:
                    result['handle_closes'].append(dict(role=role,returned=False,error=repr(error)))
                    result['cleanup_errors'].append('handle close unverified: '+role+': '+repr(error))
        if result['cleanup_errors'] or not result['containment']['empty_verified']:
            result['status'] = 'FAIL'
        result['elapsed_s'] = time.monotonic()-started
        _atomic_receipt(out/'receipt.json',result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--purpose', choices=['build'], required=True)
    parser.add_argument('--cwd', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--timeout', type=float, default=1800)
    parser.add_argument('--cleanup-timeout', type=float, default=10)
    parser.add_argument('--helper', nargs=2, action='append', metavar=('ABSOLUTE_IMAGE','SHA256'), default=[])
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('explicit build argv after -- is required')
    result = run_build_job(command,cwd=args.cwd,output_dir=args.out,timeout_s=args.timeout,
                           cleanup_timeout_s=args.cleanup_timeout,
                           allowed_helpers=[dict(path=path,sha256=digest) for path,digest in args.helper])
    print(json.dumps({'status':result['status'],'failure':result['failure'],'receipt':str(Path(args.out)/'receipt.json')}))
    return 0 if result['status'].startswith('PASS_') else 1


if __name__ == '__main__':
    raise SystemExit(main())
