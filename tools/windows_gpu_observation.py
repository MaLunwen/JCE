"""Opt-in Windows PDH GPU Engine rates; no renderer/presentation frame join.

English wildcard handles are consumed with the formatted counter-array API.
Microsoft: nf-pdh-pdhaddenglishcounterw, nf-pdh-pdhgetformattedcounterarrayw,
perfctrs/collecting-performance-data, perfctrs/pdh-error-codes.
Only explicit PID/birth subjects are retained; queries may enumerate more OS
instances. Output caps do not cap OS work. Independent engines are never summed.
"""
from __future__ import annotations
import ctypes as c
import math
import re
import sys
import time

COUNTER_PATH = r"\GPU Engine(*)\Utilization Percentage"
MORE_DATA = 0x800007D2
NO_DATA = 0x800007D5
INVALID_DATA = 0xC0000BBA
DENIED = (5, 0xC0000BDB)
UNSUPPORTED = (0xC0000BB8, 0xC0000BB9, 0xC0000BBE, 0xC0000BD3)
BUFFER_LIMIT = 16 * 1024 * 1024
RATE_INTERVAL = 1.0
DWORD = c.c_uint32
HANDLE = c.c_void_p


class _ValueUnion(c.Union):
    _fields_ = [("longValue", c.c_int32), ("doubleValue", c.c_double),
                ("largeValue", c.c_int64), ("stringValue", c.c_void_p)]


class _Value(c.Structure):
    _fields_ = [("CStatus", DWORD), ("value", _ValueUnion)]


class _Item(c.Structure):
    # Name pointers are range-checked before decoding Windows UTF-16LE.
    _fields_ = [("name", c.c_void_p), ("formatted", _Value)]


class _DataError(ValueError):
    pass


def _status(value):
    if type(value) is not int or not 0 <= value <= 0xffffffff:
        raise TypeError("PDH port must return an exact DWORD status")
    return value


def _reason(status):
    if status in DENIED: return "DENIED"
    if status in UNSUPPORTED: return "UNSUPPORTED_COUNTER"
    if status == NO_DATA: return "NO_DATA"
    return "PDH_ERROR"


class _Pdh:
    """Actual ctypes call owner; optional DLL port exercises these same bodies."""
    def __init__(self, dll):
        signatures = {
            "PdhOpenQueryW": [c.c_wchar_p, c.c_size_t, c.POINTER(HANDLE)],
            "PdhAddEnglishCounterW": [HANDLE, c.c_wchar_p, c.c_size_t, c.POINTER(HANDLE)],
            "PdhCollectQueryData": [HANDLE],
            "PdhGetFormattedCounterArrayW": [HANDLE, DWORD, c.POINTER(DWORD), c.POINTER(DWORD), c.POINTER(_Item)],
            "PdhCloseQuery": [HANDLE]}
        self.dll, self.query, self.counter = dll, HANDLE(), HANDLE()
        self.close_status = None
        for name, args in signatures.items():
            fn = getattr(dll, name)
            fn.argtypes, fn.restype = args, DWORD
        try:
            self.status = _status(dll.PdhOpenQueryW(None, 0, c.byref(self.query)))
            if not self.status:
                if not self.query.value: raise _DataError("NULL_QUERY")
                self.status = _status(dll.PdhAddEnglishCounterW(self.query, COUNTER_PATH, 0, c.byref(self.counter)))
                if not self.status and not self.counter.value: raise _DataError("NULL_COUNTER")
            if self.status: self.close()
        except BaseException:
            self.close()
            raise

    def close(self):
        query, self.query = self.query, HANDLE()
        self.counter = HANDLE()
        if query.value:
            self.close_status = _status(self.dll.PdhCloseQuery(query))
        return self.close_status

    def collect(self):
        return _status(self.dll.PdhCollectQueryData(self.query))

    def values(self):
        # On growth, use a fresh zero-size probe, never trust the failed fill's
        # returned size. At most two attempts; no sleeps or unbounded retry.
        for _ in range(2):
            size, count = DWORD(), DWORD()
            status = _status(self.dll.PdhGetFormattedCounterArrayW(self.counter, 0x200 | 0x8000,
                               c.byref(size), c.byref(count), None))
            if status == 0 and not size.value and not count.value: return 0, []
            if status != MORE_DATA: return status, []
            if not 0 < size.value <= BUFFER_LIMIT: raise _DataError("COUNTER_BUFFER_LIMIT")
            buffer = c.create_string_buffer(size.value)
            capacity = size.value
            status = _status(self.dll.PdhGetFormattedCounterArrayW(self.counter, 0x200 | 0x8000,
                c.byref(size), c.byref(count), c.cast(buffer, c.POINTER(_Item))))
            if status in (MORE_DATA, 0xC0000BBD): continue
            if status: return status, []
            if size.value > capacity or count.value > size.value // c.sizeof(_Item):
                raise _DataError("INVALID_ARRAY_BOUND")
            return 0, self._items(buffer, size.value, count.value)
        raise _DataError("COUNTER_ARRAY_CHANGED")

    @staticmethod
    def _items(buffer, size, count):
        base = c.addressof(buffer)
        items = c.cast(buffer, c.POINTER(_Item))
        # A generator retains no separate OS-instance list.
        for index in range(count):
            item = items[index]
            address = item.name
            if not address or address < base + count * c.sizeof(_Item) or address >= base + size or address % 2:
                raise _DataError("INVALID_NAME_POINTER")
            length = min(size - (address - base), 4098)
            raw = c.string_at(address, length)
            stop = next((i for i in range(0, len(raw) - 1, 2) if raw[i:i+2] == b"\0\0"), None)
            if stop is None: raise _DataError("INVALID_NAME_BOUND")
            try: name = raw[:stop].decode("utf-16-le", errors="strict")
            except UnicodeError as error: raise _DataError("INVALID_NAME_ENCODING") from error
            yield name, item.formatted.CStatus, item.formatted.value.doubleValue


INSTANCE = re.compile(r"pid_([1-9][0-9]*)_luid_0x([0-9a-fA-F]{1,8})_0x([0-9a-fA-F]{1,8})_phys_([0-9]+)_eng_([0-9]+)_engtype_((?:[A-Za-z0-9][A-Za-z0-9_ ]{0,63})?)(?:#([0-9]+))?\Z")


class WindowsGpuObservation:
    def __init__(self, *, dll=None, platform=None, clock=time.monotonic):
        self.dll, self.platform, self.clock = dll, sys.platform if platform is None else platform, clock
        self.api, self.last_collection, self.failure = None, None, None
        self.close_status = None
        self.closed = False

    def close(self):
        self.closed = True
        api, self.api = self.api, None
        if api is not None: self.close_status = api.close()
        return self.close_status

    def sample(self, subjects, origin, row_limit):
        start = self.clock()
        result = dict(available=False, reason="UNAVAILABLE", rows=[], subjects=subjects,
            observed_matching_rows=0, query_instance_count=0, query_data_complete=False, output_truncated=False,
            unparsed_instance_witnesses=[], unparsed_instance_count=0, unparsed_instance_unretained_count=0,
            output_row_limit=row_limit, query_buffer_limit_bytes=BUFFER_LIMIT,
            query_start_elapsed_s=start-origin, rate_window_start_elapsed_s=None,
            scope="PDH per-PID/LUID/physical-GPU/engine rates in observer collection windows; no sum, GPU memory, FPS, presentation, fence or CPU/GPU frame join")
        try:
            if self.closed: result["reason"] = "CLOSED"; return result
            if not math.isfinite(start): raise ValueError("GPU observer clock must be finite")
            if self.last_collection is not None and start < self.last_collection:
                raise ValueError("GPU observer clock must be monotonic")
            valid = {s["pid"]: s for s in subjects if s["identity_available"]}
            if self.platform != "win32": result["reason"] = "UNSUPPORTED_PLATFORM"; return result
            if not valid: result["reason"] = "IDENTITY_UNAVAILABLE"; return result
            if self.failure: result.update(self.failure); return result
            if self.api is None:
                try: dll = self.dll if self.dll is not None else c.WinDLL("pdh.dll", use_last_error=True)
                except OSError as error:
                    self.failure = dict(reason="DLL_UNAVAILABLE", os_error=getattr(error, "winerror", error.errno))
                    result.update(self.failure); return result
                try: self.api = _Pdh(dll)
                except _DataError as error:
                    self.failure = dict(reason=str(error)); result.update(self.failure); return result
                if self.api.status:
                    self.failure = dict(reason=_reason(self.api.status), pdh_status=f"0x{self.api.status:08X}")
                    result.update(self.failure); return result
            if self.last_collection is not None and start-self.last_collection < RATE_INTERVAL:
                result["reason"] = "WAITING_RATE_WINDOW"; return result
            status = self.api.collect()
            if status:
                self.last_collection = None
                result.update(reason=_reason(status), pdh_status=f"0x{status:08X}"); return result
            previous = self.last_collection
            self.last_collection = self.clock()
            if not math.isfinite(self.last_collection) or self.last_collection < start:
                raise ValueError("GPU observer clock must be finite and monotonic")
            if previous is None: result["reason"] = "PRIMING"; return result
            result["rate_window_start_elapsed_s"] = previous-origin
            status, items = self.api.values()
            if status:
                result.update(reason=_reason(status), pdh_status=f"0x{status:08X}"); return result
            result["query_available"] = True
            invalid = 0
            for name, cstatus, value in items:
                result["query_instance_count"] += 1
                match = INSTANCE.fullmatch(name)
                if not match:
                    selected = next((pid for pid in valid if name.startswith(f"pid_{pid}_")), None)
                    if selected is not None:
                        invalid += 1
                        result["unparsed_instance_count"] += 1
                        if len(result["unparsed_instance_witnesses"]) < 8:
                            subject = valid[selected]
                            result["unparsed_instance_witnesses"].append(dict(pid=selected,
                                creation_time_unix_s=subject["creation_time_unix_s"], purpose=subject["purpose"],
                                instance_name=name[:256], name_truncated=len(name)>256,
                                cstatus=f"0x{cstatus:08X}", reason="UNPARSED_NAME",
                                identity_available=True, identity_reason="AVAILABLE"))
                        else: result["unparsed_instance_unretained_count"] += 1
                    continue
                pid, high, low, physical, engine, kind, suffix = match.groups()
                subject = valid.get(int(pid))
                if subject is None: continue
                result["observed_matching_rows"] += 1
                status_ok = cstatus in (0, 1)
                value_finite = status_ok and math.isfinite(value)
                ok = value_finite and 0 <= value <= 100
                invalid += not ok
                if len(result["rows"]) < row_limit:
                    row = dict(pid=int(pid), creation_time_unix_s=subject["creation_time_unix_s"],
                        purpose=subject["purpose"], adapter_luid=f"{int(high,16):08X}:{int(low,16):08X}",
                        physical_gpu_index=int(physical), engine_index=int(engine), engine_type=kind or None,
                        engine_type_available=bool(kind), engine_type_reason="AVAILABLE" if kind else "EMPTY_OS_LABEL",
                        instance_suffix=suffix, cstatus=f"0x{cstatus:08X}", available=ok,
                        utilization_percent=value if ok else None,
                        reason="AVAILABLE" if ok else "INVALID_COUNTER_DATA")
                    if not ok:
                        if not status_ok:
                            issue = "PDH_STATUS"
                        elif not value_finite:
                            issue = "NONFINITE"
                        else:
                            issue = "OUT_OF_RANGE"
                        row["invalid_counter_detail"] = dict(issue=issue,
                            formatted_value=value if value_finite else None)
                    result["rows"].append(row)
            result["query_data_complete"] = True
            result["output_truncated"] = result["observed_matching_rows"] > len(result["rows"])
            result["invalid_matching_rows"] = invalid
            result["available"] = bool(result["rows"]) and any(row["available"] for row in result["rows"])
            result["reason"] = ("OUTPUT_TRUNCATED" if result["output_truncated"] else "PARTIAL_INVALID" if invalid
                                else "AVAILABLE" if result["available"] else "NO_MATCHING_INSTANCE")
            return result
        except _DataError as error:
            result.update(reason=str(error), rows=[], available=False); return result
        finally:
            end = self.clock()
            result["query_end_elapsed_s"] = end-origin
            result["query_elapsed_s"] = end-start
