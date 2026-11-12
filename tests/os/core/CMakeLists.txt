cmake_minimum_required(VERSION 3.20)

jce_add_unit_test(test_jce_sha256 SOURCES test_jce_sha256.c LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
jce_add_unit_test(test_jce_str             SOURCES test_jce_str.c             LINK jce_core)
# Exact path owner with typed temporary-allocation failure ports.
jce_add_unit_test(test_jce_path SOURCES test_jce_path.c LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
jce_add_unit_test(test_jce_alloc           SOURCES test_jce_alloc.c           LINK jce_core)
jce_add_unit_test(test_jce_allocator       SOURCES test_jce_allocator.c       LINK jce_core)
jce_add_unit_test(test_jce_handle          SOURCES test_jce_handle.c          LINK jce_core)
jce_add_unit_test(test_jce_event           SOURCES test_jce_event.c           LINK jce_core)
jce_add_unit_test(test_jce_sysinfo         SOURCES test_jce_sysinfo.c         LINK jce_core)
jce_add_unit_test(test_jce_timer           SOURCES test_jce_timer.c           LINK jce_core)
# Compile the actual clock owner with controlled private Web/SDL readings.
jce_add_unit_test(test_jce_timer_web_provider SOURCES test_jce_timer_web_provider.c
    LINK jce_core sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
jce_add_unit_test(test_jce_fixed_clock     SOURCES test_jce_fixed_clock.c     LINK jce_core)
jce_add_unit_test(test_jce_json            SOURCES test_jce_json.c            LINK jce_core)
# Malicious/edge input for the JSON facade (audit §35.2): escaping, UTF-8,
# nesting depth, truncated documents, explicit-length bounds, defaults.
jce_add_unit_test(test_jce_json_hardening  SOURCES test_jce_json_hardening.c  LINK jce_core)
jce_add_unit_test(test_jce_profiler SOURCES test_jce_profiler.c
    LINK jce_core INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
# Compile the real sampler owner with controlled native observation outcomes.
jce_add_unit_test(test_jce_sampler_ownership SOURCES test_jce_sampler_ownership.c
    LINK jce_core INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
include("${CMAKE_CURRENT_LIST_DIR}/sampler_capacity_controls.cmake")
jce_add_unit_test(test_jce_perf_phase      SOURCES test_jce_perf_phase.c      LINK jce_core)
jce_add_unit_test(test_jce_trace SOURCES test_jce_trace.c LINK jce_core sdl::sdl)
include("${CMAKE_CURRENT_LIST_DIR}/trace_clock_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/trace_json_span_controls.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/trace_json_integer_controls.cmake")
jce_add_unit_test(test_jce_frustum         SOURCES test_jce_frustum.c         LINK jce_core)
jce_add_unit_test(test_jce_thread          SOURCES test_jce_thread.c          LINK jce_core)
include("${CMAKE_CURRENT_LIST_DIR}/thread_retirement_controls.cmake")
jce_add_unit_test(test_jce_async SOURCES test_jce_async.c LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
# jce_jobs was retired onto the enkiTS pool (P1-2).  Its replacement pins the
# invariant the migration nearly broke: parallel_for partitions must start on a
# multiple of `chunk`, because jce_render_queue.c and jce_scene_renderer.c derive
# a single-writer slot index from `begin`.
jce_add_unit_test(test_jce_thread_parallel_for SOURCES test_jce_thread_parallel_for.c LINK jce_core)
jce_add_unit_test(test_jce_filesystem_host SOURCES test_jce_filesystem_host.c LINK jce_core)
jce_add_unit_test(test_jce_filesystem      SOURCES test_jce_filesystem.c      LINK jce_core jce_resource)
jce_add_unit_test(test_jce_log             SOURCES test_jce_log.c             LINK jce_core)
include(log_format_controls.cmake)
# Exercise the exact private bootstrap defaults through the config owner.
jce_add_unit_test(test_jce_config SOURCES test_jce_config.c LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
jce_add_unit_test(test_jce_i18n            SOURCES test_jce_i18n.c            LINK jce_core jce_resource)
jce_add_unit_test(test_jce_console         SOURCES test_jce_console.c         LINK jce_core)

# The console's EDITING layer -- line, history, Tab completion -- which is the
# first reader the registry's introspection surface has ever had.  jce_cvar_at,
# jce_cvar_help and jce_cvar_flags had no caller anywhere; jce_cvar_count and
# jce_cvar_format_value were reached only by the test above.  Renders nothing
# and owns no window, so every case runs headlessly: the RML overlay that will
# sit on top of it cannot be unit-tested at all (jce_ui_create needs a
# JceRenderer for bgfx draws, and no test in the tree creates a JceUIContext).
jce_add_unit_test(test_jce_console_session SOURCES test_jce_console_session.c LINK jce_core)
jce_add_unit_test(test_jce_log_colors      SOURCES test_jce_log_colors.c      LINK jce_core)
# The file sink used to append forever.  Pins the bounded, zstd-compressed
# history: generation count, real zstd frames (not renames), the uncompressed
# fallback, the documented defaults/clamps, and the byte counter being seeded
# from an existing file on an "a" reopen.
jce_add_unit_test(test_jce_log_rotation    SOURCES test_jce_log_rotation.c    LINK jce_core)
jce_add_unit_test(test_jce_camera_shake    SOURCES test_jce_camera_shake.c    LINK jce_core)
jce_add_unit_test(test_jce_easing          SOURCES test_jce_easing.c          LINK jce_core)
jce_add_unit_test(test_jce_rand            SOURCES test_jce_rand.c            LINK jce_core)
jce_add_unit_test(test_jce_platform_services SOURCES test_jce_platform_services.c LINK jce_core)

# Re-entrancy contract of jce_event_publish: a handler that subscribes to NEW
# event ids mid-dispatch used to make find_slot() rehash() and free the slot
# array the publisher was still holding a pointer into (use-after-free).
jce_add_unit_test(test_jce_event_reentrant
    SOURCES test_jce_event_reentrant.c
    LINK    jce_core)

# FNV-1a constants are magic numbers: a single-digit typo is invisible to every
# functional test (JCE_FNV1A64_INIT shipped for a long time as the canonical
# basis with its trailing digit dropped).  Assert the VALUES, in two spellings,
# plus the published known-answer vectors.
jce_add_unit_test(test_jce_hash_fnv_constants
    SOURCES test_jce_hash_fnv_constants.c
    LINK    jce_core)

# ABI layout facts a header parser cannot see (bool width, public enum width,
# consumer-allocated descriptor layout).  Complements
# tools/audit/check_abi_snapshot.py, which freezes DECLARATIONS but states in
# its own docs that it cannot see sizes — only a compiler can.  Links the
# layers whose public enums/structs it pins.
jce_add_unit_test(test_jce_abi_layout
    SOURCES test_jce_abi_layout.c
    LINK    jce_core jce_resource jce_physics)

# The single IEEE binary16 decoder, after three copies (renderer capture,
# net quantiser, post-fx exposure metering) converged on it.  Header-only and
# in jce_math.h, so this links nothing but jce_core.
jce_add_unit_test(test_jce_half_float
    SOURCES test_jce_half_float.c
    LINK    jce_core)

# The assertion facility, and the one thing that makes its test meaningful:
# it runs in the SAME configuration everything else here runs in, and that
# configuration defines NDEBUG.  The file's first case executes a bare
# assert(0) and requires it to be inert, so "JCE_ASSERT fired" is a statement
# about the shipped build rather than about a Debug one.
jce_add_unit_test(test_jce_assert
    SOURCES test_jce_assert.c
    LINK    jce_core)

jce_add_unit_test(test_jce_read_source SOURCES test_jce_read_source.c LINK jce_core)

jce_add_unit_test(test_jce_mem_profile SOURCES test_jce_mem_profile.c LINK jce_core)

# Compile the actual owner with private deterministic allocator seams.
jce_add_unit_test(test_jce_realloc_accounting SOURCES test_jce_realloc_accounting.c
    LINK mimalloc-static INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
include(memory_atomic_controls.cmake)

jce_add_unit_test(test_jce_trace_capture SOURCES test_jce_trace_capture.c
    LINK jce_core sdl::sdl INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
include(diagnostics_retirement_controls.cmake)
include(trace_capture_clock_controls.cmake)
include(trace_reader_controls.cmake)
include(filesystem_pak_bounded_controls.cmake)

jce_add_unit_test(test_jce_filesystem_trace SOURCES test_jce_filesystem_trace.c LINK jce_core sdl::sdl)

jce_add_unit_test(test_jce_filesystem_lock_failure SOURCES test_jce_filesystem_lock_failure.c
    LINK jce_core physfs-static INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

jce_add_unit_test(test_jce_trace_publication SOURCES test_jce_trace_publication.c
    LINK jce_core sdl::sdl INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

# Exact owner boundary contrasts use only defined unsigned arithmetic. The
# fallback target models the 32-bit ID algorithm on this host, not a 32-bit ABI.
file(READ "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_trace.c" _trace_limits_owner)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_trace.c")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/trace-limits")
function(_jce_trace_limits_replace source_variable wire replacement)
    string(FIND "${${source_variable}}" "${wire}" _first)
    string(FIND "${${source_variable}}" "${wire}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Trace boundary contrast requires one actual owner wire")
    endif()
    string(REPLACE "${wire}" "${replacement}" _changed "${${source_variable}}")
    set(${source_variable} "${_changed}" PARENT_SCOPE)
endfunction()
foreach(_trace_limits_variant IN ITEMS id_fallback old_unsigned_wrap old_id32 old_task32 old_suppression_depth
        old_enable_race old_registration_generation)
    set(_trace_limits_selected "${_trace_limits_owner}")
    set(_trace_limits_selector JCE_TEST_TRACE_LIMIT_CONTEXT_ONLY)
    set(_trace_limits_kind contract)
    if(_trace_limits_variant STREQUAL "id_fallback")
        string(REGEX MATCHALL "#if UINTPTR_MAX > UINT32_MAX" _trace_limits_branches "${_trace_limits_selected}")
        list(LENGTH _trace_limits_branches _trace_limits_count)
        if(NOT _trace_limits_count EQUAL 3)
            message(FATAL_ERROR "Trace ID fallback requires all three actual storage/reserve/reset branches")
        endif()
        string(REPLACE "#if UINTPTR_MAX > UINT32_MAX" "#if 0 /* Controlled 32-bit ID algorithm. */"
            _trace_limits_selected "${_trace_limits_selected}")
        set(_trace_limits_selector JCE_TEST_TRACE_LIMIT_ID_ONLY)
        set(_trace_limits_kind unit)
    elseif(_trace_limits_variant STREQUAL "old_unsigned_wrap")
        _jce_trace_limits_replace(_trace_limits_selected
            "        if (current == UINT32_MAX) return false;" "")
        _jce_trace_limits_replace(_trace_limits_selected
            "        const uint32_t next = current + 1u;"
            "        const uint32_t next = current == UINT32_MAX ? 1u : current + 1u;")
        _jce_trace_limits_replace(_trace_limits_selected
            "            if (next == UINT32_MAX) {\n                /* Preserve this invalid state across disable/re-enable. */\n                SDL_SetAtomicInt(&s_enabled, 0);\n                return false;\n            }" "")
        set(_trace_limits_selector JCE_TEST_TRACE_LIMIT_SEQUENCE_ONLY)
    elseif(_trace_limits_variant STREQUAL "old_id32")
        _jce_trace_limits_replace(_trace_limits_selected "    *id = next;" "    *id = (uint32_t)next;")
        set(_trace_limits_selector JCE_TEST_TRACE_LIMIT_ID_ONLY)
    elseif(_trace_limits_variant STREQUAL "old_task32")
        _jce_trace_limits_replace(_trace_limits_selected "    s_thread.task = id;" "    s_thread.task = (uint32_t)id;")
    elseif(_trace_limits_variant STREQUAL "old_enable_race")
        _jce_trace_limits_replace(_trace_limits_selected
            "    if (enabled && !jce_trace_capture_valid_internal())\n        SDL_SetAtomicInt(&s_enabled, 0);" "")
        set(_trace_limits_selector JCE_TEST_TRACE_LIMIT_ENABLE_ONLY)
    elseif(_trace_limits_variant STREQUAL "old_registration_generation")
        _jce_trace_limits_replace(_trace_limits_selected
            "    if (s_thread.active || !jce_trace_enabled())" "    if (!jce_trace_enabled())")
        _jce_trace_limits_replace(_trace_limits_selected
            "    if (s_thread.active && s_thread.registration_epoch == registration_epoch) {"
            "    if (s_thread.active && s_thread.generation == (uint32_t)SDL_GetAtomicInt(&s_generation)) {")
        set(_trace_limits_selector JCE_TEST_TRACE_LIMIT_REGISTRATION_ONLY)
    else()
        _jce_trace_limits_replace(_trace_limits_selected
            "    s_thread.suppression = depth + 1;" "    s_thread.suppression = 1;")
    endif()
    set(_trace_limits_path "${CMAKE_CURRENT_BINARY_DIR}/trace-limits/${_trace_limits_variant}.c")
    jce_test_write_if_changed("${_trace_limits_path}" "${_trace_limits_selected}")
    set(_trace_limits_name "test_jce_trace_publication_${_trace_limits_variant}")
    jce_add_unit_test(${_trace_limits_name} KIND ${_trace_limits_kind}
        SOURCES test_jce_trace_publication.c LINK jce_core sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/core")
    target_compile_definitions(${_trace_limits_name} PRIVATE
        JCE_TEST_TRACE_PUBLICATION_OWNER="${_trace_limits_path}" ${_trace_limits_selector}=1)
    if(_trace_limits_kind STREQUAL "contract")
        set_tests_properties(${_trace_limits_name} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_trace_limits_key IN ITEMS owner variant selected selector kind branches count path name)
    unset(_trace_limits_${_trace_limits_key})
endforeach()
unset(_trace_limits_key)

target_include_directories(test_jce_filesystem_trace PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src")

# Exact host owner with real disk/SDL operations and bounded failure ports.
# Caller-owned OPEN contexts also cover host-service borrow preservation across
# active VFS/reader/probe changes, without a second production singleton.
jce_add_unit_test(test_jce_filesystem_host_atomic
    SOURCES test_jce_filesystem_host_atomic.c
    LINK jce_core sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

# Same complete host fixture, with only the included owner's size_t/SIZE_MAX
# length arithmetic modeled as 32-bit. SDL, file handles and ABI remain native.
jce_add_unit_test(test_jce_filesystem_host_atomic_size32
    SOURCES test_jce_filesystem_host_atomic.c
    LINK jce_core sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
target_compile_definitions(test_jce_filesystem_host_atomic_size32 PRIVATE
    JCE_TEST_FS_HOST_SIZE32=1
    JCE_TEST_FS_HOST_FILE_PREFIX="_ut_fs_atomic_size32")

# Replace only the actual capped-read narrowing/admission wire with its old
# body. Huge/wrapped allocations are refused by counted fixture ports, so the
# same size32 controls fail ordinary assertions instead of corrupting memory.
file(READ "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_filesystem_host.c" _host_capped_owner)
set(_host_capped_safe_wire [=[uint64_t read_bytes = (uint64_t)sz;
    if (read_bytes > max_bytes) read_bytes = max_bytes;
    if (detail) {
        detail->total = (uint64_t)sz;
        detail->requested = read_bytes;
        detail->known |= JCE_FS_HOST_READ_KNOWN_TOTAL | JCE_FS_HOST_READ_KNOWN_REQUESTED;
    }
    if (read_bytes > (uint64_t)(SIZE_MAX - 1u)) {
        s_read_trace_fail(detail, JCE_FS_HOST_READ_LIMIT);
        s_read_trace_close(io, detail);
        return NULL;
    }
    size_t read_size = (size_t)read_bytes;]=])
set(_host_capped_old_body [=[size_t read_size = (size_t)sz;
    if ((uint64_t)read_size > max_bytes) read_size = (size_t)max_bytes;
    if (detail) {
        detail->total = (uint64_t)sz;
        detail->requested = (uint64_t)read_size;
        detail->known |= JCE_FS_HOST_READ_KNOWN_TOTAL | JCE_FS_HOST_READ_KNOWN_REQUESTED;
    }]=])
string(REGEX MATCHALL "uint64_t read_bytes = [(]uint64_t[)]sz"
    _host_capped_marks "${_host_capped_owner}")
list(LENGTH _host_capped_marks _host_capped_mark_count)
if(NOT _host_capped_mark_count EQUAL 1)
    message(FATAL_ERROR "Host capped-read control requires one actual length admission wire")
endif()
string(REPLACE "${_host_capped_safe_wire}"
    "${_host_capped_old_body}"
    _host_capped_old_wire "${_host_capped_owner}")
if(_host_capped_old_wire STREQUAL _host_capped_owner)
    message(FATAL_ERROR "Host capped-read control did not restore the old narrowing wire")
endif()
set(_host_capped_old_path
    "${CMAKE_CURRENT_BINARY_DIR}/host-capped-mutation/jce_filesystem_host_old_size.c")
unset(_host_capped_previous)
if(EXISTS "${_host_capped_old_path}")
    file(READ "${_host_capped_old_path}" _host_capped_previous)
endif()
if(NOT _host_capped_previous STREQUAL _host_capped_old_wire)
    file(WRITE "${_host_capped_old_path}" "${_host_capped_old_wire}")
endif()
jce_add_unit_test(test_jce_filesystem_host_atomic_old_size32
    KIND contract
    SOURCES test_jce_filesystem_host_atomic.c
    LINK jce_core sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/core")
target_compile_definitions(test_jce_filesystem_host_atomic_old_size32 PRIVATE
    JCE_TEST_FS_HOST_SIZE32=1
    JCE_TEST_FS_HOST_OWNER="${_host_capped_old_path}"
    JCE_TEST_FS_HOST_FILE_PREFIX="_ut_fs_atomic_old_size32")
set_tests_properties(test_jce_filesystem_host_atomic_old_size32 PROPERTIES WILL_FAIL TRUE)
unset(_host_capped_owner)
unset(_host_capped_safe_wire)
unset(_host_capped_old_body)
unset(_host_capped_marks)
unset(_host_capped_mark_count)
unset(_host_capped_old_wire)
unset(_host_capped_old_path)
unset(_host_capped_previous)

# Independently remove only the whole-file read admission. The capped-read
# mutant above retains this wire; this mutant retains the capped-read wire.
file(READ "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_filesystem_host.c" _host_read_all_owner)
set(_host_read_all_safe_wire [=[if ((uint64_t)sz > (uint64_t)SIZE_MAX - 1u) {
        s_read_trace_fail(detail, JCE_FS_HOST_READ_LIMIT);
        s_read_trace_close(io, detail);
        return NULL;
    }]=])
string(REGEX MATCHALL "if [(][(]uint64_t[)]sz > [(]uint64_t[)]SIZE_MAX - 1u[)]"
    _host_read_all_marks "${_host_read_all_owner}")
list(LENGTH _host_read_all_marks _host_read_all_mark_count)
if(NOT _host_read_all_mark_count EQUAL 1)
    message(FATAL_ERROR "Host read-all control requires one actual length admission wire")
endif()
string(REPLACE "${_host_read_all_safe_wire}"
    "/* Previous unchecked read-all narrowing and sentinel allocation. */"
    _host_read_all_old_wire "${_host_read_all_owner}")
if(_host_read_all_old_wire STREQUAL _host_read_all_owner)
    message(FATAL_ERROR "Host read-all control did not remove its actual admission wire")
endif()
set(_host_read_all_old_path
    "${CMAKE_CURRENT_BINARY_DIR}/host-read-all-mutation/jce_filesystem_host_old_read_all.c")
unset(_host_read_all_previous)
if(EXISTS "${_host_read_all_old_path}")
    file(READ "${_host_read_all_old_path}" _host_read_all_previous)
endif()
if(NOT _host_read_all_previous STREQUAL _host_read_all_old_wire)
    file(WRITE "${_host_read_all_old_path}" "${_host_read_all_old_wire}")
endif()
jce_add_unit_test(test_jce_filesystem_host_atomic_old_read_all_size32
    KIND contract
    SOURCES test_jce_filesystem_host_atomic.c
    LINK jce_core sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/core")
target_compile_definitions(test_jce_filesystem_host_atomic_old_read_all_size32 PRIVATE
    JCE_TEST_FS_HOST_SIZE32=1
    JCE_TEST_FS_HOST_OWNER="${_host_read_all_old_path}"
    JCE_TEST_FS_HOST_FILE_PREFIX="_ut_fs_atomic_old_read_all_size32")
set_tests_properties(test_jce_filesystem_host_atomic_old_read_all_size32 PROPERTIES WILL_FAIL TRUE)
unset(_host_read_all_owner)
unset(_host_read_all_safe_wire)
unset(_host_read_all_marks)
unset(_host_read_all_mark_count)
unset(_host_read_all_old_wire)
unset(_host_read_all_old_path)
unset(_host_read_all_previous)

# Actual host read observations: each mutant is the whole owner with only
# one real detail wire removed. All negatives retain the same IO assertions;
# compile/link errors, crash and timeout are not the intended assertion failure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_filesystem_host.c")
file(READ "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_filesystem_host.c" _host_read_detail_owner)
function(jce_add_host_read_detail_negative name wire replacement expected_count)
    string(FIND "${_host_read_detail_owner}" "${wire}" _first)
    string(FIND "${_host_read_detail_owner}" "${wire}" _last REVERSE)
    if(_first EQUAL -1)
        message(FATAL_ERROR "${name}: actual host read detail wire is missing")
    endif()
    if(expected_count EQUAL 1 AND NOT _first EQUAL _last)
        message(FATAL_ERROR "${name}: expected one actual host read detail wire")
    endif()
    string(REPLACE "${wire}" "" _without "${_host_read_detail_owner}")
    string(LENGTH "${_host_read_detail_owner}" _before)
    string(LENGTH "${_without}" _after)
    string(LENGTH "${wire}" _wire_size)
    math(EXPR _count "(${_before}-${_after})/${_wire_size}")
    if(NOT _count EQUAL expected_count)
        message(FATAL_ERROR "${name}: expected ${expected_count} actual host read detail wires, got ${_count}")
    endif()
    string(REPLACE "${wire}" "${replacement}" _mutant "${_host_read_detail_owner}")
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/host-read-detail-mutation/${name}.c")
    unset(_previous)
    if(EXISTS "${_path}")
        file(READ "${_path}" _previous)
    endif()
    if(NOT _previous STREQUAL _mutant)
        file(WRITE "${_path}" "${_mutant}")
    endif()
    jce_add_unit_test(${name} KIND contract
        SOURCES test_jce_filesystem_host_atomic.c LINK jce_core sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/core")
    target_compile_definitions(${name} PRIVATE
        JCE_TEST_FS_HOST_OWNER="${_path}" JCE_TEST_FS_HOST_FILE_PREFIX="_ut_${name}")
    set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
endfunction()
jce_add_host_read_detail_negative(test_jce_filesystem_host_read_missing_detail
    "s_read_trace_emit(detail, scope);" "(void)s_read_trace_emit; /* Previous service name/result only. */" 2)
jce_add_host_read_detail_negative(test_jce_filesystem_host_read_missing_stage
    "detail->failure_stage = (uint32_t)stage;" "(void)stage;" 1)
jce_add_host_read_detail_negative(test_jce_filesystem_host_read_unguarded_detail
    "if (!scope.id) return NULL;" "/* Previous unguarded detail computation. */" 1)
# Same complete FS + linked real logger: deliberate late snapshot and false
# full-text label must fail ordinary assertions, not compilation or a crash.
jce_add_host_read_detail_negative(test_jce_filesystem_host_open_delayed_snapshot
    "    s_open_detail_copy(&error, SDL_GetError());\n    s_open_detail_copy(&request, path);\n    const uint32_t generation = jce_trace_capture_generation();"
    "    const uint32_t generation = jce_trace_capture_generation();\n    s_open_detail_copy(&error, SDL_GetError());\n    s_open_detail_copy(&request, path);" 1)
jce_add_host_read_detail_negative(test_jce_filesystem_host_open_unmarked_prefix
    "(unsigned)(text->length != text->captured)" "0u" 1)
jce_add_unit_test(test_jce_filesystem_host_open_log_none
    SOURCES test_jce_filesystem_host_atomic.c LINK jce_core sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
target_compile_definitions(test_jce_filesystem_host_open_log_none PRIVATE
    JCE_LOG_NONE=1 JCE_TEST_FS_HOST_FILE_PREFIX="_ut_fs_open_log_none")
jce_add_host_read_detail_negative(test_jce_filesystem_host_copy_missing_created
    "target = jce__fs_host_stream_new(dst, out_created);"
    "bool ignored_created = false; target = jce__fs_host_stream_new(dst, &ignored_created);" 1)
jce_add_host_read_detail_negative(test_jce_filesystem_host_copy_ignored_close
    "if (!destination_closed) {" "if (false && !destination_closed) {" 1)
jce_add_host_read_detail_negative(test_jce_filesystem_host_copy_overwrite
    "target = jce__fs_host_stream_new(dst, out_created);"
    "target = SDL_IOFromFile(dst, \"wb\"); if (target) *out_created = true;" 1)
unset(_host_read_detail_owner)

# Exact discovery owner, real host markers, counted optional process/env ports.
# Plausible discovery is not a compiler/SDK/ABI execution certificate.
jce_add_unit_test(test_jce_toolchain_override
    SOURCES test_jce_toolchain_override.c
    LINK jce_core sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

# Restore only the actual discovery owner's inherited-stderr wire. Missing
# windowed-parent handles and pipe draining must fail ordinary assertions;
# compilation or a crash cannot count as the intended negative control.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_toolchain.c")
file(READ "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_toolchain.c" _toolchain_query_owner)
string(REGEX MATCHALL "cfg[.]capture_stderr   = true"
    _toolchain_stderr_marks "${_toolchain_query_owner}")
list(LENGTH _toolchain_stderr_marks _toolchain_stderr_count)
if(NOT _toolchain_stderr_count EQUAL 1)
    message(FATAL_ERROR "Toolchain stderr control requires one actual capture wire")
endif()
string(REPLACE "cfg.capture_stderr   = true;" "cfg.capture_stderr   = false;"
    _toolchain_old_stderr "${_toolchain_query_owner}")
if(_toolchain_old_stderr STREQUAL _toolchain_query_owner)
    message(FATAL_ERROR "Toolchain stderr control did not restore its actual old wire")
endif()
set(_toolchain_old_stderr_path
    "${CMAKE_CURRENT_BINARY_DIR}/toolchain-mutation/jce_toolchain_old_stderr.c")
unset(_toolchain_previous)
if(EXISTS "${_toolchain_old_stderr_path}")
    file(READ "${_toolchain_old_stderr_path}" _toolchain_previous)
endif()
if(NOT _toolchain_previous STREQUAL _toolchain_old_stderr)
    file(WRITE "${_toolchain_old_stderr_path}" "${_toolchain_old_stderr}")
endif()
jce_add_unit_test(test_jce_toolchain_override_old_stderr
    KIND contract
    SOURCES test_jce_toolchain_override.c
    LINK jce_core sdl::sdl
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
target_compile_definitions(test_jce_toolchain_override_old_stderr PRIVATE
    JCE_TEST_TOOLCHAIN_OWNER="${_toolchain_old_stderr_path}"
    JCE_TEST_TOOLCHAIN_FILE_PREFIX="_ut_toolchain_override_old_stderr")
set_tests_properties(test_jce_toolchain_override_old_stderr PROPERTIES WILL_FAIL TRUE)
unset(_toolchain_query_owner)
unset(_toolchain_stderr_marks)
unset(_toolchain_stderr_count)
unset(_toolchain_old_stderr)
unset(_toolchain_old_stderr_path)
unset(_toolchain_previous)

# Exact shared default worker policy plus real config/enki owner observations.
jce_add_unit_test(test_jce_worker_budget
    KIND integration
    SOURCES test_jce_worker_budget.c
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")

# Only the real config publisher loses its LOW default branch. A counted
# 32-core query makes the contrast observable even on a one-core test host;
# real enki workers and join/teardown remain unchanged.
if(NOT EMSCRIPTEN)
    file(READ "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_config.c" _worker_config_owner)
    string(REGEX MATCHALL "machine_class == JCE_MACHINE_CLASS_LOW"
        _worker_config_wires "${_worker_config_owner}")
    list(LENGTH _worker_config_wires _worker_config_wire_count)
    if(NOT _worker_config_wire_count EQUAL 1)
        message(FATAL_ERROR "Worker budget control requires one actual LOW publisher wire")
    endif()
    string(REPLACE "machine_class == JCE_MACHINE_CLASS_LOW" "false"
        _worker_config_mutant "${_worker_config_owner}")
    if(_worker_config_mutant STREQUAL _worker_config_owner)
        message(FATAL_ERROR "Worker budget control did not remove the LOW publisher wire")
    endif()
    set(_worker_config_mutant_path "${CMAKE_CURRENT_BINARY_DIR}/jce_config_old_low_budget.c")
    jce_test_write_if_changed("${_worker_config_mutant_path}" "${_worker_config_mutant}")
    jce_add_unit_test(test_jce_worker_budget_old_low_publish
        KIND contract
        SOURCES test_jce_worker_budget.c "${_worker_config_mutant_path}"
        LINK jce_core sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
    target_compile_definitions(test_jce_worker_budget_old_low_publish PRIVATE
        JCE_TEST_WORKER_CONFIG_CORES=32
        SDL_GetNumLogicalCPUCores=jce_test_worker_logical_cores)
    set_tests_properties(test_jce_worker_budget_old_low_publish PROPERTIES WILL_FAIL TRUE)
    unset(_worker_config_owner)
    unset(_worker_config_wires)
    unset(_worker_config_wire_count)
    unset(_worker_config_mutant)
    unset(_worker_config_mutant_path)
endif()

# Actual trace capture TU runtime contrasts, each isolated after owned cleanup.
# Compilation failure cannot count as the expected ordinary assertion failure.
file(READ "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_trace_capture.c" _persistent_capture_owner)
string(REGEX MATCHALL "jce__fs_host_stream_write[(]capture->stream, capture->staging.data," _persistent_capture_marks "${_persistent_capture_owner}")
list(LENGTH _persistent_capture_marks _persistent_capture_mark_count)
if(NOT _persistent_capture_mark_count EQUAL 1)
    message(FATAL_ERROR "Persistent capture old reopen requires its unique actual write wire")
endif()
string(REPLACE "jce__fs_host_stream_write(capture->stream, capture->staging.data,"
    "jce_fs_host_append(capture->path, capture->staging.data,"
    _persistent_capture_old_reopen "${_persistent_capture_owner}")
string(REGEX MATCHALL "bool closed = jce__fs_host_stream_close[(]&capture->stream[)]" _persistent_close_marks "${_persistent_capture_owner}")
list(LENGTH _persistent_close_marks _persistent_close_mark_count)
# Startup failure unwind has its own bool; mutate only the stop-time declaration.
string(FIND "${_persistent_capture_owner}" "uint64_t close_started = jce_time_ticks_ns();" _persistent_close_stop)
if(NOT _persistent_close_mark_count EQUAL 2 OR _persistent_close_stop LESS 0)
    message(FATAL_ERROR "Persistent capture close contrast requires startup and final close owners")
endif()
string(REPLACE "uint64_t close_started = jce_time_ticks_ns();\n    bool closed = jce__fs_host_stream_close(&capture->stream);"
    "uint64_t close_started = jce_time_ticks_ns();\n    bool closed = true; (void)jce__fs_host_stream_close(&capture->stream);"
    _persistent_capture_old_close "${_persistent_capture_owner}")
foreach(_persistent_variant IN ITEMS old_reopen old_close)
    set(_persistent_content "${_persistent_capture_${_persistent_variant}}")
    if(_persistent_content STREQUAL _persistent_capture_owner)
        message(FATAL_ERROR "Persistent capture ${_persistent_variant} did not change actual owner")
    endif()
    set(_persistent_path "${CMAKE_CURRENT_BINARY_DIR}/trace-capture-mutation/jce_trace_capture_${_persistent_variant}.c")
    unset(_persistent_previous)
    if(EXISTS "${_persistent_path}")
        file(READ "${_persistent_path}" _persistent_previous)
    endif()
    if(NOT _persistent_previous STREQUAL _persistent_content)
        file(WRITE "${_persistent_path}" "${_persistent_content}")
    endif()
    set(_persistent_target "test_jce_trace_capture_${_persistent_variant}")
    jce_add_unit_test(${_persistent_target} KIND contract
        SOURCES test_jce_trace_capture.c
        LINK jce_core sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/core")
    target_compile_definitions(${_persistent_target} PRIVATE
        JCE_TEST_TRACE_CAPTURE_OWNER="${_persistent_path}"
        # Parallel normal/mutant processes own different marker/partial names.
        JCE_TEST_TRACE_CAPTURE_FILE_PREFIX="${_persistent_target}")
    if(_persistent_variant STREQUAL "old_reopen")
        target_compile_definitions(${_persistent_target} PRIVATE JCE_TEST_TRACE_CAPTURE_PERSISTENT_ONLY=1)
    else()
        target_compile_definitions(${_persistent_target} PRIVATE JCE_TEST_TRACE_CAPTURE_CLOSE_ONLY=1)
    endif()
    set_tests_properties(${_persistent_target} PROPERTIES WILL_FAIL TRUE)
endforeach()

# Full SDL process owner: real property bags, typed counted OS/process ports.
# The public GUI control below is separate real OS/SDK-consumer evidence.
if(WIN32)
    set(_process_stdio_policy "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_process_stdio_internal.h")
    set(_process_stdio_owner "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_process.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_process_stdio_policy}" "${_process_stdio_owner}")
    file(READ "${_process_stdio_policy}" _process_stdio_policy_source)
    string(REGEX MATCHALL "result[.]mode=SDL_PROCESS_STDIO_NULL" _process_stdio_marks "${_process_stdio_policy_source}")
    list(LENGTH _process_stdio_marks _process_stdio_count)
    if(NOT _process_stdio_count EQUAL 2)
        message(FATAL_ERROR "Process absent/stale negative requires both actual NULL policy assignments")
    endif()
    string(REPLACE "result.mode=SDL_PROCESS_STDIO_NULL;" "result.mode=SDL_PROCESS_STDIO_INHERITED;"
        _process_stdio_old_inheritance "${_process_stdio_policy_source}")
    file(READ "${_process_stdio_owner}" _process_stdio_owner_source)
    set(_process_stdio_error_copy "if (!raw) SDL_strlcpy(err,SDL_GetError(),sizeof err);")
    string(FIND "${_process_stdio_owner_source}" "${_process_stdio_error_copy}" _process_stdio_first)
    string(FIND "${_process_stdio_owner_source}" "${_process_stdio_error_copy}" _process_stdio_last REVERSE)
    if(_process_stdio_first LESS 0 OR NOT _process_stdio_first EQUAL _process_stdio_last)
        message(FATAL_ERROR "Process error-copy negative requires one actual pre-cleanup copy")
    endif()
    string(REPLACE "${_process_stdio_error_copy}" "if (!raw) (void)SDL_GetError();"
        _process_stdio_missing_error_copy "${_process_stdio_owner_source}")
    set(_process_stdio_dir "${CMAKE_CURRENT_BINARY_DIR}/process-stdio")
    file(MAKE_DIRECTORY "${_process_stdio_dir}")
    foreach(_process_stdio_variant IN ITEMS old_inheritance missing_error_copy)
        set(_process_stdio_path "${_process_stdio_dir}/${_process_stdio_variant}.inc.h")
        set(_process_stdio_content "${_process_stdio_${_process_stdio_variant}}")
        unset(_process_stdio_previous)
        if(EXISTS "${_process_stdio_path}")
            file(READ "${_process_stdio_path}" _process_stdio_previous)
        endif()
        if(NOT _process_stdio_previous STREQUAL _process_stdio_content)
            file(WRITE "${_process_stdio_path}" "${_process_stdio_content}")
        endif()
    endforeach()
    foreach(_process_stdio_target IN ITEMS test_jce_process_stdio test_jce_process_stdio_old_inheritance
        test_jce_process_stdio_missing_error_copy)
        set(_process_stdio_kind unit)
        if(NOT _process_stdio_target STREQUAL "test_jce_process_stdio")
            set(_process_stdio_kind contract)
        endif()
        jce_add_unit_test(${_process_stdio_target} KIND ${_process_stdio_kind}
            SOURCES test_jce_process_stdio.c LINK jce_core sdl::sdl
            INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/core")
        target_compile_definitions(${_process_stdio_target} PRIVATE JCE_DIST=1)
        if(MSVC)
            target_compile_options(${_process_stdio_target} PRIVATE /Zc:preprocessor)
        endif()
    endforeach()
    target_compile_definitions(test_jce_process_stdio_old_inheritance PRIVATE
        JCE_TEST_PROCESS_STDIO_POLICY="${_process_stdio_dir}/old_inheritance.inc.h")
    target_compile_definitions(test_jce_process_stdio_missing_error_copy PRIVATE
        JCE_TEST_PROCESS_OWNER="${_process_stdio_dir}/missing_error_copy.inc.h")
    set_tests_properties(test_jce_process_stdio_old_inheritance
        test_jce_process_stdio_missing_error_copy PROPERTIES WILL_FAIL TRUE)
    jce_add_unit_test(test_jce_process_gui_stdio KIND integration
        SOURCES test_jce_process_gui_stdio.c LINK jce_core)
    set_target_properties(test_jce_process_gui_stdio PROPERTIES WIN32_EXECUTABLE TRUE)
    set_tests_properties(test_jce_process_gui_stdio PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
else()
    jce_test_suite_skipped("process-windows-absent-stdio"
        "Windows absent HANDLE and GUI/public-SDK self-child controls require a Windows compiler; POSIX inheritance is unchanged")
endif()

# Complete public VFS whole-read owner and its existing bounded reader.
# Typed PhysFS/provider ports model lengths and failures without OS files or
# huge allocation. SIZE32 changes only owner arithmetic/allocator-port width.
set(_vfs_read_all_bounds_owner "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_filesystem.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_vfs_read_all_bounds_owner}")
file(READ "${_vfs_read_all_bounds_owner}" _vfs_read_all_bounds_source)
set(_vfs_read_all_bounds_current [=[static void *jce_fs_read_all_impl(const JceFileSystem *fs, const char *virtual_path,
                      uint64_t *out_size)
{
    if (out_size) *out_size = 0;
    uint64_t observed = 0;
    /* One bounded whole-asset owner: reject lengths without representable
     * sentinel headroom before PAK eager allocation or PhysFS allocation.
     * Preserve public failure out_size=0; the internal reader independently
     * retains an observed total on rejection. Returned PAK data is binary,
     * without a general NUL-termination promise. */
    void *result = jce_fs_read_all_bounded_internal(fs, virtual_path,
                                                   (uint64_t)SIZE_MAX - 1u, &observed);
    if (result && out_size) *out_size = observed;
    return result;
}
]=])
set(_vfs_read_all_bounds_previous [=[static void *jce_fs_read_all_impl(const JceFileSystem *fs, const char *virtual_path,
                      uint64_t *out_size)
{
    if (out_size) *out_size = 0;
    JceFile *f = jce_fs_open(fs, virtual_path);
    if (!f) return NULL;

    uint64_t sz = jce_fs_size(f);
    if (sz == 0) {
        jce_fs_close(f);
        return NULL;
    }

    void *buf = JCE_MALLOC(sz);
    if (!buf) { jce_fs_close(f); return NULL; }

    uint64_t nread = jce_fs_read(f, buf, sz);
    jce_fs_close(f);

    if (nread != sz) {
        JCE_FREE(buf);
        return NULL;
    }

    if (out_size) *out_size = nread;
    return buf;
}
]=])
string(FIND "${_vfs_read_all_bounds_source}" "${_vfs_read_all_bounds_current}" _vfs_read_all_bounds_first)
string(FIND "${_vfs_read_all_bounds_source}" "${_vfs_read_all_bounds_current}" _vfs_read_all_bounds_last REVERSE)
if(_vfs_read_all_bounds_first LESS 0 OR NOT _vfs_read_all_bounds_first EQUAL _vfs_read_all_bounds_last)
    message(FATAL_ERROR "VFS whole-read controls require one complete actual bounded-consumption body")
endif()
set(_vfs_read_all_bounds_zero_wire "    if (out_size) *out_size = 0;")
string(REPLACE "${_vfs_read_all_bounds_zero_wire}" "    /* historical missing public failure output reset */"
    _vfs_read_all_bounds_missing_zero "${_vfs_read_all_bounds_current}")
set(_vfs_read_all_bounds_dir "${CMAKE_CURRENT_BINARY_DIR}/vfs-read-all-bounds")
file(MAKE_DIRECTORY "${_vfs_read_all_bounds_dir}")
foreach(_vfs_read_all_bounds_variant IN ITEMS current size32 old_read_all_size32 missing_output_zero)
    set(_vfs_read_all_bounds_content "${_vfs_read_all_bounds_source}")
    set(_vfs_read_all_bounds_kind unit)
    set(_vfs_read_all_bounds_target test_jce_filesystem_read_all_bounds)
    if(NOT _vfs_read_all_bounds_variant STREQUAL "current")
        string(APPEND _vfs_read_all_bounds_target "_${_vfs_read_all_bounds_variant}")
    endif()
    if(_vfs_read_all_bounds_variant STREQUAL "old_read_all_size32")
        set(_vfs_read_all_bounds_kind contract)
        string(REPLACE "${_vfs_read_all_bounds_current}" "${_vfs_read_all_bounds_previous}"
            _vfs_read_all_bounds_content "${_vfs_read_all_bounds_source}")
    elseif(_vfs_read_all_bounds_variant STREQUAL "missing_output_zero")
        set(_vfs_read_all_bounds_kind contract)
        string(REPLACE "${_vfs_read_all_bounds_current}" "${_vfs_read_all_bounds_missing_zero}"
            _vfs_read_all_bounds_content "${_vfs_read_all_bounds_source}")
    endif()
    set(_vfs_read_all_bounds_path "${_vfs_read_all_bounds_dir}/${_vfs_read_all_bounds_variant}.c")
    unset(_vfs_read_all_bounds_existing)
    if(EXISTS "${_vfs_read_all_bounds_path}")
        file(READ "${_vfs_read_all_bounds_path}" _vfs_read_all_bounds_existing)
    endif()
    if(NOT _vfs_read_all_bounds_existing STREQUAL _vfs_read_all_bounds_content)
        file(WRITE "${_vfs_read_all_bounds_path}" "${_vfs_read_all_bounds_content}")
    endif()
    jce_add_unit_test(${_vfs_read_all_bounds_target} KIND ${_vfs_read_all_bounds_kind}
        SOURCES test_jce_filesystem_read_all_bounds.c LINK jce_core physfs-static
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/core")
    target_compile_definitions(${_vfs_read_all_bounds_target} PRIVATE JCE_BUILDING_ENGINE=1
        JCE_TEST_FS_READ_ALL_OWNER="${_vfs_read_all_bounds_path}")
    if(_vfs_read_all_bounds_variant STREQUAL "size32" OR
       _vfs_read_all_bounds_variant STREQUAL "old_read_all_size32")
        target_compile_definitions(${_vfs_read_all_bounds_target} PRIVATE JCE_TEST_FS_READ_ALL_SIZE32=1)
    endif()
    if(MSVC)
        target_compile_options(${_vfs_read_all_bounds_target} PRIVATE /Zc:preprocessor)
    endif()
    if(_vfs_read_all_bounds_kind STREQUAL "contract")
        set_tests_properties(${_vfs_read_all_bounds_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_vfs_read_all_bounds_var IN ITEMS owner source current previous first last zero_wire missing_zero
    dir variant content kind target path existing)
    unset(_vfs_read_all_bounds_${_vfs_read_all_bounds_var})
endforeach()
unset(_vfs_read_all_bounds_var)

# Threaded capture contrasts compile the complete production consumer with the
# unchanged real ring/encoder fixture; deterministic clocks/worker ports model
# admission policy, not native scheduling or filesystem throughput.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_trace_capture.c")
file(READ "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_trace_capture.c" _trace_coalescing_owner)
function(jce_add_trace_coalescing_negative name wire replacement selector)
    string(FIND "${_trace_coalescing_owner}" "${wire}" _first)
    string(FIND "${_trace_coalescing_owner}" "${wire}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "${name} requires one actual capture admission wire")
    endif()
    string(REPLACE "${wire}" "${replacement}" _mutant "${_trace_coalescing_owner}")
    set(_path "${CMAKE_CURRENT_BINARY_DIR}/trace-capture-mutation/${name}.c")
    unset(_previous)
    if(EXISTS "${_path}")
        file(READ "${_path}" _previous)
    endif()
    if(NOT _previous STREQUAL _mutant)
        file(WRITE "${_path}" "${_mutant}")
    endif()
    jce_add_unit_test(${name} KIND contract SOURCES test_jce_trace_capture.c
        LINK jce_core sdl::sdl INCLUDES "${CMAKE_SOURCE_DIR}/engine/src"
        "${CMAKE_SOURCE_DIR}/engine/src/os/core")
    target_compile_definitions(${name} PRIVATE JCE_TEST_TRACE_CAPTURE_OWNER="${_path}"
        JCE_TEST_TRACE_CAPTURE_FILE_PREFIX="${name}" ${selector}=1)
    set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
endfunction()
jce_add_trace_coalescing_negative(test_jce_trace_capture_old_short_flush
    "if (force_flush || capture->mode == JCE_TRACE_CAPTURE_COOPERATIVE ||\n        capture_flush_due(capture))"
    "if (true) /* Previous short/empty read flush on every poll. */"
    JCE_TEST_TRACE_CAPTURE_SHORT_ONLY)
jce_add_trace_coalescing_negative(test_jce_trace_capture_missing_deadline
    "return now < capture->staging_started_ns ||\n        now - capture->staging_started_ns >= CAPTURE_FLUSH_DELAY_NS;"
    "return false; /* Missing elapsed deadline admission. */"
    JCE_TEST_TRACE_CAPTURE_DEADLINE_ONLY)
jce_add_trace_coalescing_negative(test_jce_trace_capture_missing_stop_flush
    "if (capture->write_ok && capture->generation_stable) capture_drain(capture, true);"
    "if (capture->write_ok && capture->generation_stable) capture_drain(capture, false);"
    JCE_TEST_TRACE_CAPTURE_STOP_ONLY)
jce_add_trace_coalescing_negative(test_jce_trace_capture_missing_diagnostics
    "bool counted = capture_add(&capture->append_size_buckets[capture_size_bucket(capture->staging.size)], 1) &&\n        capture_add(&capture->append_wall_buckets[capture_wall_bucket(elapsed)], 1);"
    "bool counted = true; /* Missing actual write distribution observation. */"
    JCE_TEST_TRACE_CAPTURE_DIAGNOSTICS_ONLY)
jce_add_trace_coalescing_negative(test_jce_trace_capture_old_small_budget
    "#define CAPTURE_DRAIN_BATCHES 128u"
    "#define CAPTURE_DRAIN_BATCHES 32u"
    JCE_TEST_TRACE_CAPTURE_FULL_TAIL_ONLY)
unset(_trace_coalescing_owner)

# Complete FS/host policy owners and actual private native metadata adapter.
# Typed OS ports model precise result/error branches; filesystem_trace retains
# real native file/missing/directory observations separately.
set(_fs_presence_vfs "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_filesystem.c")
set(_fs_presence_host "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_filesystem_host.c")
set(_fs_presence_native "${CMAKE_SOURCE_DIR}/engine/src/os/platform/jce_filesystem_presence_native_internal.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_fs_presence_vfs}" "${_fs_presence_host}" "${_fs_presence_native}")
file(READ "${_fs_presence_host}" _fs_presence_source)
function(_jce_fs_presence_replace_once SOURCE_VARIABLE NEEDLE REPLACEMENT)
    string(FIND "${${SOURCE_VARIABLE}}" "${NEEDLE}" _first)
    string(FIND "${${SOURCE_VARIABLE}}" "${NEEDLE}" _last REVERSE)
    if(_first LESS 0 OR NOT _first EQUAL _last)
        message(FATAL_ERROR "Presence negative requires one real host-policy wire")
    endif()
    string(REPLACE "${NEEDLE}" "${REPLACEMENT}" _result "${${SOURCE_VARIABLE}}")
    set(${SOURCE_VARIABLE} "${_result}" PARENT_SCOPE)
endfunction()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fs-presence")
foreach(_fs_presence_variant IN ITEMS current missing_custom_unknown missing_unknown_priority)
    set(_fs_presence_owner "${_fs_presence_source}")
    set(_fs_presence_kind unit)
    set(_fs_presence_target test_jce_filesystem_presence)
    if(_fs_presence_variant STREQUAL "missing_custom_unknown")
        _jce_fs_presence_replace_once(_fs_presence_owner
            "            if (!s_host_read_service.probe) goto done;"
            "            if (!s_host_read_service.probe) { presence = JCE_FS_PRESENCE_ABSENT; goto done; }")
    elseif(_fs_presence_variant STREQUAL "missing_unknown_priority")
        _jce_fs_presence_replace_once(_fs_presence_owner
            "            if (presence != JCE_FS_PRESENCE_ABSENT ||"
            "            if (presence == JCE_FS_PRESENCE_PRESENT ||")
    endif()
    if(NOT _fs_presence_variant STREQUAL "current")
        set(_fs_presence_kind contract)
        string(APPEND _fs_presence_target "_${_fs_presence_variant}")
    endif()
    set(_fs_presence_path "${CMAKE_CURRENT_BINARY_DIR}/fs-presence/${_fs_presence_variant}.c")
    unset(_fs_presence_previous)
    if(EXISTS "${_fs_presence_path}")
        file(READ "${_fs_presence_path}" _fs_presence_previous)
    endif()
    if(NOT _fs_presence_previous STREQUAL _fs_presence_owner)
        file(WRITE "${_fs_presence_path}" "${_fs_presence_owner}")
    endif()
    jce_add_unit_test(${_fs_presence_target} KIND ${_fs_presence_kind}
        SOURCES test_jce_filesystem_presence.c LINK jce_core physfs-static sdl::sdl
        INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/core")
    target_compile_definitions(${_fs_presence_target} PRIVATE JCE_BUILDING_ENGINE=1
        JCE_TEST_FS_PRESENCE_VFS_OWNER="${_fs_presence_vfs}"
        JCE_TEST_FS_PRESENCE_HOST_OWNER="${_fs_presence_path}")
    if(MSVC)
        target_compile_options(${_fs_presence_target} PRIVATE /Zc:preprocessor)
    elseif(UNIX)
        # Expose the POSIX test port's file-mode constants under strict C99.
        target_compile_definitions(${_fs_presence_target} PRIVATE _XOPEN_SOURCE=700)
    endif()
    if(_fs_presence_kind STREQUAL "contract")
        set_tests_properties(${_fs_presence_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_fs_presence_variable IN ITEMS vfs host native source variant owner kind target path previous)
    unset(_fs_presence_${_fs_presence_variable})
endforeach()
unset(_fs_presence_variable)

# The existing actual Async lifecycle fixture also pins current-default stats.
# Ordinary control compiles the complete Async owner, no fake borrowed pointer.
set(_async_stats_owner "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_async.c")
file(READ "${_async_stats_owner}" _async_stats_source)
set(_async_stats_guard "    if (!g_default_closing && g_default_executor) {")
string(FIND "${_async_stats_source}" "${_async_stats_guard}" _async_stats_first)
string(FIND "${_async_stats_source}" "${_async_stats_guard}" _async_stats_last REVERSE)
if(_async_stats_first LESS 0 OR NOT _async_stats_first EQUAL _async_stats_last)
    message(FATAL_ERROR "Default stats control requires one actual owner live/closing guard")
endif()
string(REPLACE "${_async_stats_guard}" "    if (true) { /* Former copied-is-live inference. */"
    _async_stats_mutant "${_async_stats_source}")
set(_async_stats_dir "${CMAKE_CURRENT_BINARY_DIR}/async-default-stats")
file(MAKE_DIRECTORY "${_async_stats_dir}")
set(_async_stats_output "${_async_stats_dir}/missing_guard.c")
unset(_async_stats_previous)
if(EXISTS "${_async_stats_output}")
    file(READ "${_async_stats_output}" _async_stats_previous)
endif()
if(NOT _async_stats_previous STREQUAL _async_stats_mutant)
    file(WRITE "${_async_stats_output}" "${_async_stats_mutant}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_async_stats_owner}")
jce_add_unit_test(test_jce_async_default_stats_missing_guard KIND contract
    SOURCES test_jce_async.c "${_async_stats_output}"
    LINK jce_core sdl::sdl enkits::enkits
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/core")
set_tests_properties(test_jce_async_default_stats_missing_guard PROPERTIES WILL_FAIL TRUE)
foreach(_async_stats_var IN ITEMS owner source guard first last mutant dir output previous)
    unset(_async_stats_${_async_stats_var})
endforeach()
unset(_async_stats_var)

# Actual public allocator stats function: native provider vs conservative Web
# process-memory qualification, without a browser or native OS query.
set(_mem_stats_owner "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_allocator.c")
file(READ "${_mem_stats_owner}" _mem_stats_source)
set(_mem_stats_start_marker "bool jce_mem_stats(JceMemStats *out)")
set(_mem_stats_end_marker "void jce_alloc_low_mem_mode(bool on)")
foreach(_mem_stats_marker IN ITEMS start end)
    string(FIND "${_mem_stats_source}" "${_mem_stats_${_mem_stats_marker}_marker}" _mem_stats_first)
    string(FIND "${_mem_stats_source}" "${_mem_stats_${_mem_stats_marker}_marker}" _mem_stats_last REVERSE)
    if(_mem_stats_first LESS 0 OR NOT _mem_stats_first EQUAL _mem_stats_last)
        message(FATAL_ERROR "Memory stats controls require one actual function ${_mem_stats_marker} marker")
    endif()
    set(_mem_stats_${_mem_stats_marker} "${_mem_stats_first}")
endforeach()
if(NOT _mem_stats_end GREATER _mem_stats_start)
    message(FATAL_ERROR "Memory stats owner function markers are reversed")
endif()
math(EXPR _mem_stats_length "${_mem_stats_end} - ${_mem_stats_start}")
string(SUBSTRING "${_mem_stats_source}" ${_mem_stats_start} ${_mem_stats_length} _mem_stats_actual)
set(_mem_stats_guard "#if JCE_PLATFORM_WEB\n    /* Emscripten's mimalloc primitive leaves allocator commit estimates as\n     * RSS/commit defaults. They are not OS process memory observations. */\n    cur_rss = peak_rss = 0;\n    cur_commit = peak_commit = 0;\n#endif\n")
string(FIND "${_mem_stats_actual}" "${_mem_stats_guard}" _mem_stats_first)
string(FIND "${_mem_stats_actual}" "${_mem_stats_guard}" _mem_stats_last REVERSE)
if(_mem_stats_first LESS 0 OR NOT _mem_stats_first EQUAL _mem_stats_last)
    message(FATAL_ERROR "Memory stats control requires one actual Web estimate qualification")
endif()
set(_mem_stats_dir "${CMAKE_CURRENT_BINARY_DIR}/mem-stats-platform")
file(MAKE_DIRECTORY "${_mem_stats_dir}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mem_stats_owner}")
foreach(_mem_stats_variant IN ITEMS current old_web_estimate)
    set(_mem_stats_target test_jce_mem_stats_platform)
    set(_mem_stats_kind unit)
    set(_mem_stats_content "${_mem_stats_actual}")
    if(NOT _mem_stats_variant STREQUAL "current")
        string(APPEND _mem_stats_target "_${_mem_stats_variant}")
        set(_mem_stats_kind contract)
        string(REPLACE "${_mem_stats_guard}" "/* Former Web allocator estimates reported as process memory. */\n"
            _mem_stats_content "${_mem_stats_actual}")
    endif()
    set(_mem_stats_header "${_mem_stats_dir}/${_mem_stats_variant}.h")
    unset(_mem_stats_previous)
    if(EXISTS "${_mem_stats_header}")
        file(READ "${_mem_stats_header}" _mem_stats_previous)
    endif()
    if(NOT _mem_stats_previous STREQUAL _mem_stats_content)
        file(WRITE "${_mem_stats_header}" "${_mem_stats_content}")
    endif()
    jce_add_unit_test(${_mem_stats_target} KIND ${_mem_stats_kind}
        SOURCES test_jce_mem_stats_platform.c)
    target_compile_definitions(${_mem_stats_target} PRIVATE
        JCE_TEST_MEM_STATS_OWNER="${_mem_stats_header}")
    if(_mem_stats_kind STREQUAL "contract")
        set_tests_properties(${_mem_stats_target} PROPERTIES WILL_FAIL TRUE)
    endif()
endforeach()
foreach(_mem_stats_var IN ITEMS owner source start_marker end_marker marker first last start end length actual guard dir variant target kind content header previous)
    unset(_mem_stats_${_mem_stats_var})
endforeach()
unset(_mem_stats_var)
