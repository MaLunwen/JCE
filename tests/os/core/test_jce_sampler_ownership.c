/* Exercise the actual sampler owner with controlled native outcomes on all OSes.
 * No native worker, file output, graphics device or Windows SDK is required. */
#include "unity.h"
#define jce_sampler_init sampler_control_init
#define jce_sampler_shutdown sampler_control_shutdown
#include "jce_sampler_test_owner.h"

void setUp(void) { port_reset(); }
void tearDown(void)
{
    s_wait_result = WAIT_OBJECT_0; s_resume_failure = false;
    s_exit_query_failure = false;
    /* Failed exit tests must also clean up against the BEFORE owner. Restore
     * a resumable fake obligation after assertions; no native thread exists. */
    if (g_s.target_suspended && g_s.target) {
        g_s.target->lifetime->terminated = false;
        g_s.target->lifetime->suspend_count = 1;
    }
    for (uint32_t t = 0; t < g_thr_count; ++t) {
        if (!g_thr[t].suspended || !g_thr[t].h) continue;
        g_thr[t].h->lifetime->terminated = false;
        g_thr[t].h->lifetime->suspend_count = 1;
    }
    if (g_s.worker) sampler_control_shutdown();
}

static void test_shutdown_timeout_preserves_owner_and_blocks_reinit(void)
{
    port_start(); port_roster(2, 11, 100); samp_refresh_threads(999);
    HANDLE worker = g_s.worker, target = g_s.target, observed = g_thr[0].h;
    s_wait_result = WAIT_TIMEOUT;
    TEST_ASSERT_FALSE(sampler_control_shutdown());
    port_delayed_worker_access();
    sampler_control_init(); /* actual reinit attempt while the old worker owns state */
    TEST_ASSERT_EQUAL_UINT(0, s_invalid_worker_access);
    TEST_ASSERT_EQUAL_PTR(worker, g_s.worker);
    TEST_ASSERT_EQUAL_PTR(target, g_s.target);
    TEST_ASSERT_EQUAL_PTR(observed, g_thr[0].h);
    TEST_ASSERT_EQUAL_UINT(0, worker->closes + target->closes + observed->closes);
    TEST_ASSERT_EQUAL_UINT(0, s_slot_frees + s_writes);
    TEST_ASSERT_EQUAL_UINT(1, s_created_workers);
    TEST_ASSERT_EQUAL_PTR(s_initial_slots, g_s.slots);
    s_wait_result = WAIT_OBJECT_0;
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_EQUAL_UINT(1, s_slot_frees);
    TEST_ASSERT_EQUAL_UINT(1, worker->closes);
    TEST_ASSERT_EQUAL_UINT(1, target->closes);
    TEST_ASSERT_EQUAL_UINT(1, observed->closes);
}

static void test_shutdown_wait_failure_preserves_owner_and_retry_releases_once(void)
{
    port_start();
    HANDLE worker = g_s.worker, target = g_s.target;
    s_wait_result = WAIT_FAILED;
    TEST_ASSERT_FALSE(sampler_control_shutdown());
    TEST_ASSERT_EQUAL_PTR(s_initial_slots, g_s.slots);
    TEST_ASSERT_EQUAL_UINT(0, worker->closes + target->closes + s_slot_frees);
    s_wait_result = WAIT_OBJECT_0;
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_NULL(g_s.slots);
    TEST_ASSERT_EQUAL_UINT(1, worker->closes);
    TEST_ASSERT_EQUAL_UINT(1, target->closes);
    TEST_ASSERT_EQUAL_UINT(1, s_slot_frees);
}

static void test_early_shutdown_delay_and_period_wait_do_not_sample(void)
{
    s_delay = 4000; port_start(); s_stop_in_sleep = true;
    samp_thread(NULL);
    TEST_ASSERT_EQUAL_UINT(0, s_suspends);
    sampler_control_shutdown();
    s_delay = 0; s_sleeps = 0; port_roster(1, 11, 100); port_start();
    samp_thread(NULL);
    TEST_ASSERT_EQUAL_UINT(0, s_suspends);
    TEST_ASSERT_EQUAL_UINT64(0, g_s.total);
}

static void test_roster_reorder_retirement_and_tid_reuse_keep_histogram_identity(void)
{
    port_start(); port_roster(2, 11, 100);
    s_roster[0].name = L"alpha"; s_roster[1].name = L"beta";
    samp_refresh_threads(999);
    samp_record(0x1000, 0); ++g_thr[0].samples; ++g_s.total;
    PortThread swap = s_roster[0]; s_roster[0] = s_roster[1]; s_roster[1] = swap;
    samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_STRING("alpha", g_thr[0].name);
    TEST_ASSERT_EQUAL_UINT32(11, g_thr[0].tid);
    s_roster_count = 1; samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_UINT(2, g_thr_count);
    TEST_ASSERT_NULL(g_thr[0].h);
    s_roster_count = 2; s_roster[1].born = 777; s_roster[1].name = L"gamma";
    samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_UINT(3, g_thr_count);
    TEST_ASSERT_EQUAL_STRING("alpha", g_thr[0].name);
    TEST_ASSERT_EQUAL_STRING("gamma", g_thr[2].name);
    samp_record(0x3000, 2); ++g_thr[2].samples; ++g_s.total;
    sampler_control_shutdown();
    const char *old = strstr(s_report, "# alpha");
    const char *fresh = strstr(s_report, "# gamma");
    TEST_ASSERT_NOT_NULL(old); TEST_ASSERT_NOT_NULL(fresh);
    TEST_ASSERT_TRUE(old < fresh);
    const char *old_pc = strstr(old, "% of thread   control!pc_1000");
    TEST_ASSERT_NOT_NULL(old_pc);
    TEST_ASSERT_TRUE(old_pc < fresh);
    TEST_ASSERT_NOT_NULL(strstr(fresh, "% of thread   control!pc_3000"));
}

static void assert_thread_report_pc(DWORD tid, uint64_t born,
                                    const char *name, uint64_t pc)
{
    char label[128], function[64];
    snprintf(label, sizeof label, "# %s [tid=%lu created=0x%llx",
             name, (unsigned long)tid, (unsigned long long)born);
    snprintf(function, sizeof function, "%% of thread   control!pc_%llx",
             (unsigned long long)pc);
    const char *row = strstr(s_report, label);
    TEST_ASSERT_NOT_NULL(row);
    const char *hit = strstr(row, function);
    TEST_ASSERT_NOT_NULL(hit);
    const char *next = strstr(row + 1, "\n# ");
    TEST_ASSERT_TRUE(!next || hit < next);
}

static void test_process_sweep_covers_102_threads_including_late_roster(void)
{
    port_start(); port_roster(102, 1000, 5000);
    for (unsigned t = 84; t < 100; ++t) s_roster[t].name = L"late-busy";
    samp_thread(NULL); /* actual collector: one complete sweep, then stop */
    TEST_ASSERT_EQUAL_UINT(102, g_thr_count);
    TEST_ASSERT_EQUAL_UINT64(102, g_s.total);
    TEST_ASSERT_EQUAL_UINT64(0, g_s.missed);
    TEST_ASSERT_EQUAL_UINT64(0, g_s.thread_cap_skipped);
    TEST_ASSERT_EQUAL_UINT(102, s_suspends);
    TEST_ASSERT_EQUAL_UINT(102, s_resumes);
    for (uint32_t t = 0; t < 102; ++t) {
        TEST_ASSERT_EQUAL_UINT32(1000 + t, g_thr[t].tid);
        TEST_ASSERT_EQUAL_UINT64(5000 + t, g_thr[t].created);
        TEST_ASSERT_EQUAL_UINT64(1, g_thr[t].samples);
        TEST_ASSERT_FALSE(g_thr[t].suspended);
        TEST_ASSERT_EQUAL_UINT(0, g_thr[t].h->lifetime->suspend_count);
    }
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_EQUAL_UINT(1, s_writes);
    TEST_ASSERT_NOT_NULL(strstr(s_report, "thread lifetime cap 128, 102 admitted"));
    for (unsigned t = 84; t < 100; ++t)
        assert_thread_report_pc(1000 + t, 5000 + t, "late-busy", 0x1000u + t * 16u);
}

static void test_high_thread_indices_keep_same_pc_histograms_separate(void)
{
    static const uint16_t indices[] = { 0, 64, 127 };
    port_start(); port_roster(128, 1000, 5000); samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_UINT(128, g_thr_count); /* fail before any high-index access */
    for (unsigned k = 0; k < 3; ++k) {
        for (unsigned n = 0; n <= k; ++n) {
            samp_record(0xa000, indices[k]);
            ++g_thr[indices[k]].samples; ++g_s.total;
        }
    }
    unsigned seen = 0;
    for (uint32_t s = 0; s < SAMP_CAP; ++s) {
        const SampSlot *slot = &g_s.slots[s];
        if (slot->pc != 0xa000) continue;
        unsigned k = 0;
        while (k < 3 && indices[k] != slot->tidx) ++k;
        TEST_ASSERT_TRUE(k < 3);
        TEST_ASSERT_EQUAL_UINT(k + 1, slot->count);
        seen |= 1u << k;
    }
    TEST_ASSERT_EQUAL_UINT(7, seen);
    TEST_ASSERT_EQUAL_UINT64(6, g_s.total);
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    for (unsigned k = 0; k < 3; ++k)
        assert_thread_report_pc(1000 + indices[k], 5000 + indices[k], "worker", 0xa000);
}

static void test_session_thread_cap_retains_old_entries_and_reports_skips(void)
{
    port_start(); port_roster(SAMP_MAX_THREADS, 1, 100);
    samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_UINT(SAMP_MAX_THREADS, g_thr_count);
    samp_record(0x1000, 0); ++g_thr[0].samples; ++g_s.total;
    port_roster(1, 1000, 9000); samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_UINT(SAMP_MAX_THREADS, g_thr_count);
    TEST_ASSERT_EQUAL_UINT32(1, g_thr[0].tid);
    sampler_control_shutdown();
    TEST_ASSERT_NOT_NULL(strstr(s_report, "thread lifetime cap 128"));
    TEST_ASSERT_NOT_NULL(strstr(s_report, "1 capacity-skipped"));
}

static void test_resume_failure_is_not_accepted_sample_and_cleanup_retries_owned_suspend(void)
{
    port_start(); port_roster(1, 11, 100); s_resume_failure = true;
    samp_thread(NULL);
    TEST_ASSERT_EQUAL_UINT64(0, g_s.total);
    TEST_ASSERT_EQUAL_UINT(1, s_suspends);
    HANDLE suspended = g_thr[0].h, worker = g_s.worker;
    s_wait_result = WAIT_TIMEOUT;
    unsigned attempts = s_resumes;
    TEST_ASSERT_FALSE(sampler_control_shutdown());
    TEST_ASSERT_EQUAL_UINT(attempts, s_resumes);
    TEST_ASSERT_EQUAL_PTR(worker, g_s.worker);
    s_wait_result = WAIT_OBJECT_0;
    TEST_ASSERT_FALSE(sampler_control_shutdown());
    TEST_ASSERT_EQUAL_PTR(worker, g_s.worker);
    TEST_ASSERT_EQUAL_UINT(0, suspended->closes + s_slot_frees);
    s_resume_failure = false;
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_EQUAL_UINT(0, suspended->lifetime->suspend_count);
    TEST_ASSERT_EQUAL_UINT(1, suspended->closes);
    TEST_ASSERT_EQUAL_UINT(1, s_slot_frees);
}

static void test_successful_sampling_balances_only_its_borrowed_suspend(void)
{
    port_start(); port_roster(1, 11, 100); samp_refresh_threads(999);
    HANDLE observed = g_thr[0].h;
    observed->lifetime->suspend_count = 2; /* acquired by another owner */
    samp_thread(NULL);
    TEST_ASSERT_EQUAL_UINT64(1, g_s.total);
    TEST_ASSERT_EQUAL_UINT(1, s_suspends);
    TEST_ASSERT_EQUAL_UINT(1, s_resumes);
    TEST_ASSERT_EQUAL_UINT(2, observed->lifetime->suspend_count);
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_EQUAL_UINT(1, s_resumes); /* cleanup cannot resume other owners */
    TEST_ASSERT_EQUAL_UINT(2, observed->lifetime->suspend_count);
    TEST_ASSERT_EQUAL_UINT(1, observed->closes);
    TEST_ASSERT_EQUAL_UINT(1, s_slot_frees);
    TEST_ASSERT_EQUAL_UINT(1, s_writes);
    TEST_ASSERT_NOT_NULL(strstr(s_report, "% of thread   control!pc_1000"));
}

static void test_unavailable_thread_identity_is_skipped_without_rekeying_history(void)
{
    port_start(); port_roster(1, 11, 100); samp_refresh_threads(999);
    samp_record(0x1000, 0); ++g_thr[0].samples; ++g_s.total;
    s_identity_failure = true; samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_UINT(1, g_thr_count);
    TEST_ASSERT_EQUAL_UINT64(1, g_thr[0].samples);
    TEST_ASSERT_NULL(g_thr[0].h);
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_NOT_NULL(strstr(s_report, "1 identity failures"));
    TEST_ASSERT_NOT_NULL(strstr(s_report, "% of thread   control!pc_1000"));
}

static void test_main_target_confirmed_exit_releases_owned_suspend(void)
{
    s_all = false; port_start(); g_s.target->pc = 0x4000;
    s_resume_failure = true; samp_thread(NULL);
    HANDLE target = g_s.target, worker = g_s.worker;
    TEST_ASSERT_TRUE(g_s.target_suspended);
    target->lifetime->exited = 500;
    target->lifetime->terminated = true;
    target->lifetime->suspend_count = 0; /* native termination ends suspension */
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_EQUAL_UINT(1, target->closes);
    TEST_ASSERT_EQUAL_UINT(1, worker->closes);
    TEST_ASSERT_EQUAL_UINT(1, s_slot_frees);
    TEST_ASSERT_EQUAL_UINT(1, s_writes);
    TEST_ASSERT_NULL(g_s.slots);
}

static void test_all_thread_confirmed_exit_releases_owned_suspend(void)
{
    port_start(); port_roster(1, 11, 100);
    s_resume_failure = true; samp_thread(NULL);
    HANDLE observed = g_thr[0].h;
    TEST_ASSERT_TRUE(g_thr[0].suspended);
    observed->lifetime->exited = 500;
    observed->lifetime->terminated = true;
    observed->lifetime->suspend_count = 0;
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_EQUAL_UINT(1, observed->closes);
    TEST_ASSERT_EQUAL_UINT(1, s_slot_frees);
    TEST_ASSERT_NULL(g_s.slots);
}

static void test_failed_exit_query_retains_suspended_owner_for_later_retry(void)
{
    s_all = false; port_start(); g_s.target->pc = 0x4000;
    s_resume_failure = true; samp_thread(NULL);
    HANDLE target = g_s.target, worker = g_s.worker;
    target->lifetime->exited = 500; target->lifetime->terminated = true;
    target->lifetime->suspend_count = 0;
    s_exit_query_failure = true;
    TEST_ASSERT_FALSE(sampler_control_shutdown());
    TEST_ASSERT_EQUAL_PTR(worker, g_s.worker);
    TEST_ASSERT_EQUAL_PTR(target, g_s.target);
    TEST_ASSERT_EQUAL_UINT(0, target->closes + worker->closes + s_slot_frees + s_writes);
    sampler_control_init();
    TEST_ASSERT_EQUAL_UINT(1, s_created_workers);
    s_exit_query_failure = false;
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_EQUAL_UINT(1, target->closes);
    TEST_ASSERT_EQUAL_UINT(1, s_slot_frees);
}

static void test_live_thread_exit_time_cannot_release_acquired_suspend(void)
{
    port_start(); port_roster(1, 11, 100);
    s_resume_failure = true; samp_thread(NULL);
    HANDLE observed = g_thr[0].h;
    observed->lifetime->exited = 500; /* deliberately poisoned live FILETIME */
    observed->lifetime->terminated = false;
    TEST_ASSERT_FALSE(sampler_control_shutdown());
    TEST_ASSERT_EQUAL_UINT(0, observed->closes + s_slot_frees + s_writes);
    TEST_ASSERT_TRUE(g_thr[0].suspended);
    s_resume_failure = false;
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_EQUAL_UINT(0, observed->lifetime->suspend_count);
    TEST_ASSERT_EQUAL_UINT(1, observed->closes);
}

static void test_thread_name_missing_modules_keeps_tid_fallback(void)
{
    s_native_modules[0] = s_native_modules[1] = false;
    port_start(); port_roster(1, 11, 100); samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_STRING("tid-11", g_thr[0].name);
    TEST_ASSERT_EQUAL_UINT(2, s_module_queries);
    TEST_ASSERT_EQUAL_UINT(0, s_export_queries + s_description_queries + s_description_frees);
}

static void test_missing_thread_name_clears_prior_output_without_native_call(void)
{
    s_native_modules[0] = s_native_modules[1] = false;
    wchar_t prior[] = L"prior";
    PWSTR description = prior;
    TEST_ASSERT_EQUAL_INT32(E_NOTIMPL,
        jce_native_get_thread_description_internal(NULL, &description));
    TEST_ASSERT_NULL(description);
    TEST_ASSERT_EQUAL_UINT(0, s_description_queries + s_description_frees);
}

static void test_thread_name_missing_exports_keeps_tid_fallback(void)
{
    s_native_exports[0] = s_native_exports[1] = false;
    port_start(); port_roster(1, 11, 100); samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_STRING("tid-11", g_thr[0].name);
    TEST_ASSERT_EQUAL_UINT(2, s_module_queries);
    TEST_ASSERT_EQUAL_UINT(2, s_export_queries);
    TEST_ASSERT_EQUAL_UINT(0, s_description_queries + s_description_frees);
}

static void test_thread_name_uses_kernelbase_before_kernel32(void)
{
    port_start(); port_roster(1, 11, 100); samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_STRING("worker", g_thr[0].name);
    TEST_ASSERT_EQUAL_UINT(1, s_module_queries);
    TEST_ASSERT_EQUAL_UINT(1, s_export_queries);
    TEST_ASSERT_EQUAL_UINT(0, s_resolved_module);
    TEST_ASSERT_EQUAL_UINT(1, s_description_queries);
    TEST_ASSERT_EQUAL_UINT(1, s_description_frees);
}

static void test_thread_name_uses_kernel32_if_kernelbase_export_absent(void)
{
    s_native_exports[0] = false;
    port_start(); port_roster(1, 11, 100); samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_STRING("worker", g_thr[0].name);
    TEST_ASSERT_EQUAL_UINT(2, s_module_queries);
    TEST_ASSERT_EQUAL_UINT(2, s_export_queries);
    TEST_ASSERT_EQUAL_UINT(1, s_resolved_module);
    TEST_ASSERT_EQUAL_UINT(1, s_description_queries);
    TEST_ASSERT_EQUAL_UINT(1, s_description_frees);
}

static void test_failed_thread_name_api_keeps_tid_and_releases_native_output(void)
{
    s_description_result = (HRESULT)0x80004005L;
    port_start(); port_roster(1, 11, 100); samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_STRING("tid-11", g_thr[0].name);
    TEST_ASSERT_EQUAL_UINT(1, s_description_queries);
    TEST_ASSERT_EQUAL_UINT(1, s_description_frees);
}

static void test_empty_thread_name_keeps_tid_and_releases_native_output(void)
{
    port_start(); port_roster(1, 11, 100); s_roster[0].name = L"";
    samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_STRING("tid-11", g_thr[0].name);
    TEST_ASSERT_EQUAL_UINT(1, s_description_queries);
    TEST_ASSERT_EQUAL_UINT(1, s_description_frees);
}

static void test_null_thread_name_keeps_tid_without_fictitious_free(void)
{
    s_description_null = true;
    port_start(); port_roster(1, 11, 100); samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_STRING("tid-11", g_thr[0].name);
    TEST_ASSERT_EQUAL_UINT(1, s_description_queries);
    TEST_ASSERT_EQUAL_UINT(0, s_description_frees);
}

static void test_invalid_thread_name_output_does_not_query_native_modules(void)
{
    TEST_ASSERT_EQUAL_INT32(E_POINTER, jce_native_get_thread_description_internal(NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(0, s_module_queries + s_export_queries + s_description_queries);
}

static void test_optional_refresh_present_preserves_handle_result_and_error(void)
{
    HANDLE process = port_handle(3);
    s_refresh_result = 7; s_refresh_error = 1234u;
    TEST_ASSERT_EQUAL_INT(7, jce_native_sym_refresh_module_list_internal(process));
    TEST_ASSERT_EQUAL_PTR(process, s_refresh_process);
    TEST_ASSERT_EQUAL_UINT(1234u, GetLastError());
    TEST_ASSERT_EQUAL_UINT(1, s_dbghelp_module_queries);
    TEST_ASSERT_EQUAL_UINT(1, s_refresh_export_queries);
    TEST_ASSERT_EQUAL_UINT(1, s_refresh_calls);
}

static void test_optional_refresh_missing_module_is_truthfully_unavailable(void)
{
    s_dbghelp_module_available = false;
    TEST_ASSERT_FALSE(jce_native_sym_refresh_module_list_internal(NULL));
    TEST_ASSERT_EQUAL_UINT(120u, GetLastError());
    TEST_ASSERT_EQUAL_UINT(1, s_dbghelp_module_queries);
    TEST_ASSERT_EQUAL_UINT(0, s_refresh_export_queries + s_refresh_calls);
}

static void test_optional_refresh_missing_export_is_truthfully_unavailable(void)
{
    s_refresh_export_available = false;
    TEST_ASSERT_FALSE(jce_native_sym_refresh_module_list_internal(NULL));
    TEST_ASSERT_EQUAL_UINT(120u, GetLastError());
    TEST_ASSERT_EQUAL_UINT(1, s_dbghelp_module_queries);
    TEST_ASSERT_EQUAL_UINT(1, s_refresh_export_queries);
    TEST_ASSERT_EQUAL_UINT(0, s_refresh_calls);
}

static void test_optional_refresh_real_provider_false_is_preserved(void)
{
    s_refresh_result = FALSE; s_refresh_error = 5u;
    TEST_ASSERT_FALSE(jce_native_sym_refresh_module_list_internal(NULL));
    TEST_ASSERT_EQUAL_UINT(5u, GetLastError());
    TEST_ASSERT_EQUAL_UINT(1, s_refresh_calls);
}

static void test_optional_refresh_does_not_cache_absence(void)
{
    s_dbghelp_module_available = false;
    TEST_ASSERT_FALSE(jce_native_sym_refresh_module_list_internal(NULL));
    s_dbghelp_module_available = true;
    TEST_ASSERT_TRUE(jce_native_sym_refresh_module_list_internal(NULL));
    TEST_ASSERT_EQUAL_UINT(2, s_dbghelp_module_queries);
    TEST_ASSERT_EQUAL_UINT(1, s_refresh_export_queries);
    TEST_ASSERT_EQUAL_UINT(1, s_refresh_calls);
}

static void assert_optional_refresh_still_reports_symbols(void)
{
    port_start(); port_roster(1, 11, 100); samp_refresh_threads(999);
    samp_record(0x1000, 0); ++g_thr[0].samples; ++g_s.total;
    TEST_ASSERT_TRUE(sampler_control_shutdown());
    TEST_ASSERT_NOT_NULL(strstr(s_report, "% of thread   control!pc_1000"));
    TEST_ASSERT_EQUAL_UINT(1, s_dbghelp_module_queries);
    TEST_ASSERT_EQUAL_UINT(1, s_refresh_export_queries);
    TEST_ASSERT_EQUAL_UINT(1, s_writes);
}

static void test_optional_refresh_absence_keeps_existing_symbol_report(void)
{
    s_refresh_export_available = false;
    assert_optional_refresh_still_reports_symbols();
    TEST_ASSERT_EQUAL_UINT(0, s_refresh_calls);
}

static void test_optional_refresh_failure_keeps_existing_symbol_report(void)
{
    s_refresh_result = FALSE; s_refresh_error = 5u;
    assert_optional_refresh_still_reports_symbols();
    TEST_ASSERT_EQUAL_UINT(1, s_refresh_calls);
}

static void test_optional_refresh_error_does_not_poison_later_enumeration(void)
{
    s_refresh_result = FALSE; s_refresh_error = 5u;
    TEST_ASSERT_FALSE(jce_native_sym_refresh_module_list_internal(NULL));
    port_start(); port_roster(1, 11, 100); samp_refresh_threads(999);
    TEST_ASSERT_EQUAL_UINT(0, g_s.thread_snapshot_failed);
    TEST_ASSERT_EQUAL_UINT(1, g_thr_count);
    TEST_ASSERT_EQUAL_UINT(ERROR_NO_MORE_FILES, GetLastError());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_optional_refresh_error_does_not_poison_later_enumeration);
    RUN_TEST(test_optional_refresh_present_preserves_handle_result_and_error);
    RUN_TEST(test_optional_refresh_missing_module_is_truthfully_unavailable);
    RUN_TEST(test_optional_refresh_missing_export_is_truthfully_unavailable);
    RUN_TEST(test_optional_refresh_real_provider_false_is_preserved);
    RUN_TEST(test_optional_refresh_does_not_cache_absence);
    RUN_TEST(test_optional_refresh_absence_keeps_existing_symbol_report);
    RUN_TEST(test_optional_refresh_failure_keeps_existing_symbol_report);
    RUN_TEST(test_thread_name_missing_modules_keeps_tid_fallback);
    RUN_TEST(test_missing_thread_name_clears_prior_output_without_native_call);
    RUN_TEST(test_thread_name_missing_exports_keeps_tid_fallback);
    RUN_TEST(test_thread_name_uses_kernelbase_before_kernel32);
    RUN_TEST(test_thread_name_uses_kernel32_if_kernelbase_export_absent);
    RUN_TEST(test_failed_thread_name_api_keeps_tid_and_releases_native_output);
    RUN_TEST(test_empty_thread_name_keeps_tid_and_releases_native_output);
    RUN_TEST(test_null_thread_name_keeps_tid_without_fictitious_free);
    RUN_TEST(test_invalid_thread_name_output_does_not_query_native_modules);
    RUN_TEST(test_shutdown_timeout_preserves_owner_and_blocks_reinit);
    RUN_TEST(test_shutdown_wait_failure_preserves_owner_and_retry_releases_once);
    RUN_TEST(test_early_shutdown_delay_and_period_wait_do_not_sample);
    RUN_TEST(test_roster_reorder_retirement_and_tid_reuse_keep_histogram_identity);
    RUN_TEST(test_process_sweep_covers_102_threads_including_late_roster);
    RUN_TEST(test_high_thread_indices_keep_same_pc_histograms_separate);
    RUN_TEST(test_session_thread_cap_retains_old_entries_and_reports_skips);
    RUN_TEST(test_resume_failure_is_not_accepted_sample_and_cleanup_retries_owned_suspend);
    RUN_TEST(test_successful_sampling_balances_only_its_borrowed_suspend);
    RUN_TEST(test_unavailable_thread_identity_is_skipped_without_rekeying_history);
    RUN_TEST(test_main_target_confirmed_exit_releases_owned_suspend);
    RUN_TEST(test_all_thread_confirmed_exit_releases_owned_suspend);
    RUN_TEST(test_failed_exit_query_retains_suspended_owner_for_later_retry);
    RUN_TEST(test_live_thread_exit_time_cannot_release_acquired_suspend);
    return UNITY_END();
}
