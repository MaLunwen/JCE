#!/usr/bin/env python3
"""Bounded, non-atomic Windows address-space and optional page observations.

VQE type/AllocationBase metadata is not a heap owner, RSS, GPU residency or a
PrivateUsage identity. Image/mapped copy-on-write pages keep their VQE type.
Optional page flags retain selected-address coverage rather than an RSS sum.
"""
import argparse
import hashlib
import json
import math
import ntpath
from pathlib import Path
import sys
import time

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.windows_memory_reader import (ObservationError, WindowsReader,
    MAPPED_FILE_NAME_CHARS, WORKING_SET_PAGE_LIMIT)

MEM_COMMIT, MEM_RESERVE, MEM_FREE = 0x1000, 0x2000, 0x10000
TYPE_NAMES = {0x20000: "MEM_PRIVATE", 0x40000: "MEM_MAPPED", 0x1000000: "MEM_IMAGE"}
UINT64_END = 1 << 64
SCOPE = ("Non-atomic VirtualQueryEx metadata over one native AMD64 process handle. "
    "Committed region bytes and reserved address space are separate. VQE type/AllocationBase "
    "is not heap ownership, WorkingSet, GPU memory, or an equality proof for PrivateUsage; "
    "mapped/image copy-on-write pages retain their original VQE type. No target contents are read.")


def integer(value, name, minimum=0, maximum=UINT64_END-1):
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise ValueError("invalid " + name)
    return value


def walk_regions(query, end, *, page_bytes=4096, max_regions=16384,
                 max_groups=4096, max_seconds=2.0, clock=time.monotonic, alive=lambda: True,
                 retain_committed_spans=False):
    """Consume actual port responses; faults expose a prefix, never full totals."""
    integer(end, "address end", 1, UINT64_END)
    integer(page_bytes, "page size", 1, 1 << 20)
    if page_bytes & (page_bytes-1) or end % page_bytes:
        raise ValueError("page-aligned bounds required")
    integer(max_regions, "query bound", 1, 65536)
    integer(max_groups, "allocation group bound", 1, 16384)
    if type(retain_committed_spans) is not bool:
        raise ValueError("committed-span opt-in must be boolean")
    if isinstance(max_seconds, bool) or not isinstance(max_seconds, (int,float)) or not math.isfinite(max_seconds) or not 0 < max_seconds <= 10:
        raise ValueError("finite time budget in (0,10] required")
    begin = clock()
    if isinstance(begin, bool) or not isinstance(begin, (int,float)) or not math.isfinite(begin):
        raise ValueError("finite clock required")
    previous = begin
    cursor, calls = 0, 0
    committed = {name: 0 for name in TYPE_NAMES.values()}
    reserved = {name: 0 for name in (*TYPE_NAMES.values(), "UNDEFINED_RESERVED")}
    groups = {}
    spans = []
    error = None
    clipped_tail = False
    while cursor < end:
        try:
            now = clock()
            if not isinstance(now, (int,float)) or isinstance(now, bool) or not math.isfinite(now) or now < previous:
                raise ObservationError("clock", "invalid/nonmonotonic time port")
            previous = now
            if now-begin >= max_seconds or calls >= max_regions:
                raise ObservationError("budget", "query/time bound exhausted")
            if alive() is not True:
                raise ObservationError("process_exit", "target exited during address-space walk")
            calls += 1
            region = query(cursor)
            if not isinstance(region, dict):
                raise ValueError("region response must be an object")
            base = integer(region.get("base"), "region base")
            size = integer(region.get("size"), "region size", 1)
            state = integer(region.get("state"), "region state")
            next_address = base + size
            if next_address > UINT64_END:
                raise ValueError("region end overflow")
            if base % page_bytes or size % page_bytes or not base <= cursor < next_address:
                raise ValueError("unaligned/nonadvancing/overlapping region response")
            stop = min(next_address, end)
            length = stop-cursor
            if state not in (MEM_COMMIT, MEM_RESERVE, MEM_FREE):
                raise ValueError("unsupported region state")
            if state != MEM_FREE:
                allocation = integer(region.get("allocation_base"), "allocation base")
                kind = integer(region.get("type"), "region type")
                if allocation > base or allocation % page_bytes:
                    raise ValueError("invalid allocation base")
                name = TYPE_NAMES.get(kind)
                if name is None and state == MEM_RESERVE and kind == 0:
                    name = "UNDEFINED_RESERVED"
                if name is None:
                    raise ValueError("unsupported committed/reserved region type")
                key = (name, allocation)
                if key not in groups:
                    if len(groups) >= max_groups:
                        raise ObservationError("budget", "allocation group bound exhausted")
                    groups[key] = dict(type=name, allocation_base=hex(allocation), regions=0,
                                       committed_bytes=0, reserved_bytes=0)
                group = groups[key]
                group["regions"] += 1
                if state == MEM_COMMIT:
                    committed[name] += length
                    group["committed_bytes"] += length
                    if retain_committed_spans:
                        spans.append(dict(base=hex(cursor), end_exclusive=hex(stop),
                            allocation_base=hex(allocation), type=name))
                else:
                    reserved[name] += length
                    group["reserved_bytes"] += length
            clipped_tail |= stop != next_address
            cursor = stop
        except (ObservationError, ValueError) as fault:
            error = dict(stage=getattr(fault,"stage","region_response"), reason=str(fault),
                         win32_error=getattr(fault,"code",None), query_address=hex(cursor))
            break
    observed = dict(committed_by_type=committed, reserved_by_type=reserved,
        committed_bytes=sum(committed.values()), reserved_bytes=sum(reserved.values()),
        allocation_groups=sorted(groups.values(), key=lambda g: (-g["committed_bytes"],-g["reserved_bytes"],g["allocation_base"])))
    complete = cursor == end and error is None
    result = dict(status="COMPLETE" if complete else "PARTIAL", address_space_complete=complete,
        start_address="0x0", end_exclusive=hex(end), covered_end_exclusive=hex(cursor),
        query_calls=calls, clipped_final_region=clipped_tail, error=error,
        full_totals=observed if complete else None, observed_prefix=observed)
    if retain_committed_spans:
        result["committed_spans"] = dict(page_bytes=page_bytes, span_limit=max_regions,
            rows=spans, address_space_complete=complete)
    return result


PAGE_QUERY_DEFAULT_LIMIT = 262144
PAGE_QUERY_DEFAULT_SECONDS = 0.5
PAGE_FLAGS_HISTOGRAM_LIMIT = 4096
PAGE_SCOPE = ("Non-atomic QueryWorkingSetEx metadata for selected walk-observed committed addresses. "
    "PRIVATE/MAPPED/IMAGE label the earlier VQE walk, not current heap or GPU ownership. "
    "Valid=0 is known invalid-page metadata, not a failed query. Valid-page flags may include "
    "AWE/large pages outside the ordinary working set; their byte sum is not RSS. "
    "No extrapolation, target-content read, page touch, trim, lock or suspension. "
    "One entered OS call cannot be interrupted by the inter-call time budget.")


def validate_page_options(requested, max_pages, max_seconds):
    if type(requested) is not bool:
        raise ValueError("working-set page opt-in must be boolean")
    integer(max_pages, "page query bound", 1, 1048576)
    if (isinstance(max_seconds, bool) or not isinstance(max_seconds, (int, float)) or
            not math.isfinite(max_seconds) or not 0 < max_seconds <= 10):
        raise ValueError("finite page time budget in (0,10] required")


def working_set_page_metadata(walk, query, *, max_pages=PAGE_QUERY_DEFAULT_LIMIT,
                              max_seconds=PAGE_QUERY_DEFAULT_SECONDS,
                              clock=time.monotonic, alive=lambda: True):
    """Stream bounded selected spans through the existing raw page-query port."""
    validate_page_options(True, max_pages, max_seconds)
    result = dict(requested=True, status="UNAVAILABLE", scope=PAGE_SCOPE,
        max_pages=max_pages, batch_page_limit=WORKING_SET_PAGE_LIMIT,
        native_batch_bytes_limit=WORKING_SET_PAGE_LIMIT * 16,
        query_time_budget_s=max_seconds, histogram_row_limit=PAGE_FLAGS_HISTOGRAM_LIMIT,
        selection_policy="ascending walk-observed committed addresses; no extrapolation",
        walk_address_space_complete=False, selected_address_coverage_complete=False,
        page_query_coverage_complete=False, raw_flags_coverage_complete=False,
        metadata_complete=False, observed_committed_pages=None, unselected_observed_pages=None,
        unobserved_address_space_pages=None, query_calls=0, query_windows=[],
        query_start_monotonic_s=None, query_end_monotonic_s=None, time_budget_exceeded=False,
        raw_flags_histogram=[], histogram_unretained_pages=0, observed=None, errors=[])
    totals = None
    by_type, groups, histogram = {}, {}, {}

    def record(fault):
        result["errors"].append(dict(stage=getattr(fault, "stage", "page_response"),
            reason=str(fault), win32_error=getattr(fault, "code", None)))

    def counts():
        return dict(selected_pages=0, attempted_pages=0, observed_pages=0,
            valid_pages=0, invalid_pages=0, shareable_valid_pages=0,
            unshared_valid_pages=0, locked_valid_pages=0, large_valid_pages=0)

    try:
        if not isinstance(walk, dict) or not isinstance(walk.get("committed_spans"), dict):
            raise ObservationError("address_walk", "requested committed spans unavailable")
        meta = walk["committed_spans"]
        page = integer(meta.get("page_bytes"), "page size", 1, 1 << 20)
        if page & (page - 1):
            raise ValueError("power-of-two page size required")
        rows = meta.get("rows")
        span_limit = integer(meta.get("span_limit"), "span bound", 1, 65536)
        if not isinstance(rows, list) or len(rows) > span_limit:
            raise ValueError("bounded committed spans required")
        selected, observed_pages, previous_end = [], 0, 0
        remaining = max_pages
        totals = counts()
        by_type = {name: counts() for name in TYPE_NAMES.values()}
        for span in rows:
            if not isinstance(span, dict) or span.get("type") not in by_type:
                raise ValueError("invalid walk-observed span type")
            base = integer(int(span["base"], 16), "span base")
            end = integer(int(span["end_exclusive"], 16), "span end", 1, UINT64_END)
            allocation = integer(int(span["allocation_base"], 16), "allocation base")
            if base < previous_end or not allocation <= base < end or any(v % page for v in (base, end, allocation)):
                raise ValueError("ordered aligned nonoverlapping committed spans required")
            previous_end = end
            observed_pages += (end - base) // page
            take = min(remaining, (end - base) // page)
            if take:
                kind = span["type"]
                key = (kind, allocation)
                group = groups.setdefault(key, dict(type=kind, allocation_base=hex(allocation), **counts()))
                for row in (totals, by_type[kind], group): row["selected_pages"] += take
                selected.append((base, take, kind, group))
                remaining -= take
        observed = walk.get("observed_prefix")
        if not isinstance(observed, dict) or integer(observed.get("committed_bytes"), "observed commit") != observed_pages * page:
            raise ValueError("retained spans do not cover the observed committed prefix")
        result.update(page_bytes=page, observed_committed_pages=observed_pages,
            unselected_observed_pages=observed_pages - totals["selected_pages"],
            walk_address_space_complete=walk.get("address_space_complete") is True,
            unobserved_address_space_pages=0 if walk.get("address_space_complete") is True else None,
            selected_address_coverage_complete=(walk.get("address_space_complete") is True and
                observed_pages == totals["selected_pages"]))
        if not selected:
            result["status"] = "NOT_EXERCISED" if result["walk_address_space_complete"] else "UNAVAILABLE"
            return result
        begin = previous = clock()

        def stamp():
            nonlocal previous
            now = clock()
            if (isinstance(now, bool) or not isinstance(now, (int, float)) or
                    not math.isfinite(now) or now < previous):
                raise ObservationError("page_clock", "invalid/nonmonotonic page clock")
            previous = now
            return now

        if isinstance(begin, bool) or not isinstance(begin, (int, float)) or not math.isfinite(begin):
            raise ObservationError("page_clock", "finite page clock required")
        result["query_start_monotonic_s"] = begin
        addresses, owners = [], []

        def flush():
            start = stamp()
            if start - begin >= max_seconds:
                result["time_budget_exceeded"] = True
                raise ObservationError("page_budget", "page inter-call time budget exhausted")
            if alive() is not True:
                raise ObservationError("process_exit", "target exited before page query")
            window = dict(begin_monotonic_s=start, end_monotonic_s=None,
                first_address=hex(addresses[0]), last_address=hex(addresses[-1]),
                pages=len(addresses), status="UNKNOWN")
            result["query_windows"].append(window)
            result["query_calls"] += 1
            for kind, group in owners:
                for row in (totals, by_type[kind], group): row["attempted_pages"] += 1
            try:
                flags = query(addresses)
                if not isinstance(flags, (list, tuple)) or len(flags) != len(addresses):
                    raise ValueError("page flag count does not match requested addresses")
                for word in flags: integer(word, "raw page flags")
                for (kind, group), word in zip(owners, flags):
                    valid = bool(word & 1)
                    for row in (totals, by_type[kind], group):
                        row["observed_pages"] += 1
                        row["valid_pages" if valid else "invalid_pages"] += 1
                        if valid:
                            row["shareable_valid_pages" if word & (1 << 15) else "unshared_valid_pages"] += 1
                            row["locked_valid_pages"] += bool(word & (1 << 22))
                            row["large_valid_pages"] += bool(word & (1 << 23))
                    key = (kind, word)
                    if key in histogram or len(histogram) < PAGE_FLAGS_HISTOGRAM_LIMIT:
                        histogram[key] = histogram.get(key, 0) + 1
                    else:
                        result["histogram_unretained_pages"] += 1
                window["status"] = "OBSERVED"
            finally:
                active_fault = sys.exc_info()[0] is not None
                try:
                    window["end_monotonic_s"] = stamp()
                    if window["end_monotonic_s"] - begin >= max_seconds:
                        result["time_budget_exceeded"] = True
                except (ObservationError, ValueError) as fault:
                    if not active_fault: raise
                    record(fault)  # Keep the original API failure too.
            addresses.clear()
            owners.clear()

        for base, take, kind, group in selected:
            for offset in range(take):
                addresses.append(base + offset * page)
                owners.append((kind, group))
                if len(addresses) == WORKING_SET_PAGE_LIMIT: flush()
        if addresses: flush()
    except (ObservationError, ValueError, TypeError, KeyError) as fault:
        record(fault)
    finally:
        if result["query_start_monotonic_s"] is not None:
            try: result["query_end_monotonic_s"] = stamp()
            except (ObservationError, ValueError) as fault: record(fault)
        if totals is not None:
            for row in (totals, *by_type.values(), *groups.values()):
                row["unknown_attempted_pages"] = row["attempted_pages"] - row["observed_pages"]
                row["unattempted_selected_pages"] = row["selected_pages"] - row["attempted_pages"]
                row["valid_page_bytes"] = row["valid_pages"] * page
            result["observed"] = dict(totals=totals, by_walk_type=by_type,
                allocation_groups=sorted(groups.values(), key=lambda row: (row["type"], int(row["allocation_base"], 16))))
        result["raw_flags_histogram"] = [dict(walk_observed_type=kind, flags_hex=f"0x{word:016x}", pages=count)
            for (kind, word), count in sorted(histogram.items())]
    if result["status"] == "NOT_EXERCISED": return result
    result["page_query_coverage_complete"] = bool(totals and totals["selected_pages"] == totals["observed_pages"] and
        totals["selected_pages"] > 0 and not result["errors"])
    result["raw_flags_coverage_complete"] = result["page_query_coverage_complete"] and not result["histogram_unretained_pages"]
    result["metadata_complete"] = (result["selected_address_coverage_complete"] and
        result["raw_flags_coverage_complete"] and not result["time_budget_exceeded"])
    result["status"] = "OBSERVED_NONATOMIC_PAGE_METADATA" if result["metadata_complete"] else (
        "PARTIAL_PAGE_METADATA" if totals and totals["observed_pages"] else "UNAVAILABLE")
    return result


MAPPED_FILE_GROUP_LIMIT = 32
MAPPED_FILE_QUERY_SECONDS = 0.5


def mapped_allocation_names(walk, query, *, clock=time.monotonic, alive=lambda: True):
    """Attach bounded names to complete mapped groups; never alter walk totals."""
    result = dict(requested=True, status="UNAVAILABLE", allocation_group_limit=MAPPED_FILE_GROUP_LIMIT,
        name_buffer_characters=MAPPED_FILE_NAME_CHARS, query_time_budget_s=MAPPED_FILE_QUERY_SECONDS,
        query_calls=0, total_mapped_groups=None, unqueried_group_count=None,
        unqueried_committed_bytes=None, name_coverage_complete=False, rows=[], errors=[],
        scope="Non-atomic mapped-file device-name metadata after a complete address walk, using the same process handle. Not heap/GPU ownership or RSS attribution. One native call may exceed the inter-call time budget.")
    if not isinstance(walk,dict) or not walk.get("address_space_complete") or not isinstance(walk.get("full_totals"),dict):
        result["errors"].append(dict(stage="address_walk",reason="complete certified allocation groups unavailable",win32_error=None))
        return result
    groups=[group for group in walk["full_totals"]["allocation_groups"] if group["type"] == "MEM_MAPPED"]
    groups.sort(key=lambda group:(-group["committed_bytes"],-group["reserved_bytes"],int(group["allocation_base"],16)))
    result["total_mapped_groups"] = len(groups)
    selected=groups[:MAPPED_FILE_GROUP_LIMIT]
    result["unqueried_group_count"] = len(groups)-len(selected)
    result["unqueried_committed_bytes"] = sum(group["committed_bytes"] for group in groups[MAPPED_FILE_GROUP_LIMIT:])
    if not groups:
        result["status"] = "NOT_EXERCISED"
        return result
    begin=previous=clock()
    halted=None
    for group in selected:
        row=dict(group, query_address=group["allocation_base"], status="NOT_QUERIED", name=None, error=None, truncated=False)
        result["rows"].append(row)
        try:
            if halted is not None:
                raise ObservationError("mapped_file_query_budget",halted)
            now=clock()
            if any(isinstance(value,bool) or not isinstance(value,(int,float)) or not math.isfinite(value) for value in (begin,now)) or now < previous:
                halted="invalid/nonmonotonic metadata clock"
                raise ObservationError("mapped_file_clock",halted)
            previous=now
            if now-begin >= MAPPED_FILE_QUERY_SECONDS:
                halted="metadata inter-call time budget exhausted"
                raise ObservationError("mapped_file_query_budget",halted)
            if alive() is not True:
                halted="target exited before mapped filename query"
                raise ObservationError("process_exit",halted)
            result["query_calls"] += 1
            name=query(int(group["allocation_base"],16))
            if not isinstance(name,str) or not name or len(name)>=MAPPED_FILE_NAME_CHARS or "\0" in name or any(ord(c)<32 for c in name):
                raise ValueError("invalid bounded mapped filename response")
            row.update(status="OBSERVED",name=name)
        except (ObservationError,ValueError) as fault:
            row["status"] = "NOT_QUERIED" if halted is not None else "UNAVAILABLE"
            row["truncated"] = getattr(fault,"truncated",False) is True
            row["error"] = dict(stage=getattr(fault,"stage","mapped_file_response"),reason=str(fault),win32_error=getattr(fault,"code",None))
            result["errors"].append(dict(allocation_base=group["allocation_base"],**row["error"]))
    result["unqueried_group_count"] += sum(row["status"]=="NOT_QUERIED" for row in result["rows"])
    result["unqueried_committed_bytes"] += sum(row["committed_bytes"] for row in result["rows"] if row["status"]=="NOT_QUERIED")
    observed=sum(row["status"]=="OBSERVED" for row in result["rows"])
    result["observed_named_groups"] = observed
    result["name_coverage_complete"] = observed==len(groups) and not result["errors"]
    result["status"] = "COMPLETE_MAPPED_NAMES" if result["name_coverage_complete"] else ("PARTIAL_MAPPED_NAMES" if observed else "UNAVAILABLE")
    return result


def anchor(value):
    if not isinstance(value,dict):
        raise ValueError("invalid QPC port")
    return dict(ticks=integer(value.get("ticks"),"QPC ticks"),
                frequency=integer(value.get("frequency"),"QPC frequency",1))


def identity(value, pid, exe, creation=None):
    if not isinstance(value,dict) or value.get("alive") is not True:
        raise ObservationError("identity", "target is not alive")
    if integer(value.get("pid"),"target PID",1,(1<<32)-1) != pid:
        raise ObservationError("identity", "PID witness mismatch")
    actual = value.get("exe")
    if not isinstance(actual,str) or not actual or "\0" in actual or ntpath.normcase(ntpath.normpath(actual)) != ntpath.normcase(ntpath.normpath(exe)):
        raise ObservationError("identity", "EXE path witness mismatch")
    stamp=integer(value.get("creation_filetime_100ns"),"creation FILETIME",1)
    if creation is not None and stamp != creation:
        raise ObservationError("identity", "creation FILETIME witness mismatch")
    return dict(pid=pid,exe=actual,creation_filetime_100ns=stamp,alive=True)


def process_sample(value):
    if not isinstance(value,dict):
        raise ValueError("invalid process sample")
    result={key:integer(value.get(key),key) for key in
            ("rss_bytes","private_commit_bytes","cpu_time_100ns")}
    count=value.get("thread_count")
    result["thread_count"]=None if count is None else integer(count,"thread count")
    return result


def capture(pid, exe, *, creation=None, label="manual", reader_factory=WindowsReader,
            max_regions=16384, max_groups=4096, max_seconds=2.0, mapped_file_names=False,
            working_set_pages=False, max_pages=PAGE_QUERY_DEFAULT_LIMIT,
            max_page_seconds=PAGE_QUERY_DEFAULT_SECONDS):
    if type(mapped_file_names) is not bool:
        raise ValueError("mapped-file opt-in must be boolean")
    validate_page_options(working_set_pages, max_pages, max_page_seconds)
    integer(pid,"PID",1,(1<<32)-1)
    if not isinstance(exe,str) or not ntpath.isabs(exe) or "\0" in exe:
        raise ValueError("absolute expected EXE path required")
    if creation is not None:
        integer(creation,"expected creation FILETIME",1)
    if not isinstance(label,str) or not label or len(label)>64 or any(ord(c)<32 for c in label):
        raise ValueError("bounded observation label required")
    report=dict(schema="jce.windows-commit-regions.v1",status="UNAVAILABLE",scope=SCOPE,
        label=label,expected_pid=pid,expected_exe=exe,expected_creation_filetime_100ns=creation,
        limits=dict(max_regions=max_regions,max_groups=max_groups,max_seconds=max_seconds),
        authority_sha256={name:hashlib.sha256((Path(__file__).parent/name).read_bytes()).hexdigest()
                          for name in ("memory_commit_probe.py","windows_memory_reader.py")},
        caller_creation_witness=creation is not None,identity_before=None,identity_after=None,
        qpc_begin=None,qpc_end=None,os_before=None,os_after=None,walk=None,errors=[],
        mapped_file_metadata=dict(requested=mapped_file_names,status="NOT_REQUESTED" if not mapped_file_names else "UNAVAILABLE"),
        working_set_metadata=dict(requested=working_set_pages,status="NOT_REQUESTED" if not working_set_pages else "UNAVAILABLE"),
        handle_rights="PROCESS_QUERY_INFORMATION | SYNCHRONIZE; no VM_READ/VM_WRITE")
    def record(fault, stage=None):
        report["errors"].append(dict(stage=stage or getattr(fault,"stage","port_response"),
            reason=str(fault),win32_error=getattr(fault,"code",None)))
    try:
        reader_options = {}
        if mapped_file_names: reader_options["mapped_file_names"] = True
        if working_set_pages: reader_options["working_set_pages"] = True
        with reader_factory(pid, **reader_options) as reader:
            try:
                report["qpc_begin"] = anchor(reader.qpc())
                first = identity(reader.identity(),pid,exe,creation)
                report["identity_before"] = first
                bounds = reader.bounds
                if not isinstance(bounds,dict) or bounds.get("pointer_bits") != 64:
                    raise ValueError("native 64-bit address bounds required")
                low=integer(bounds.get("minimum_address"),"minimum address")
                high=integer(bounds.get("maximum_address"),"maximum address",low)
                report["address_bounds"] = bounds
                try:
                    report["os_before"] = process_sample(reader.sample())
                except (ObservationError,ValueError) as fault:
                    record(fault,"sample_before")
                frequency=report["qpc_begin"]["frequency"]
                def clock():
                    stamp=anchor(reader.qpc())
                    if stamp["frequency"] != frequency:
                        raise ObservationError("qpc","frequency changed during observation")
                    return stamp["ticks"] / frequency
                report["walk"] = walk_regions(reader.region,high+1,page_bytes=bounds.get("page_bytes"),
                    max_regions=max_regions,max_groups=max_groups,max_seconds=max_seconds,clock=clock,alive=reader.alive,
                    retain_committed_spans=working_set_pages)
                if working_set_pages:
                    query=getattr(reader,"working_set_page_flags",None)
                    if query is None:
                        def query(_):
                            raise ObservationError("working_set_pages","optional reader page port unavailable")
                    report["working_set_metadata"] = working_set_page_metadata(report["walk"],query,
                        max_pages=max_pages,max_seconds=max_page_seconds,clock=clock,alive=reader.alive)
                if mapped_file_names:
                    query=getattr(reader,"mapped_file_name",None)
                    if query is None:
                        def query(_):
                            raise ObservationError("mapped_file_names","optional reader metadata port unavailable")
                    report["mapped_file_metadata"] = mapped_allocation_names(report["walk"],query,clock=clock,alive=reader.alive)
                report["identity_after"] = identity(reader.identity(),pid,exe,first["creation_filetime_100ns"])
                try:
                    report["os_after"] = process_sample(reader.sample())
                except (ObservationError,ValueError) as fault:
                    record(fault,"sample_after")
            finally:
                # QPC is independent of target liveness. Bracket failed walks too.
                if report["qpc_begin"]:
                    try:
                        report["qpc_end"] = anchor(reader.qpc())
                        if (report["qpc_end"]["frequency"] != report["qpc_begin"]["frequency"] or
                                report["qpc_end"]["ticks"] < report["qpc_begin"]["ticks"]):
                            raise ObservationError("qpc","invalid end anchor")
                    except (ObservationError,ValueError) as fault:
                        record(fault,"qpc_end")
    except (ObservationError,ValueError) as fault:
        record(fault)
    walk=report["walk"]
    identity_verified=report["identity_before"] is not None and report["identity_after"]==report["identity_before"]
    clock_verified=(report["qpc_begin"] is not None and report["qpc_end"] is not None and
        report["qpc_begin"]["frequency"]==report["qpc_end"]["frequency"] and
        report["qpc_end"]["ticks"]>=report["qpc_begin"]["ticks"])
    report["same_target_identity_verified"]=identity_verified
    report["qpc_window_verified"]=clock_verified
    report["observation_window_s"]=(report["qpc_end"]["ticks"]-report["qpc_begin"]["ticks"])/report["qpc_begin"]["frequency"] if clock_verified else None
    if mapped_file_names:
        report["mapped_file_metadata"]["same_target_identity_verified"] = identity_verified
        report["mapped_file_metadata"]["qpc_window_verified"] = clock_verified
        if not identity_verified or not clock_verified:
            report["mapped_file_metadata"].update(status="UNAVAILABLE",name_coverage_complete=False,
                certificate_error="target identity or QPC window did not remain valid; rows are diagnostic only")
    if working_set_pages:
        metadata=report["working_set_metadata"]
        metadata["same_target_identity_verified"] = identity_verified
        metadata["qpc_window_verified"] = clock_verified
        if not identity_verified or not clock_verified:
            metadata.update(status="UNAVAILABLE",metadata_complete=False,
                certificate_error="target identity or QPC window did not remain valid; observations are diagnostic only")
    if walk and (not identity_verified or not clock_verified):
        walk["full_totals"]=None  # Keep the observed prefix, without a target/window certificate.
    if walk and walk["address_space_complete"] and identity_verified and clock_verified and not report["errors"]:
        report["status"]="OBSERVED_NONATOMIC_COMMIT_REGIONS"
    elif walk:
        report["status"]="PARTIAL"
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid",type=int,required=True)
    parser.add_argument("--exe",required=True)
    parser.add_argument("--creation-filetime",type=int)
    parser.add_argument("--label",default="manual")
    parser.add_argument("--max-regions",type=int,default=16384)
    parser.add_argument("--max-seconds",type=float,default=2.0)
    parser.add_argument("--mapped-file-names",action="store_true",help="query at most 32 complete MEM_MAPPED allocation groups; optional device-name metadata only")
    parser.add_argument("--working-set-pages",action="store_true",help="optional bounded page flags over walk-observed committed spans; not an RSS sum")
    parser.add_argument("--page-limit",type=int,default=PAGE_QUERY_DEFAULT_LIMIT)
    parser.add_argument("--page-seconds",type=float,default=PAGE_QUERY_DEFAULT_SECONDS)
    parser.add_argument("--out",type=Path,required=True)
    args=parser.parse_args()
    output=args.out.resolve()
    if output.exists():
        parser.error("fresh output directory required")
    try:
        report=capture(args.pid,args.exe,creation=args.creation_filetime,label=args.label,
                       max_regions=args.max_regions,max_seconds=args.max_seconds,mapped_file_names=args.mapped_file_names,
                       working_set_pages=args.working_set_pages,max_pages=args.page_limit,max_page_seconds=args.page_seconds)
    except ValueError as fault:
        parser.error(str(fault))
    output.mkdir(parents=True)
    (output/"report.json").write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8",newline="\n")
    summary={key:report[key] for key in ("status","expected_pid","label","errors")}
    summary["mapped_file_status"]=report["mapped_file_metadata"]["status"]
    summary["working_set_page_status"]=report["working_set_metadata"]["status"]
    print(json.dumps(summary))
    pages_ok=not args.working_set_pages or report["working_set_metadata"].get("metadata_complete") is True
    return 0 if report["status"] == "OBSERVED_NONATOMIC_COMMIT_REGIONS" and pages_ok else 2


if __name__ == "__main__":
    raise SystemExit(main())
