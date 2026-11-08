/* Public SDK bootstrap. Gameplay lives in the scene-bound Lua scripts. */
#include <jce/application/jce_default_main.inc.h>

static JceAppDesc trigger_graph_lab_desc(void)
{
    JceAppDesc desc = {0};
    desc.name = "Trigger Graph Lab";
    desc.init = app_init;
    desc.update = app_update;
    desc.draw = app_draw;
    desc.exit = app_exit;
    desc.window_width = 1280;
    desc.window_height = 720;
    return desc;
}

JCE_MAIN(trigger_graph_lab_desc)
