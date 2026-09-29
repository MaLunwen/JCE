cmake_minimum_required(VERSION 3.20)

jce_add_unit_test(test_jce_str             SOURCES test_jce_str.c             LINK jce_core)
jce_add_unit_test(test_jce_path            SOURCES test_jce_path.c            LINK jce_core)
jce_add_unit_test(test_jce_alloc           SOURCES test_jce_alloc.c           LINK jce_core)
jce_add_unit_test(test_jce_allocator       SOURCES test_jce_allocator.c       LINK jce_core)
jce_add_unit_test(test_jce_handle          SOURCES test_jce_handle.c          LINK jce_core)
jce_add_unit_test(test_jce_event           SOURCES test_jce_event.c           LINK jce_core)
jce_add_unit_test(test_jce_sysinfo         SOURCES test_jce_sysinfo.c         LINK jce_core)
jce_add_unit_test(test_jce_timer           SOURCES test_jce_timer.c           LINK jce_core)
jce_add_unit_test(test_jce_fixed_clock     SOURCES test_jce_fixed_clock.c     LINK jce_core)
jce_add_unit_test(test_jce_json            SOURCES test_jce_json.c            LINK jce_core)
# Malicious/edge input for the JSON facade (audit §35.2): escaping, UTF-8,
# nesting depth, truncated documents, explicit-length bounds, defaults.
jce_add_unit_test(test_jce_json_hardening  SOURCES test_jce_json_hardening.c  LINK jce_core)
jce_add_unit_test(test_jce_profiler        SOURCES test_jce_profiler.c        LINK jce_core)
jce_add_unit_test(test_jce_perf_phase      SOURCES test_jce_perf_phase.c      LINK jce_core)
jce_add_unit_test(test_jce_trace           SOURCES test_jce_trace.c           LINK jce_core)
jce_add_unit_test(test_jce_frustum         SOURCES test_jce_frustum.c         LINK jce_core)
jce_add_unit_test(test_jce_thread          SOURCES test_jce_thread.c          LINK jce_core)
jce_add_unit_test(test_jce_async           SOURCES test_jce_async.c           LINK jce_core)
# jce_jobs was retired onto the enkiTS pool (P1-2).  Its replacement pins the
# invariant the migration nearly broke: parallel_for partitions must start on a
# multiple of `chunk`, because jce_render_queue.c and jce_scene_renderer.c derive
# a single-writer slot index from `begin`.
jce_add_unit_test(test_jce_thread_parallel_for SOURCES test_jce_thread_parallel_for.c LINK jce_core)
jce_add_unit_test(test_jce_filesystem_host SOURCES test_jce_filesystem_host.c LINK jce_core)
jce_add_unit_test(test_jce_filesystem      SOURCES test_jce_filesystem.c      LINK jce_core jce_resource)
jce_add_unit_test(test_jce_log             SOURCES test_jce_log.c             LINK jce_core)
jce_add_unit_test(test_jce_config          SOURCES test_jce_config.c          LINK jce_core)
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
