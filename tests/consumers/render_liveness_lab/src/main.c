/* The shared JCE host owns the window, GPU frame loop and shutdown. */
#include <jce/api_app.h>
#include "fixture.h"
#include <stdlib.h>

JceAppDesc lab_get_desc(void)
{
    const char *config = getenv("JCE_LAB_CONFIG");
    if (config && config[0]) jce_engine_set_config_path(config);
    JceAppDesc desc = {0};
    desc.name = "JCE Public SDK Render Liveness Lab";
    desc.init = lab_init;
    desc.draw = lab_draw;
    desc.exit = lab_exit;
    desc.window_width = 960;
    desc.window_height = 540;
    return desc;
}
JCE_MAIN(lab_get_desc)
