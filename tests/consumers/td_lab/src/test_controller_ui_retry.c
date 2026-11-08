/* Complete consumer controller + real view, admitted actual callbacks and
 * counted public ScriptHost ports. No native VM, runtime, GPU or input window.
 * This checks retry scheduling, not pixels or application frame performance. */
#include "view.h"
#include <jce/os/core/jce_json.h>
#include <jce/script_vm/jce_script_vm_c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every optional diagnostic stays off regardless of the launching shell. */
static const char *retry_env(const char *name) { (void)name; return NULL; }
#define getenv retry_env
#ifndef TD_CONTROLLER_SOURCE
#define TD_CONTROLLER_SOURCE "controller.c"
#endif
#include TD_CONTROLLER_SOURCE
#undef getenv

static unsigned checks, failures, cases, invalid_json;
static unsigned calls[TD_WIDGETS][3], accepted[TD_WIDGETS][3];
static int rejected_component=-1,remaining_rejections;
static float viewport_width=1280,viewport_height=720;
static const char *case_name;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    printf("TD_UI_RETRY FAIL case=%s line=%d: %s\n",case_name,__LINE__,#x); return; } } while(0)
static int find_ui(void *user,const char *name,JceScriptEntity *out,int capacity)
{
    (void)user;int index;
    if(capacity<1)return 0;
    if(sscanf(name,"Hud%d",&index)==1 && index>=0 && index<TD_WIDGETS)*out=100+index;
    else *out=!strcmp(name,"MainCamera")?1:(!strcmp(name,"Selection")?2:3);
    return 1;
}
static bool publish(void *user,JceScriptEntity entity,const char *type,const char *json)
{
    (void)user;
    if(entity<100 || entity>=100+TD_WIDGETS)return true;
    int component=!strcmp(type,"UIImage")?0:(!strcmp(type,"UIText")?1:(!strcmp(type,"UIButton")?2:-1));
    if(component<0)return true;
    int slot=(int)entity-100;++calls[slot][component];
    JceJson *patch=jce_json_parse_strict(json,0);
    bool valid=patch && jce_json_is_object(patch);jce_json_free(patch);
    if(!valid){++invalid_json;return false;}
    if(slot==0 && component==rejected_component && remaining_rejections>0) {
        --remaining_rejections;return false;
    }
    ++accepted[slot][component];return true;
}
static void set_text(void *u,JceScriptEntity e,const char *text){(void)u;(void)e;(void)text;}
static void position(void *u,JceScriptEntity e,float x,float y,float z){(void)u;(void)e;(void)x;(void)y;(void)z;}
static void set_enabled(void *u,JceScriptEntity e,const char *type,bool on){(void)u;(void)e;(void)type;(void)on;}
static void quiet_log(void *u,const char *text){(void)u;(void)text;}
static bool key_up(void *u,int key){(void)u;(void)key;return false;}
static bool button_up(void *u,int button){(void)u;(void)button;return false;}
static void no_pointer(void *u,float out[4]){(void)u;memset(out,0,4*sizeof(float));}
static void viewport(void *u,float out[2]){(void)u;out[0]=viewport_width;out[1]=viewport_height;}
static JceScriptEntity no_spawn(void *u,const char *path,float x,float y,float z)
{(void)u;(void)path;(void)x;(void)y;(void)z;return 1000;}
static void no_destroy(void *u,JceScriptEntity e){(void)u;(void)e;}
static void reset_counts(void){memset(calls,0,sizeof calls);memset(accepted,0,sizeof accepted);}
static unsigned count_requests(void)
{
    unsigned count=0;for(int i=0;i<TD_WIDGETS;++i)for(int c=0;c<3;++c)count+=calls[i][c];return count;
}
static bool open_controller(void)
{
    if(app.used)destroy(&app);
    JceScriptHost host={0};JceCScriptContext context={0};
    host.find_by_name=find_ui;host.comp_set_json=publish;host.ui_set_text=set_text;
    host.set_position=position;host.set_rotation=position;host.set_component_enabled=set_enabled;
    host.is_key_down=key_up;host.pointer_button=button_up;host.pointer_position=no_pointer;
    host.viewport_size=viewport;host.spawn=no_spawn;host.destroy_entity=no_destroy;host.log=quiet_log;
    viewport_width=1280;viewport_height=720;rejected_component=-1;remaining_rejections=0;
    context.host=&host;context.host_size=sizeof host;
    reset_counts();return create(&context)==&app;
}
static void retry_after_menu_update(int component)
{
    static TdGame before;
    case_name="game_next_update";++cases;
    CHECK(open_controller());CHECK(on_start(&app)==JCE_C_OK);
    CHECK(on_update(&app,0)==JCE_C_OK); /* Normalize first last_second dirtiness. */
    CHECK(app.menu==1 && app.rules.frames==0);
    /* Enter the real game: slot 0 changes from full-window menu artwork to
     * an actionable Back button, so all three component requests are real. */
    command(2);CHECK(app.menu==0);before=app.rules;
    reset_counts();rejected_component=component;remaining_rejections=1;
    CHECK(on_update(&app,0)==JCE_C_OK);CHECK(remaining_rejections==0);
    CHECK(calls[0][component]==1 && accepted[0][component]==0);
    reset_counts();CHECK(on_update(&app,0)==JCE_C_OK);
    CHECK(calls[0][component]==1 && accepted[0][component]==1);
    CHECK(count_requests()==1);CHECK(memcmp(&before,&app.rules,sizeof before)==0);
    reset_counts();CHECK(on_update(&app,0)==JCE_C_OK);CHECK(count_requests()==0);
}
static void retry_after_viewport_message(int component)
{
    static TdGame before;
    case_name="viewport_next_update";++cases;
    CHECK(open_controller());CHECK(on_start(&app)==JCE_C_OK);
    CHECK(on_update(&app,0)==JCE_C_OK);before=app.rules;
    viewport_width=1600;viewport_height=900;
    reset_counts();rejected_component=component;remaining_rejections=1;
    CHECK(on_message(&app,"on_viewport_resize",0,NULL)==JCE_C_OK);
    CHECK(remaining_rejections==0);CHECK(calls[0][component]==1 && accepted[0][component]==0);
    reset_counts();CHECK(on_update(&app,0)==JCE_C_OK);
    CHECK(calls[0][component]==1 && accepted[0][component]==1);CHECK(count_requests()==1);
    CHECK(memcmp(&before,&app.rules,sizeof before)==0);
    reset_counts();CHECK(on_update(&app,0)==JCE_C_OK);CHECK(count_requests()==0);
}
static void retry_after_initial_publish(int component)
{
    case_name="initial_next_update";++cases;
    CHECK(open_controller());rejected_component=component;remaining_rejections=1;
    CHECK(on_start(&app)==JCE_C_OK);CHECK(remaining_rejections==0);
    CHECK(calls[0][component]==1 && accepted[0][component]==0);
    reset_counts();CHECK(on_update(&app,0)==JCE_C_OK);
    CHECK(calls[0][component]==1 && accepted[0][component]==1);CHECK(count_requests()==1);
    reset_counts();CHECK(on_update(&app,0)==JCE_C_OK);CHECK(count_requests()==0);
}
int main(void)
{
    for(int component=0;component<3;++component)retry_after_menu_update(component);
    for(int component=0;component<2;++component)retry_after_viewport_message(component);
    for(int component=0;component<3;++component)retry_after_initial_publish(component);
    if(app.used)destroy(&app);
    if(invalid_json){++failures;printf("TD_UI_RETRY FAIL invalid_json=%u\n",invalid_json);}
    printf("TD_UI_RETRY %s cases=%u checks=%u failures=%u scope=actual_controller_view_callbacks_no_vm_gpu\n",
           failures?"FAIL":"PASS",cases,checks,failures);
    return failures?1:0;
}
