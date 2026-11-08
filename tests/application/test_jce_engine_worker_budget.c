/* Actual public headless engine boot publishes before its first lazy schedulers. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "unity.h"
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_config.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_thread.h>
#include <jce/renderer/jce_renderer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef JCE_TEST_WORKER_OUTPUT
#error JCE_TEST_WORKER_OUTPUT must name the private ignored fixture directory
#endif
const unsigned char assets_pak_data[1]={0};
const size_t assets_pak_data_size=0;
typedef struct {
    unsigned init_calls;
    int frame_workers,published_class,pin;
    uint32_t async_workers;
    bool headless,fail_init;
} Boot;
static Boot boot;
static char config_path[1024];

static void low_override(const char *value)
{
#if JCE_PLATFORM_WINDOWS
    (void)_putenv_s("JCE_LOW_MEM",value);
#else
    if (value[0]) (void)setenv("JCE_LOW_MEM",value,1);
    else (void)unsetenv("JCE_LOW_MEM");
#endif
}
void setUp(void) { memset(&boot,0,sizeof boot); }
void tearDown(void)
{
    low_override("");
    jce_engine_set_config_path(NULL);
    (void)jce_fs_host_remove_file(config_path);
}
static bool init(const JceServices *services,void *user)
{
    Boot *state=user;
    ++state->init_calls;
    state->headless=services && !services->window && !services->audio &&
        jce_renderer_is_headless(services->renderer);
    state->published_class=jce_config_machine_class();
    state->pin=jce_config_job_workers();
    JceAsyncExecutorStats stats={0};
    JceAsyncExecutor *executor=jce_async_default_executor();
    if (!executor) return false;
    jce_async_executor_get_stats(executor,&stats);
    state->async_workers=stats.worker_count;
    JceThreadPool *pool=jce_thread_pool_shared();
    if (!pool) return false;
    state->frame_workers=jce_thread_pool_worker_count(pool);
    return state->headless && !state->fail_init;
}
static bool run_boot(const char *name,const char *override,const char *machine,
                     int workers,bool fail_init,int *authored_class,int *authored_pin)
{
    int n=snprintf(config_path,sizeof config_path,"%s/%s.ini",JCE_TEST_WORKER_OUTPUT,name);
    TEST_ASSERT_TRUE(n>0 && (size_t)n<sizeof config_path);
    char content[256];
    n=snprintf(content,sizeof content,
        "[renderer]\nbackend=noop\n[performance]\nmachine_class=%s\njob_workers=%d\n",
        machine,workers);
    TEST_ASSERT_TRUE(n>0 && (size_t)n<sizeof content);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(config_path,content,(uint64_t)n));
    low_override(override);
    jce_engine_set_config_path(config_path);
    memset(&boot,0,sizeof boot);
    boot.fail_init=fail_init;
    JceAppDesc app={0};
    app.name="worker-budget-public-headless";
    app.headless=true;
    app.init=init;
    app.user_data=&boot;
    jce_engine_set_app_desc(&app);
    char arg0[]="test_jce_engine_worker_budget";
    char *argv[]={arg0,NULL};
    JceEngine *engine=jce_engine_create(1,argv);
    bool created=engine!=NULL;
    if (engine) jce_engine_destroy(engine);
    JceConfig authored=jce_config_defaults();
    TEST_ASSERT_TRUE(jce_config_load(&authored,config_path));
    *authored_class=authored.machine_class;*authored_pin=authored.job_workers;
    return created;
}
static void check_budget(int frame,int async)
{
    TEST_ASSERT_EQUAL_UINT(1,boot.init_calls);
    TEST_ASSERT_TRUE(boot.headless);
#if JCE_PLATFORM_WEB
    TEST_ASSERT_EQUAL_INT(0,boot.frame_workers);
    TEST_ASSERT_EQUAL_UINT32(0,boot.async_workers);
#else
    TEST_ASSERT_EQUAL_INT(frame,boot.frame_workers);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)async,boot.async_workers);
#endif
}
static void low_override_reaches_first_executor_and_pool_without_rewriting_full(void)
{
    int author,pin;
    bool created=run_boot("low","1","full",0,false,&author,&pin);
    TEST_ASSERT_TRUE(created);
    check_budget(1,1);
    TEST_ASSERT_EQUAL_INT(JCE_MACHINE_CLASS_FULL,boot.published_class);
    TEST_ASSERT_EQUAL_INT(JCE_MACHINE_CLASS_FULL,author);
    TEST_ASSERT_EQUAL_INT(0,pin);
}
static void full_override_clears_low_history_without_rewriting_low(void)
{
    int author,pin;
    jce_config_publish_perf(JCE_MACHINE_CLASS_LOW,3);
    bool created=run_boot("full","0","low",0,false,&author,&pin);
    TEST_ASSERT_TRUE(created);
    check_budget(jce_thread_pool_default_workers(),jce_thread_pool_default_workers());
    TEST_ASSERT_EQUAL_INT(JCE_MACHINE_CLASS_LOW,boot.published_class);
    TEST_ASSERT_EQUAL_INT(JCE_MACHINE_CLASS_LOW,author);
    TEST_ASSERT_EQUAL_INT(0,pin);
}
static void explicit_pin_overrides_default_low_budget(void)
{
    int author,pin;
    bool created=run_boot("pin","1","auto",4,false,&author,&pin);
    TEST_ASSERT_TRUE(created);
    check_budget(4,4);
    TEST_ASSERT_EQUAL_INT(JCE_MACHINE_CLASS_AUTO,boot.published_class);
    TEST_ASSERT_EQUAL_INT(JCE_MACHINE_CLASS_AUTO,author);
    TEST_ASSERT_EQUAL_INT(4,pin);
}
static void failed_init_releases_schedulers_before_next_explicit_boot(void)
{
    int author,pin;
    bool created=run_boot("failed","1","full",0,true,&author,&pin);
    TEST_ASSERT_FALSE(created);
    check_budget(1,1);
    created=run_boot("retry","0","full",3,false,&author,&pin);
    TEST_ASSERT_TRUE(created);
    check_budget(3,3);
    TEST_ASSERT_EQUAL_INT(JCE_MACHINE_CLASS_FULL,boot.published_class);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(low_override_reaches_first_executor_and_pool_without_rewriting_full);
    RUN_TEST(full_override_clears_low_history_without_rewriting_low);
    RUN_TEST(explicit_pin_overrides_default_low_budget);
    RUN_TEST(failed_init_releases_schedulers_before_next_explicit_boot);
    return UNITY_END();
}
