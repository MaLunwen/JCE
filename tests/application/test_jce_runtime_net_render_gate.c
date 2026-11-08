/* Exact runtime networking render-step block with real public declarations
 * and typed counted session/interpolation ports. No runtime boot/socket/GPU. */
#include "unity.h"
#include <jce/middleware/net/jce_session.h>
#include <jce/middleware/net/jce_net_transform.h>
#include <jce/middleware/net/jce_net_animator.h>

static JceSessionMode observed_mode;
static unsigned mode_reads,transforms,animators,order;
static double transform_alpha,animator_alpha;
JceSessionMode JCE_CALL jce_session_mode(void) { ++mode_reads;return observed_mode; }
void JCE_CALL jce_net_transform_render_step(double alpha)
{ ++transforms;transform_alpha=alpha;order=order*10u+1u; }
void JCE_CALL jce_net_animator_render_step(double alpha)
{ ++animators;animator_alpha=alpha;order=order*10u+2u; }

/* Only the fields borrowed by the actual call-site block, not a runtime ABI. */
typedef struct RuntimeGateView {
    bool net_bridged;
    struct { double alpha; } clock;
} RuntimeGateView;
static void invoke_actual_owner(bool bridged,JceSessionMode mode)
{
    RuntimeGateView state={0};RuntimeGateView *rt=&state;
    state.net_bridged=bridged;state.clock.alpha=0.375;
    observed_mode=mode;mode_reads=transforms=animators=order=0;
    transform_alpha=animator_alpha=-1;
#include JCE_TEST_RUNTIME_NET_RENDER_OWNER
}
void setUp(void) { mode_reads=transforms=animators=order=0; }
void tearDown(void) {}
static void no_render_steps(unsigned expected_mode_reads)
{
    TEST_ASSERT_EQUAL_UINT(expected_mode_reads,mode_reads);
    TEST_ASSERT_EQUAL_UINT(0,transforms);TEST_ASSERT_EQUAL_UINT(0,animators);
    TEST_ASSERT_EQUAL_UINT(0,order);
}
static void test_unbridged_none_does_not_consume_either_step(void)
{
    invoke_actual_owner(false,JCE_SESSION_MODE_NONE);no_render_steps(0);
}
static void test_unbridged_live_does_not_consume_either_step(void)
{
    invoke_actual_owner(false,JCE_SESSION_MODE_HOST);no_render_steps(0);
}
static void test_bridged_none_does_not_consume_either_step(void)
{
    invoke_actual_owner(true,JCE_SESSION_MODE_NONE);no_render_steps(1);
}
static void test_bridged_live_consumes_transform_then_animator_once(void)
{
    const JceSessionMode modes[]={JCE_SESSION_MODE_HOST,JCE_SESSION_MODE_DEDICATED_SERVER,JCE_SESSION_MODE_CLIENT};
    unsigned i;
    for(i=0;i<sizeof(modes)/sizeof(modes[0]);++i) {
        invoke_actual_owner(true,modes[i]);
        TEST_ASSERT_EQUAL_UINT(1,mode_reads);TEST_ASSERT_EQUAL_UINT(1,transforms);
        TEST_ASSERT_EQUAL_UINT(1,animators);TEST_ASSERT_EQUAL_UINT(12,order);
        TEST_ASSERT_EQUAL_DOUBLE(0.375,transform_alpha);TEST_ASSERT_EQUAL_DOUBLE(0.375,animator_alpha);
    }
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_unbridged_none_does_not_consume_either_step);
    RUN_TEST(test_unbridged_live_does_not_consume_either_step);
    RUN_TEST(test_bridged_none_does_not_consume_either_step);
    RUN_TEST(test_bridged_live_consumes_transform_then_animator_once);
    return UNITY_END();
}
