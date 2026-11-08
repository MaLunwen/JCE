"""Canonical read-only native port for new Windows AMD64 memory observations.

Promotes the VQE/handle contract from the frozen residency-attribution210 probe.
Historical observers remain evidence, not a second active measurement owner.
No process memory contents, UI actions or target allocation policy are accessed.
"""
import ctypes
from ctypes import wintypes
import sys


class ObservationError(RuntimeError):
    def __init__(self, stage, reason, code=None):
        super().__init__(reason)
        self.stage, self.reason, self.code = stage, reason, code


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wintypes.DWORD), ("PartitionId", wintypes.WORD),
        ("RegionSize", ctypes.c_size_t), ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD), ("Type", wintypes.DWORD)]


class SYSTEM_INFO(ctypes.Structure):
    _fields_ = [("Architecture", wintypes.WORD), ("Reserved", wintypes.WORD),
        ("PageSize", wintypes.DWORD), ("MinimumAddress", ctypes.c_void_p),
        ("MaximumAddress", ctypes.c_void_p), ("ProcessorMask", ctypes.c_size_t),
        ("Processors", wintypes.DWORD), ("ProcessorType", wintypes.DWORD),
        ("AllocationGranularity", wintypes.DWORD), ("ProcessorLevel", wintypes.WORD),
        ("ProcessorRevision", wintypes.WORD)]


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in ("PeakWorkingSetSize", "WorkingSetSize",
        "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
        "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage", "PrivateUsage")]


MAPPED_FILE_NAME_CHARS = 32768
WORKING_SET_PAGE_LIMIT = 4096


class WORKING_SET_EX_INFORMATION(ctypes.Structure):
    # PSAPI_WORKING_SET_EX_INFORMATION: pointer and ULONG_PTR union Flags.
    _fields_ = [("VirtualAddress", ctypes.c_void_p), ("Flags", ctypes.c_size_t)]


class WindowsReader:
    """One query/synchronize handle; context close is idempotent, including faults."""
    def __init__(self, pid, *, mapped_file_names=False, working_set_pages=False):
        self.handle = None
        if type(mapped_file_names) is not bool:
            raise ValueError("mapped-file opt-in must be boolean")
        if type(working_set_pages) is not bool:
            raise ValueError("working-set page opt-in must be boolean")
        self.working_set_pages = working_set_pages
        self._working_set_api = None
        self._working_set_api_unavailable = None
        self.mapped_file_names = mapped_file_names
        self._mapped_file_api = None
        self._mapped_file_api_unavailable = None
        if isinstance(pid, bool) or not isinstance(pid, int) or not 0 < pid < 1 << 32:
            raise ValueError("positive DWORD PID required")
        if sys.platform != "win32" or ctypes.sizeof(ctypes.c_void_p) != 8:
            raise ObservationError("platform", "native Windows x64 Python required")
        if (ctypes.sizeof(MEMORY_BASIC_INFORMATION) != 48 or
                MEMORY_BASIC_INFORMATION.RegionSize.offset != 24 or
                ctypes.sizeof(PROCESS_MEMORY_COUNTERS_EX) != 80 or
                ctypes.sizeof(WORKING_SET_EX_INFORMATION) != 16 or
                WORKING_SET_EX_INFORMATION.Flags.offset != 8):
            raise ObservationError("layout", "native SDK x64 memory layouts required")
        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        try:
            signatures = {
                "OpenProcess": ([wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
                "CloseHandle": ([wintypes.HANDLE], wintypes.BOOL),
                "GetProcessId": ([wintypes.HANDLE], wintypes.DWORD),
                "WaitForSingleObject": ([wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
                "IsWow64Process2": ([wintypes.HANDLE, ctypes.POINTER(wintypes.USHORT), ctypes.POINTER(wintypes.USHORT)], wintypes.BOOL),
                "GetSystemInfo": ([ctypes.POINTER(SYSTEM_INFO)], None),
                "VirtualQueryEx": ([wintypes.HANDLE, ctypes.c_void_p, ctypes.POINTER(MEMORY_BASIC_INFORMATION), ctypes.c_size_t], ctypes.c_size_t),
                "GetProcessTimes": ([wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4, wintypes.BOOL),
                "QueryFullProcessImageNameW": ([wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
                "K32GetProcessMemoryInfo": ([wintypes.HANDLE, ctypes.POINTER(PROCESS_MEMORY_COUNTERS_EX), wintypes.DWORD], wintypes.BOOL),
                "QueryPerformanceCounter": ([ctypes.POINTER(ctypes.c_int64)], wintypes.BOOL),
                "QueryPerformanceFrequency": ([ctypes.POINTER(ctypes.c_int64)], wintypes.BOOL),
            }
            for name, (arguments, result) in signatures.items():
                function = getattr(self.api, name)
                function.argtypes, function.restype = arguments, result
        except AttributeError as error:
            raise ObservationError("platform_api", "required native host API unavailable: " + str(error)) from error
        if mapped_file_names:
            try:
                function = self.api.K32GetMappedFileNameW
                function.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.LPWSTR, wintypes.DWORD]
                function.restype = wintypes.DWORD
                self._mapped_file_api = function
            except AttributeError as error:
                # No native call occurred; there is no invented Win32 code.
                self._mapped_file_api_unavailable = str(error)
        if working_set_pages:
            try:
                function = self.api.K32QueryWorkingSetEx
                function.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
                function.restype = wintypes.BOOL
                self._working_set_api = function
            except AttributeError as error:
                self._working_set_api_unavailable = str(error)
        # Microsoft documents QUERY_INFORMATION for both optional PSAPI calls.
        # Keep the same single handle; never request VM_READ/VM_WRITE.
        self.handle = self.api.OpenProcess(0x0400 | 0x00100000, False, pid)
        if not self.handle:
            self._fail("OpenProcess")
        try:
            process_machine, native_machine = wintypes.USHORT(), wintypes.USHORT()
            if not self.api.IsWow64Process2(self.handle, ctypes.byref(process_machine), ctypes.byref(native_machine)):
                self._fail("IsWow64Process2")
            if process_machine.value != 0 or native_machine.value != 0x8664:
                raise ObservationError("target_arch", "only native AMD64 targets admitted; WOW64/ARM unavailable")
            system = SYSTEM_INFO()
            self.api.GetSystemInfo(ctypes.byref(system))
            self.bounds = dict(minimum_address=int(system.MinimumAddress or 0),
                maximum_address=int(system.MaximumAddress or 0), page_bytes=int(system.PageSize), pointer_bits=64)
            frequency = ctypes.c_int64()
            if not self.api.QueryPerformanceFrequency(ctypes.byref(frequency)):
                self._fail("QueryPerformanceFrequency")
            if frequency.value <= 0:
                raise ObservationError("qpc", "nonpositive QPC frequency")
            self.frequency = frequency.value
        except BaseException:
            self.close()
            raise

    def _fail(self, stage):
        error = ctypes.get_last_error()
        raise ObservationError(stage, f"{stage} failed (Win32 error {error})", error)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        handle, self.handle = self.handle, None
        if handle and not self.api.CloseHandle(handle):
            self._fail("CloseHandle")

    def alive(self):
        value = self.api.WaitForSingleObject(self.handle, 0)
        if value == 0x102:
            return True
        if value == 0:
            return False
        self._fail("WaitForSingleObject")

    def _times(self):
        values = [wintypes.FILETIME() for _ in range(4)]
        if not self.api.GetProcessTimes(self.handle, *(ctypes.byref(value) for value in values)):
            self._fail("GetProcessTimes")
        return [value.dwLowDateTime | (value.dwHighDateTime << 32) for value in values]

    def identity(self):
        pid = self.api.GetProcessId(self.handle)
        if not pid:
            self._fail("GetProcessId")
        creation, _, _, _ = self._times()
        buffer = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(buffer))
        if not self.api.QueryFullProcessImageNameW(self.handle, 0, buffer, ctypes.byref(length)):
            self._fail("QueryFullProcessImageNameW")
        if not 0 < length.value < len(buffer):
            raise ObservationError("identity", "empty/truncated EXE path")
        return dict(pid=int(pid), creation_filetime_100ns=creation,
                    exe=buffer.value[:length.value], alive=self.alive())

    def qpc(self):
        counter = ctypes.c_int64()
        if not self.api.QueryPerformanceCounter(ctypes.byref(counter)):
            self._fail("QueryPerformanceCounter")
        return dict(ticks=counter.value, frequency=self.frequency)

    def sample(self):
        memory = PROCESS_MEMORY_COUNTERS_EX()
        memory.cb = ctypes.sizeof(memory)
        if not self.api.K32GetProcessMemoryInfo(self.handle, ctypes.byref(memory), memory.cb):
            self._fail("K32GetProcessMemoryInfo")
        _, _, kernel, user = self._times()
        return dict(rss_bytes=int(memory.WorkingSetSize), private_commit_bytes=int(memory.PrivateUsage),
                    cpu_time_100ns=kernel + user, thread_count=None)

    def region(self, address):
        info = MEMORY_BASIC_INFORMATION()
        size = self.api.VirtualQueryEx(self.handle, ctypes.c_void_p(address), ctypes.byref(info), ctypes.sizeof(info))
        if size != ctypes.sizeof(info):
            self._fail("VirtualQueryEx")
        return dict(base=int(info.BaseAddress or 0), allocation_base=int(info.AllocationBase or 0),
            size=int(info.RegionSize), state=int(info.State), type=int(info.Type), protect=int(info.Protect))

    def mapped_file_name(self, address):
        """Optional device-form filename metadata; no process contents read.

        One fixed buffer and one API call. nSize is truncation, not a name.
        See Microsoft psapi/nf-psapi-getmappedfilenamew. Missing exports and
        zero/error/truncated results remain unavailable, not anonymous heaps.
        """
        if not self.mapped_file_names:
            raise ObservationError("mapped_file_names", "mapped-file metadata was not requested")
        if type(address) is not int or not 0 <= address < 1 << 64:
            raise ValueError("native mapped address required")
        if not self.handle:
            raise ObservationError("mapped_file_names", "process handle is closed")
        if self._mapped_file_api is None:
            raise ObservationError("K32GetMappedFileNameW", "optional API unavailable: " + str(self._mapped_file_api_unavailable))
        buffer = ctypes.create_unicode_buffer(MAPPED_FILE_NAME_CHARS)
        ctypes.set_last_error(0)
        length = self._mapped_file_api(self.handle, ctypes.c_void_p(address), buffer, len(buffer))
        error = ctypes.get_last_error()
        if type(length) is not int or length < 0 or length > len(buffer):
            raise ObservationError("mapped_file_name_response", "invalid API character count", error or None)
        if length == 0:
            raise ObservationError("K32GetMappedFileNameW", "mapped filename unavailable", error)
        if length == len(buffer):
            fault = ObservationError("K32GetMappedFileNameW", "mapped filename truncated at fixed buffer bound", error or None)
            fault.truncated = True
            raise fault
        name = buffer[:length]
        if not name or "\0" in name or any(ord(character) < 32 for character in name) or buffer[length] != "\0":
            raise ObservationError("mapped_file_name_response", "empty, embedded-NUL or unterminated mapped filename", error or None)
        return name


    def working_set_page_flags(self, addresses):
        """One bounded metadata call on the retained query handle, default off.

        Returns raw ULONG_PTR flags, not process contents or an ownership sum.
        Only fields with Valid bit0 set can be interpreted as valid-page data.
        QueryWorkingSetEx also admits AWE/large pages outside the working set;
        callers retain non-atomic QPC/identity windows and selected-page scope.
        See Microsoft psapi/nf-psapi-queryworkingsetex and
        psapi/ns-psapi-psapi_working_set_ex_block.
        """
        if not self.working_set_pages:
            raise ObservationError("working_set_pages", "page metadata was not requested")
        if not self.handle:
            raise ObservationError("working_set_pages", "process handle is closed")
        if self._working_set_api is None:
            raise ObservationError("K32QueryWorkingSetEx", "optional API unavailable: " + str(self._working_set_api_unavailable))
        if not isinstance(addresses, (list, tuple)) or not 0 < len(addresses) <= WORKING_SET_PAGE_LIMIT:
            raise ValueError("bounded nonempty page address list required")
        page = self.bounds.get("page_bytes")
        low, high = self.bounds.get("minimum_address"), self.bounds.get("maximum_address")
        if (type(page) is not int or not 0 < page <= 1 << 20 or page & (page - 1) or
                type(low) is not int or type(high) is not int or not 0 <= low <= high < 1 << 64):
            raise ObservationError("working_set_pages", "invalid native page bounds")
        previous = -1
        for address in addresses:
            if type(address) is not int or not low <= address <= high or address % page or address <= previous:
                raise ValueError("ordered unique aligned native page addresses required")
            previous = address
        buffer = (WORKING_SET_EX_INFORMATION * len(addresses))()
        for entry, address in zip(buffer, addresses):
            entry.VirtualAddress = address
        if not self._working_set_api(self.handle, buffer, ctypes.sizeof(buffer)):
            self._fail("K32QueryWorkingSetEx")
        if any(int(entry.VirtualAddress or 0) != address for entry, address in zip(buffer, addresses)):
            raise ObservationError("working_set_page_response", "API altered a requested page address")
        return [int(entry.Flags) for entry in buffer]
