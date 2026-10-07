/* Application glue only: the shared scene scripts own all gameplay. */
#include "_embedded_bundles.h"
#include <jce/application/jce_default_main.inc.h>
#include <jce/api_llm.h>
#include <jce/api_core.h>
#include <jce/script_vm/jce_script_vm_c.h>
#include <jce_script_register_linked.h>
#include "controller.h"
#include "board.h"
#include <math.h>

extern const JceCModuleDesc *mines_module(void);
static const JceServices *services;
static JcePlatformServices *records;
static JceLlmHandle ai_handle;
static char ai_work[1024];
static bool self_test;
#define REPORT(...) jce_log_write(JCE_LOG_LEVEL_INFO,"mines.app",__FILE__,__LINE__,__VA_ARGS__)
static int64_t get_best(const char *key) {return jce_platform_stat_get(records,key,0);}
static int put_best(const char *key,int64_t value) {
    return records && jce_platform_stat_set(records,key,value) && jce_platform_flush(records);
}
static void publish(void) {jce_script_vm_cpp_add_module(mines_module());}
static bool init(const JceServices *svc,void *ud) {
    const char *base=getenv("MINES_SAVE_DIR");char path[1024];
    services=svc;self_test=getenv("MINES_SELF_TEST")!=NULL;
    if(self_test) {int result=mines_self_test();jce_engine_request_quit();return result==0;}
    if(!base || !base[0]) {
        base=getenv("LOCALAPPDATA");if(!base || !base[0]) base=getenv("HOME");
        if(!base || !base[0]) base=".";
        snprintf(path,sizeof path,"%s/JCE/Minesweeper",base);base=path;
    }
    records=jce_platform_init_local(base);
    snprintf(ai_work,sizeof ai_work,"%s/ai",base);jce_fs_host_create_directory(ai_work);
    mines_records_backend(get_best,put_best);
    jce_script_register_linked_languages(publish);
    return app_init(svc,ud);
}
static void request_ai(void) {
    const char *python=getenv("MINES_AI_PYTHON"),*tool=getenv("MINES_AI_TOOL");
    char args[2048];JceLlmRequest req={0};
    if(ai_handle) return;
    if(!python || !tool || strchr(tool,'"')) {REPORT("MINES_AI_UNAVAILABLE: launch through tools/run.py or configure MINES_AI_PYTHON and MINES_AI_TOOL");return;}
    snprintf(args,sizeof args,"\"%s\" --brief-file \"{prompt}\" --out \"{response}\" --max-tokens 512 --send",tool);
    req.provider.executable=python;req.provider.arguments=args;req.provider.timeout_ms=90000;
    req.prompt="Create a calm blue Minesweeper challenge: width 10, height 10, mines 12, red 24, green 64, blue 45. Return the six requested integers as JSON.";
    req.work_dir=ai_work;
    ai_handle=jce_llm_submit(&req);
    REPORT("MINES_AI_SUBMIT handle=%u error=%s",ai_handle,jce_llm_last_error());
}
static void tick_ai(void) {
    JceLlmProgress progress;
    jce_llm_tick();
    if(!ai_handle || !jce_llm_poll(ai_handle,&progress) || progress.status==JCE_LLM_RUNNING) return;
    if(progress.status==JCE_LLM_DONE) {
        size_t length=0;const char *text=jce_llm_response(ai_handle,&length);
        JceJson *j=text && length<=4096?jce_json_parse_strict(text,length):NULL;
        int w=jce_json_get_int(j,"width",0),h=jce_json_get_int(j,"height",0),m=jce_json_get_int(j,"mines",0);
        int r=jce_json_get_int(j,"red",-1),g=jce_json_get_int(j,"green",-1),b=jce_json_get_int(j,"blue",-1);
        if(j && w>=5 && w<=30 && h>=5 && h<=16 && m>=1 && m<=w*h-9 && r>=0 && r<=255 && g>=0 && g<=255 && b>=0 && b<=255) {
            mines_apply_ai(w,h,m,r,g,b);
            REPORT("MINES_AI_APPLY proposal=%dx%d/%d rgb=%d,%d,%d; Lua validates and publishes components",w,h,m,r,g,b);
        } else REPORT("MINES_AI_REJECT invalid response; live board unchanged");
        jce_json_free(j);
    } else REPORT("MINES_AI_FAILED %s",progress.message?progress.message:"");
    jce_llm_release(ai_handle);ai_handle=0;
}
static void update(float dt,void *ud) {
    float x,y,scale;uint32_t w,h;int b;
    if(self_test) return;
    app_update(dt,ud);
    jce_input_mouse_pos(services->input,&x,&y);jce_window_get_size(services->window,&w,&h);
    scale=sqrtf(((float)w/1280.0f)*((float)h/720.0f));
    if(scale>0) for(b=2;b<=3;++b) if(jce_input_mouse_button_pressed(services->input,b)) mines_pointer_action(x/scale,y/scale,b);
    if(mines_take_ai_request()) request_ai();
    if(mines_take_quit_request()) jce_engine_request_quit();
    tick_ai();
}
static void draw(const JceServices *svc,void *ud) {if(!self_test) app_draw(svc,ud);}
static void shutdown(void *ud) {
    jce_llm_shutdown();ai_handle=0;
    if(!self_test) app_exit(ud);
    mines_records_backend(NULL,NULL);jce_platform_shutdown(records);records=NULL;
}
static JceAppDesc mines_get_desc(void) {
    JceAppDesc d={0};d.name="Minesweeper";d.init=init;d.update=update;d.draw=draw;d.exit=shutdown;
    d.window_width=1280;d.window_height=720;return d;
}
JCE_MAIN(mines_get_desc)
