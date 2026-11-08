#include "view.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/platform/jce_host_shell.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_log.h>
#include <jce/script_vm/jce_script_vm_c.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef TD_QA_CENSUS_OWNER
#define TD_QA_CENSUS_OWNER "qa_update_census.inc.h"
#endif
#include TD_QA_CENSUS_OWNER
typedef struct {
    TdGame rules; TdView view; JceScriptHost host;
    int active_cursor;
    bool used, keys[512], dragging, ui_dirty;
    int menu, selected, last_second, last_kills, last_hits, trace_frame;
    float drag_distance, touch_drag_distance, pointer[4], touch_xy[3][2], arrow_clock[4];
    uint64_t wall_start, wall_last;
    int touches, fixture_ticks, drag_button; char notice[160];
} TdApp;
static TdApp app;
static void say(const char *format, ...)
{
    char buf[1600]; va_list args;
    va_start(args,format); vsnprintf(buf,sizeof buf,format,args); va_end(args);
    const char *trace_requested = getenv("TD_TRACE");
    const char *timing_requested = getenv("TD_TIMING");
    if ((trace_requested && trace_requested[0] && trace_requested[0] != '0') ||
        (timing_requested && timing_requested[0] && timing_requested[0] != '0'))
        LOG_WARN("td_lab", "%s", buf);
    else if (app.host.log) app.host.log(app.host.user,buf);
}
static void trace(const char *action)
{
    int i, enemies = 0, towers = 0, bullets = 0;
    if (!getenv("TD_TRACE")) return;
    for (i = 0; i < TD_ENEMIES; ++i) enemies += app.rules.enemies[i].active;
    for (i = 0; i < TD_CELLS; ++i) towers += app.rules.towers[i].active;
    for (i = 0; i < TD_BULLETS; ++i) bullets += app.rules.bullets[i].active;
    say("TD_TRACE action=%s menu=%d selected=%d time=%.6f resources=%d score=%d lives=%d wave=%d towers=%d enemies=%d bullets=%d spawned=%d shots=%d hits=%d kills=%d escaped=%d hash=%llu",
        action,app.menu,app.selected,app.rules.time,app.rules.resources,app.rules.score,
        app.rules.lives,app.rules.wave,towers,enemies,bullets,app.rules.spawned,
        app.rules.shots,app.rules.hits,app.rules.kills,app.rules.escaped,
        (unsigned long long)td_hash(&app.rules));
    { float p[3]; app.host.get_position(app.host.user,app.view.camera,p);
        say("TD_CAMERA x=%.6f y=%.6f z=%.6f yaw=%.6f pitch=%.6f targetX=%.6f targetY=%.6f targetZ=%.6f width=%.0f height=%.0f",p[0],p[1],p[2],app.view.yaw,app.view.pitch,app.view.pan_x,app.view.pan_y,app.view.pan_z,app.view.width,app.view.height);
    }
    if(!strcmp(action,"destroy")) {
        for(i=0;i<TD_CELLS;++i) if(app.rules.towers[i].active)
            say("TD_TOWER cell=%d kind=%d",i,app.rules.towers[i].kind);
        for(i=0;i<TD_BULLETS;++i) if(app.rules.bullets[i].active) {
            const TdBullet *b=&app.rules.bullets[i];
            say("TD_BULLET slot=%d kind=%d x=%.6f y=%.6f z=%.6f p=%.6f",i,b->kind,b->position.x,b->position.y,b->position.z,b->progress);
        }
    }
    if (enemies) {
        for (i = 0; i < TD_ENEMIES; ++i) if (app.rules.enemies[i].active) {
            const TdEnemy *e = &app.rules.enemies[i];
            say("TD_POSE enemy=%d kind=%d hp=%d cell=%d x=%.6f y=%.6f z=%.6f p=%.6f",
                i,e->kind,e->hp,e->cell,e->position.x,e->position.y,e->position.z,e->progress);
            break;
        }
    }
}
static void update_cursor(const float sample[4])
{
    int desired=-1;
    if(sample[2]>0 && sample[3]>0 && sample[0]>=0 && sample[1]>=0 &&
       sample[0]<sample[2] && sample[1]<sample[3])
        desired=td_view_ui_action(&app.view,sample[0],sample[1])?1:0;
    if(desired!=app.active_cursor) {
        if(app.host.cursor_set) app.host.cursor_set(app.host.user,desired<0?NULL:
            desired?"textures/cursor-active.png":"textures/cursor-default.png",5,1);
        app.active_cursor=desired;
    }
}
static void start(void)
{
    const char *seed = getenv("TD_SEED");
    int resources=app.rules.resources,score=app.rules.score,lives=app.rules.lives;
    td_reset(&app.rules,seed?(uint32_t)strtoul(seed,NULL,10):7);
    if(getenv("TD_LEGACY_FRAME_MODE")) {
        app.rules.wall_epoch_ms=(double)jce_time_now_epoch_milliseconds();
        app.rules.previous_time_ms=app.rules.wall_epoch_ms;
    }
    app.rules.resources=resources; app.rules.score=score; app.rules.lives=lives;
    if(getenv("TD_RANDOM_TAPE")) {
        uint64_t size=0; char *text=(char *)jce_fs_host_read_all(getenv("TD_RANDOM_TAPE"),&size);
        if(text) {
            char *end=text; uint64_t offset=0;
            while(offset<size && app.rules.random_count<4096) {
                char *next; double value=strtod(end,&next);
                if(next==end || !isfinite(value) || value<0 || value>=1) break;
                app.rules.random_samples[app.rules.random_count++]=value;
                end=next;offset=(uint64_t)(end-text);
            }
            jce_fs_buffer_free(text);
            say("TD_RANDOM recorded_input_samples=%d",app.rules.random_count);
        }
    }
    app.view.distance=sqrtf(22100); app.view.yaw=atan2f(60,110);
    app.view.pitch=atan2f(80,sqrtf(15700));
    app.view.pan_x=app.view.pan_y=app.view.pan_z=0; td_view_camera(&app.view);
    app.wall_start=(getenv("TD_TIMING") || getenv("TD_LEGACY_FRAME_MODE"))?
        jce_time_perf_counter():0;
    app.wall_last=app.wall_start;
    app.menu = 0; app.selected = -1; app.last_second = -1; app.last_hits = app.last_kills = 0;
    snprintf(app.notice,sizeof app.notice,"Select a tile to build your defense.");
    app.ui_dirty = true; trace("start");
}
static void command(int action)
{
    if (action == 1) app.menu = 2;
    else if (action == 2 || action == 6) start();
    else if (action == 3) app.menu = app.menu == 3?1:3;
    else if (action == 4) {
        int resources=app.rules.resources,score=app.rules.score,lives=app.rules.lives;
        td_reset(&app.rules,app.rules.rng); app.rules.resources=resources;
        app.rules.score=score; app.rules.lives=lives;
        app.menu=app.menu?1:2; app.selected=-1; trace("menu");
    }
    else if (action==30 || action==31) {
        const char *url=action==30?"http://www.fellicht.nl":"http://pracalic.ugu.pl";
        say("TD_LINK %s",url); (void)jce_host_open_url(url); return;
    }
    else if (app.menu) return;
    else if (action >= 1000 && action < 1000+TD_CELLS) {
        int cell=action-1000;
        app.selected=(cell==0 || cell==120 || cell==app.selected)?-1:cell; trace("select");
        snprintf(app.notice,sizeof app.notice,"Tile (%d, %d)",app.selected/11,app.selected%11);
    } else if (action >= 10 && action <= 12) {
        int kind = action-10;
        if (td_place(&app.rules,app.selected,kind)) {
            trace("place"); app.selected = -1;
            snprintf(app.notice,sizeof app.notice,"Tower placed. Enemy routes recalculated.");
        } else {
            snprintf(app.notice,sizeof app.notice,"Cannot build: occupied, insufficient gold, or blocked route.");
            trace("place-rejected");
        }
    } else if (action == 5) {
        if (td_wave(&app.rules)) { trace("wave"); app.notice[0] = 0; }
        else snprintf(app.notice,sizeof app.notice,"Both waves are running or already completed.");
    } else if (action == 7 || action == 8) { td_spawn(&app.rules,action == 8); trace("spawn"); }
    else if (action == 9) { app.selected = -1; app.notice[0] = 0; }
    app.ui_dirty = true;
}
static JceCStatus ui_click(const JceCScriptContext *ctx, JceScriptEntity e)
{
    float xyz[3];
    if (app.used && ctx->host->get_position(ctx->host->user,e,xyz)) command((int)xyz[0]);
    return JCE_C_OK;
}
static void *create(const JceCScriptContext *ctx)
{
    size_t size;
    if (!ctx || !ctx->host) return NULL;
    if (app.used) {
        if (td_qa_census.enabled) td_qa_census.invalidated=1;
        return NULL;
    }
    memset(&app,0,sizeof app); size = ctx->host_size < sizeof app.host?ctx->host_size:sizeof app.host;
    memcpy(&app.host,ctx->host,size);
    if (!app.host.comp_set_json || !app.host.ui_set_text || !app.host.find_by_name ||
        !app.host.is_key_down || !app.host.pointer_button || !app.host.set_rotation ||
        !app.host.pointer_position) return NULL;
    app.used = true;
    const char *census=getenv("TD_UPDATE_CENSUS");
    td_qa_update_census_begin(census && !strcmp(census,"1"));
    return &app;
}
static void destroy(void *self)
{
    (void)self; td_qa_census.alive=0; trace("destroy");
    if(app.host.cursor_set) app.host.cursor_set(app.host.user,NULL,0,0);
    memset(&app,0,sizeof app);
}
static bool sync_viewport(void)
{
    float size[2] = {0};
    if (!app.host.viewport_size) return false;
    app.host.viewport_size(app.host.user,size);
    if (!isfinite(size[0]) || !isfinite(size[1]) || size[0]<=0 || size[1]<=0 ||
        (size[0]==app.view.width && size[1]==app.view.height)) return false;
    app.view.width=size[0]; app.view.height=size[1]; app.ui_dirty=true;
    return true;
}
static JceCStatus on_start(void *self)
{
    (void)self;
    if (!td_view_init(&app.view,&app.host)) return "Tower Defense scene pool is incomplete";
    (void)sync_viewport();
    bool default_ok=app.host.cursor_set && app.host.cursor_set(app.host.user,"textures/cursor-default.png",5,1);
    bool active_ok=app.host.cursor_set && app.host.cursor_set(app.host.user,"textures/cursor-active.png",5,1);
    if(app.host.cursor_set) app.host.cursor_set(app.host.user,NULL,0,0);
    app.active_cursor=-1;
    say("TD_CURSOR default=%d active=%d hotspot=5,1",default_ok,active_ok);
    td_reset(&app.rules,7); app.menu = 1; app.selected = -1;
    td_view_camera(&app.view); app.ui_dirty = true; app.last_second = -1;
    {
        const char *fixture=getenv("TD_FIXTURE"), *fixed=getenv("JCE_FRAME_DT_FIXED");
        bool legacy=getenv("TD_LEGACY_FRAME_MODE")!=NULL;
        const char *clock=fixture?
            ((!strcmp(fixture,"ghost") || !strcmp(fixture,"ufo") || !strcmp(fixture,"combat"))?
                "fixture_fixed_60hz":"fixture_frozen"):
            (legacy?(fixed?"legacy_frame_host_dt":"legacy_frame_wall"):
                "runtime_sim_dt");
        say("TD_BOOT language=c runtime=shared scene=td_lab fixed_hz=60 seed=7 clock=%s legacy_frame=%d qa_fixed_dt_requested=%d fixture=%s timing_observer=%d timing_start=%d",
            clock,legacy,fixed && fixed[0],fixture?fixture:"none",
            getenv("TD_TIMING")!=NULL,getenv("TD_TIMING_START")!=NULL);
    }
    if (getenv("TD_AUTOSTART")) start();
    if (getenv("TD_AUTODEFENSE")) {
        command(2); app.selected = 11; command(11); app.selected = 23; command(12); command(5);
    }
    if (getenv("TD_TIMING_START")) { start(); td_spawn(&app.rules,0); }
    if (getenv("TD_STRESS")) {
        int count=atoi(getenv("TD_STRESS"));
        if(count>TD_ENEMIES)count=TD_ENEMIES;
        start(); for(int k=0;k<count;++k)td_spawn(&app.rules,k%2);
    }
    if (getenv("TD_FIXTURE")) {
        const char *f=getenv("TD_FIXTURE"); command(2);
        if(!strcmp(f,"select")) { app.selected=60; app.ui_dirty=true; }
        if(!strcmp(f,"low-money")) { app.rules.resources=0;app.selected=60;app.ui_dirty=true; }
        if(!strcmp(f,"basic")) td_place(&app.rules,11,0);
        if(!strcmp(f,"advanced") || !strcmp(f,"combat")) td_place(&app.rules,11,1);
        if(!strcmp(f,"rocket") || !strcmp(f,"combat")) td_place(&app.rules,23,2);
        if(!strcmp(f,"ghost") || !strcmp(f,"ufo") || !strcmp(f,"combat"))
            td_spawn(&app.rules,!strcmp(f,"ufo"));
    }
    if(getenv("TD_TRACE")) {
        const char *names[]={"Ground","Start","Sun"}; int n;
        for(n=0;n<3;++n) {
            JceScriptEntity entity=0; char *json;
            app.host.find_by_name(app.host.user,names[n],&entity,1);
            json=app.host.comp_get_json(app.host.user,entity,n==2?"Light":"MeshRenderer");
            if(json) { say("TD_MATERIAL %s %s",names[n],json); app.host.json_free(app.host.user,json); }
        }
        /* The generic logger bounds each message to 512 bytes. Preserve the
         * actual public render snapshot without truncating QA evidence. */
        { char *json=app.host.render_get_json(app.host.user);
          if(json) {
              const size_t size=strlen(json), chunk=384;
              const size_t count=(size+chunk-1)/chunk;
              for(size_t part=0;part<count;++part) {
                  size_t left=size-part*chunk;
                  say("TD_RENDER_CHUNK part=%u total=%u bytes=%u payload=%.*s TD_RENDER_END",
                      (unsigned)part,(unsigned)count,(unsigned)size,
                      (int)(left<chunk?left:chunk),json+part*chunk);
              }
              app.host.json_free(app.host.user,json);
          }
        }
    }
    /* Explicit diagnostic controls preserve captures of individual render paths. */
    if(getenv("TD_DIAG")) {
        JceScriptEntity e=0; const char *mode=getenv("TD_DIAG");
        if(!strcmp(mode,"noshadow")) {
            app.host.find_by_name(app.host.user,"Sun",&e,1);
            app.host.comp_set_json(app.host.user,e,"Light","{\"castsShadow\":false}");
        } else if(!strcmp(mode,"unlit")) {
            app.host.find_by_name(app.host.user,"Ground",&e,1);
            app.host.comp_set_json(app.host.user,e,"MeshRenderer","{\"materialPath\":\"materials/unlit.mat.json\"}");
        } else if(!strcmp(mode,"nomarkers")) {
            app.host.find_by_name(app.host.user,"Start",&e,1);
            app.host.comp_set_json(app.host.user,e,"MeshRenderer","{\"visible\":false}");
            app.host.find_by_name(app.host.user,"End",&e,1);
            app.host.comp_set_json(app.host.user,e,"MeshRenderer","{\"visible\":false}");
        }
    }
    app.ui_dirty=!td_view_ui(&app.view,&app.rules,app.menu,app.selected,app.notice);
    trace("boot"); return JCE_C_OK;
}
static JceCStatus on_update(void *self, float dt)
{
    /* Only a valid actual callback can earn normal-return consumption. */
    if (td_qa_census.enabled && (!app.used || self!=&app || !app.host.pointer_position)) {
        td_qa_census.invalidated=1;
        return "Invalid TD update census host";
    }
    const uint32_t qa_generation=td_qa_update_census_enter();
    static const int bindings[][2]={{30,10},{31,11},{32,12},{26,5},{8,7},{24,8},{41,9},{6,9}};
    int i,button=0; bool down,changed=false; float delta[2]={0},wheel=0,sample[4]={0}; (void)self;
    changed=sync_viewport();
    app.host.pointer_position(app.host.user,sample);
    if(getenv("TD_LEGACY_FRAME_MODE"))
        app.rules.wall_epoch_ms=(double)jce_time_now_epoch_milliseconds();
    if(sample[2]>0 && sample[3]>0) {
        memcpy(app.pointer,sample,sizeof sample);
    }
    /* A focus-loss clear is not a user key-up or tile mouse-up. */
    if(sample[2]<=0 || sample[3]<=0) {
        memset(app.keys,0,sizeof app.keys); app.dragging=false; app.touches=0;
    }
    for(i=0;i<(int)(sizeof bindings/sizeof bindings[0]);++i) {
        int key=bindings[i][0]; down=app.host.is_key_down(app.host.user,key);
        if(!down && app.keys[key] && !app.menu) command(bindings[i][1]);
        app.keys[key]=down;
    }
    if(getenv("TD_EXTENDED")) {
        down=app.host.is_key_down(app.host.user,40);
        if(!down && app.keys[40]) command(2); app.keys[40]=down;
        down=app.host.is_key_down(app.host.user,21);
        if(!down && app.keys[21]) { td_reset(&app.rules,7); command(2); }
        app.keys[21]=down;
    }
    for(i=1;i<=3;++i) if(app.host.pointer_button(app.host.user,i)) {button=i; break;}
    down=button!=0;
    if(down && !app.dragging) { app.drag_distance=0; app.drag_button=button; }
    if(app.host.pointer_delta) app.host.pointer_delta(app.host.user,delta);
    if(app.host.pointer_wheel) wheel=app.host.pointer_wheel(app.host.user);
    if(!app.menu && sample[2]>0 && ((down && (delta[0] || delta[1])) || wheel)) {
        if(down) {
            app.drag_distance+=fabsf(delta[0])+fabsf(delta[1]);
            if(button==1) {
                app.view.yaw-=delta[0]*2*3.14159265358979323846f/app.view.width;
                app.view.pitch+=delta[1]*2*3.14159265358979323846f/app.view.height;
            } else if(button==3) td_view_pan(&app.view,delta[0],delta[1]);
            else if(delta[1]) app.view.distance*=delta[1]>0?1/0.95f:0.95f;
        }
        if(wheel) app.view.distance*=wheel>0?0.95f:1/0.95f;
        changed=true;
    }
    if(!down && app.dragging && app.drag_button==1 && app.drag_distance<3 && !app.menu && sample[2]>0 &&
        !td_view_ui_contains(&app.view,sample[0],sample[1])) {
        int picked=td_view_pick(&app.view,sample[0],sample[1]);
        if(picked>=0) command(1000+picked); else command(9);
    }
    /* The original canvas drag finishes by hiding its build selection. */
    if(!down && app.dragging && app.drag_button==1 && app.drag_distance>=3 &&
        !app.menu && sample[2]>0) command(9);
    app.dragging=down;
    for(i=0;i<4;++i) {
        int key=79+i; down=app.host.is_key_down(app.host.user,key);
        if(!app.menu && down) {
            app.arrow_clock[i]-=dt;
            if(!app.keys[key] || app.arrow_clock[i]<=0) {
                td_view_pan(&app.view,i==0?-7:(i==1?7:0),i==2?-7:(i==3?7:0));
                app.arrow_clock[i]=app.keys[key]?1/30.0f:0.4f; changed=true;
            }
        }
        app.keys[key]=down;
    }
    if(!app.menu && app.host.touch_count && app.host.touch_get) {
        int count=app.host.touch_count(app.host.user); float xy[3][2]={{0}};
        if(count>3) count=3;
        for(i=0;i<count;++i) {
            uint64_t id; float pressure;
            app.host.touch_get(app.host.user,i,&id,&xy[i][0],&xy[i][1],&pressure);
            xy[i][0]*=app.view.width; xy[i][1]*=app.view.height;
        }
        if(count==1 && app.touches==0) app.touch_drag_distance=0;
        if(count>1 || app.touches>1) app.touch_drag_distance=3;
        if(count && count==app.touches) {
            if(count==1) {
                app.touch_drag_distance+=fabsf(xy[0][0]-app.touch_xy[0][0])+
                                         fabsf(xy[0][1]-app.touch_xy[0][1]);
                app.view.yaw-=(xy[0][0]-app.touch_xy[0][0])*2*3.14159265358979323846f/app.view.width;
                app.view.pitch+=(xy[0][1]-app.touch_xy[0][1])*2*3.14159265358979323846f/app.view.height;
            } else if(count==2) {
                float before=hypotf(app.touch_xy[0][0]-app.touch_xy[1][0],app.touch_xy[0][1]-app.touch_xy[1][1]);
                float now=hypotf(xy[0][0]-xy[1][0],xy[0][1]-xy[1][1]);
                if(now!=before) app.view.distance*=now>before?0.95f:1/0.95f;
            } else td_view_pan(&app.view,xy[0][0]-app.touch_xy[0][0],xy[0][1]-app.touch_xy[0][1]);
            changed=true;
        }
        if(!count && app.touches==1 && app.touch_drag_distance<3 &&
            !td_view_ui_contains(&app.view,app.touch_xy[0][0],app.touch_xy[0][1])) {
            int picked=td_view_pick(&app.view,app.touch_xy[0][0],app.touch_xy[0][1]);
            if(picked>=0) command(1000+picked); else command(9);
        }
        app.touches=count; memcpy(app.touch_xy,xy,sizeof xy);
    }
    if(changed) {
        app.view.pitch=fmaxf(3.14159265358979323846f/2-1.5f,fminf(3.14159265358979323846f/2-0.000001f,app.view.pitch));
        app.view.distance=fmaxf(30,fminf(300,app.view.distance));
        td_view_camera(&app.view); trace("camera");
    }
    {
        float legacy_dt=dt;
        if(getenv("TD_LEGACY_FRAME_MODE")) {
            uint64_t now=jce_time_perf_counter(),freq=jce_time_perf_freq();
            if(!getenv("JCE_FRAME_DT_FIXED") && app.wall_last && freq && now>=app.wall_last)
                legacy_dt=(float)((double)(now-app.wall_last)/(double)freq);
            app.wall_last=now; /* Only the explicit source-frame diagnostic owns wall time. */
        }
        if(!app.menu) {
            const char *fixture=getenv("TD_FIXTURE");
            if(!fixture) {
                if(getenv("TD_LEGACY_FRAME_MODE")) td_frame(&app.rules,legacy_dt);
                /* The runtime already applied pause and time scale. Do not
                 * replace its simulation interval with a second wall clock. */
                else td_tick(&app.rules,dt);
            }
            else if(dt>0 && (!strcmp(fixture,"ghost") || !strcmp(fixture,"ufo") ||
                    !strcmp(fixture,"combat")) && app.fixture_ticks<120) {
                td_frame(&app.rules,TD_STEP); ++app.fixture_ticks;
            }
        }
    }
    if ((int)app.rules.time != app.last_second || app.rules.hits != app.last_hits || app.rules.kills != app.last_kills) {
        if (app.rules.kills != app.last_kills) trace("death");
        else if (app.rules.hits != app.last_hits) trace("hit");
        else trace("tick");
        if(getenv("TD_TIMING") && app.wall_start) {
            double wall=(double)(jce_time_perf_counter()-app.wall_start)/(double)jce_time_perf_freq();
            say("TD_TIME wall=%.6f simulation=%.6f progress=%.6f frames=%llu dropped=%.6f remainder=%.6f",wall,app.rules.time,app.rules.enemies[0].progress,(unsigned long long)app.rules.frames,app.rules.dropped_seconds,app.rules.accumulator);
        }
        app.last_second = (int)app.rules.time; app.last_hits = app.rules.hits; app.last_kills = app.rules.kills;
        app.ui_dirty = true;
    }
    td_view_render(&app.view,&app.rules,app.menu,app.selected,app.notice);
    if(app.view.cards_present && app.view.cards_time<.3f) {app.view.cards_time+=dt;app.ui_dirty=true;}
    if (app.ui_dirty) { app.ui_dirty=!td_view_ui(&app.view,&app.rules,app.menu,app.selected,app.notice); }
    update_cursor(sample);
    td_qa_update_census_return(qa_generation);
    return JCE_C_OK;
}
static JceCStatus on_message(void *self,const char *name,double number,const char *text)
{
    (void)self;(void)number;(void)text;
    if (name && !strcmp(name,"on_viewport_resize") && sync_viewport()) {
        td_view_camera(&app.view);
        app.ui_dirty=!td_view_ui(&app.view,&app.rules,app.menu,app.selected,app.notice);
        trace("camera");
    }
    return JCE_C_OK;
}
JCE_C_SCRIPT_CLASS_BEGIN(TdController,"TdController",create,destroy)
    JCE_C_ON_START(on_start)
    JCE_C_ON_UPDATE(on_update)
    JCE_C_ON_MESSAGE(on_message)
JCE_C_SCRIPT_CLASS_END()
JCE_C_SCRIPT_GLOBAL(TdClick,"td_ui_click",ui_click,NULL,NULL)
JCE_C_MODULE_BEGIN()
    JCE_C_MODULE_CLASS(TdController)
JCE_C_MODULE_GLOBALS()
    JCE_C_MODULE_GLOBAL(TdClick)
JCE_C_MODULE_END("td_lab",td_module)
