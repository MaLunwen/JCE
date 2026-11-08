/* Exact quit owner CPU controls. These verify pending/result/session policy;
 * full engine-frame/KPI/WILL_QUIT and native process exit need integration. */
#include <jce/application/jce_engine.h>
#include "application/jce_engine_quit.h"
#include "unity.h"
void setUp(void) { jce_engine_quit_end_session(); }
void tearDown(void) { jce_engine_quit_end_session(); }
static void legacy_success_retains_terminal_failure_and_continues_untouched(void)
{
    TEST_ASSERT_FALSE(jce_engine_quit_requested());
    jce_engine_request_quit();
    TEST_ASSERT_TRUE(jce_engine_quit_requested());
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, jce_engine_quit_resolve(JCE_APP_SUCCESS));
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, jce_engine_quit_resolve(JCE_APP_FAILURE));
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, jce_engine_quit_resolve(JCE_APP_CONTINUE));
}
static void explicit_failure_cannot_be_downgraded_by_any_success_request(void)
{
    jce_engine_request_quit_with_result(JCE_APP_FAILURE);
    jce_engine_request_quit();
    jce_engine_request_quit_with_result(JCE_APP_SUCCESS);
    TEST_ASSERT_TRUE(jce_engine_quit_requested());
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, jce_engine_quit_resolve(JCE_APP_SUCCESS));
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, jce_engine_quit_resolve(JCE_APP_CONTINUE));
}
static void nonterminal_and_invalid_requests_do_not_create_or_change_quit(void)
{
    jce_engine_request_quit_with_result(JCE_APP_CONTINUE);
    jce_engine_request_quit_with_result((JceAppResult)99);
    TEST_ASSERT_FALSE(jce_engine_quit_requested());
    jce_engine_request_quit_with_result(JCE_APP_FAILURE);
    jce_engine_request_quit_with_result(JCE_APP_CONTINUE);
    jce_engine_request_quit_with_result((JceAppResult)-1);
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, jce_engine_quit_resolve(JCE_APP_SUCCESS));
}
static void pending_precreate_request_survives_until_completed_owner_end(void)
{
    jce_engine_request_quit_with_result(JCE_APP_FAILURE);
    // No create-entry reset exists in this owner; reading or finalizing a
    // result does not consume the pre-create pending request.
    TEST_ASSERT_TRUE(jce_engine_quit_requested());
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, jce_engine_quit_resolve(JCE_APP_SUCCESS));
    TEST_ASSERT_TRUE(jce_engine_quit_requested());
    jce_engine_quit_end_session();
    TEST_ASSERT_FALSE(jce_engine_quit_requested());
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, jce_engine_quit_resolve(JCE_APP_SUCCESS));
}
static void next_session_starts_clean_after_failed_and_successful_shutdown(void)
{
    jce_engine_request_quit_with_result(JCE_APP_FAILURE);
    jce_engine_quit_end_session();
    jce_engine_request_quit();
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, jce_engine_quit_resolve(JCE_APP_SUCCESS));
    jce_engine_quit_end_session();
    TEST_ASSERT_FALSE(jce_engine_quit_requested());
}
static void a_late_failure_is_visible_when_terminal_result_is_resolved_again(void)
{
    jce_engine_request_quit_with_result(JCE_APP_SUCCESS);
    JceAppResult before = jce_engine_quit_resolve(JCE_APP_SUCCESS);
    jce_engine_request_quit_with_result(JCE_APP_FAILURE);
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, jce_engine_quit_resolve(before));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(legacy_success_retains_terminal_failure_and_continues_untouched);
    RUN_TEST(explicit_failure_cannot_be_downgraded_by_any_success_request);
    RUN_TEST(nonterminal_and_invalid_requests_do_not_create_or_change_quit);
    RUN_TEST(pending_precreate_request_survives_until_completed_owner_end);
    RUN_TEST(next_session_starts_clean_after_failed_and_successful_shutdown);
    RUN_TEST(a_late_failure_is_visible_when_terminal_result_is_resolved_again);
    return UNITY_END();
}
