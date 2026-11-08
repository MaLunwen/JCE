/* The stock SDK host loads the same authored scene as Editor Play. */
#include "_embedded_bundles.h"
#include <jce/application/jce_default_main.inc.h>
#include <jce_script_register_linked.h>
#include "rules.h"
static bool self_test;
static bool init(const JceServices *svc, void *ud)
{
    self_test = getenv("TD_SELF_TEST") != NULL;
    if (self_test) { int result = td_tests(); jce_engine_request_quit(); return result == 0; }
    if (jce_script_register_linked_languages(NULL) < 0) return false;
    return app_init(svc,ud);
}
static void update(float dt, void *ud) { if (!self_test) app_update(dt,ud); }
static void draw(const JceServices *svc, void *ud) { if (!self_test) app_draw(svc,ud); }
static void shutdown(void *ud) { if (!self_test) app_exit(ud); }
static JceAppDesc get_desc(void)
{
    JceAppDesc d = {0}; d.name = "JCE Tower Defense";
    d.init = init; d.update = update; d.draw = draw; d.exit = shutdown;
    d.window_width = 1280; d.window_height = 720; return d;
}
JCE_MAIN(get_desc)
