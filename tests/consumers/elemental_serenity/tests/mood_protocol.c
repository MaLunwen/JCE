/* Executes current project Lua through the installed public SDK, without GPU.
 * Counted host ports are protocol controls, not renderer/visual parity proof. */
#include <jce/api.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
typedef struct {
    JceScript *script;
    JceScriptInstance director;
    float position[8][3];
    char status[512];
    int messages, errors;
    bool drop_receiver, missing_ack;
    bool camera_case, camera_entity, camera_component, camera_reference;
    bool camera_down, camera_active;
    float camera_dx, camera_dy, camera_wheel;
    double camera_pose[6], camera_fov;
    int camera_priority, camera_gets, camera_sets, camera_checks, camera_creates;
} Probe;
static const char *NAMES[] = {"", "MoodRequest", "MoodPlan", "MoodAck", "MoodProvider", "MoodStatus", "Director", "BeautyCam"};
static int failures;
#define CHECK(v) do { if (!(v)) { fprintf(stderr,"ES_MOOD_PROTOCOL_FAIL line=%d expression=%s\n",__LINE__,#v); ++failures; } } while (0)
static int find(void *user, const char *name, JceScriptEntity *out, int max)
{
    Probe *p=(Probe *)user; int i;
    for(i=1;i<8;++i) if(strcmp(name,NAMES[i])==0) {
        if(i==7 && (!p->camera_case || !p->camera_entity)) return 0;
        if(i==3 && p->missing_ack) return 0;
        if(max>0) out[0]=(JceScriptEntity)i;
        return 1;
    }
    return 0;
}
static bool get(void *user, JceScriptEntity entity, float out[3])
{
    if(entity<1 || entity>=8) return false;
    memcpy(out,((Probe *)user)->position[entity],sizeof(float)*3); return true;
}
static void set(void *user, JceScriptEntity entity, float x,float y,float z)
{
    Probe *p=(Probe *)user;
    if(entity<1 || entity>=8) return;
    p->position[entity][0]=x;p->position[entity][1]=y;p->position[entity][2]=z;
}
static void label(void *user,JceScriptEntity entity,const char *text)
{
    if(entity==5) snprintf(((Probe *)user)->status,512,"%s",text);
}
static void log_line(void *user,const char *text)
{
    Probe *p=(Probe *)user;
    if(strstr(text," error: ") || strstr(text,"compile error")) {++p->errors;fprintf(stderr,"lua: %s\n",text);}
}
static void send(void *user,JceScriptEntity entity,const char *method,double number,const char *string)
{
    Probe *p=(Probe *)user;++p->messages;
    if(entity==6 && !p->drop_receiver)
        jce_script_call_message(p->script,p->director,method,number,string);
}
static char *source(const char *assets,const char *name)
{
    char path[2048];FILE *file;long size;char *bytes;
    snprintf(path,sizeof path,"%s/scripts/%s",assets,name);
    file=fopen(path,"rb");if(!file)return NULL;
    if(fseek(file,0,SEEK_END)!=0 || (size=ftell(file))<1 || size>1024*1024 || fseek(file,0,SEEK_SET)!=0){fclose(file);return NULL;}
    bytes=(char *)jce_malloc((size_t)size+1);
    if(!bytes || fread(bytes,1,(size_t)size,file)!=(size_t)size){jce_free(bytes);fclose(file);return NULL;}
    bytes[size]=0;fclose(file);return bytes;
}
static void remove_ack_wire(char *text)
{
    const char *wire="  jce.set_position(ack, request_seq, D.season * 2 + D.tod, 1)";
    char *found=strstr(text,wire);CHECK(found!=NULL);
    if(found)memmove(found,found+strlen(wire),strlen(found+strlen(wire))+1);
}
/* The two UI surfaces share the Director's committed state. Explicitly seed
 * each case so a failure cannot accidentally make a later case pass. */
static void apply_named(Probe *p,JceScriptInstance mood,const char *handler,int state)
{
    CHECK(jce_script_call_named(p->script,handler,0));
    jce_script_call_update(p->script,mood,.01f);
    CHECK(p->position[3][0]==p->position[1][0]);
    CHECK(p->position[3][1]==(float)state && p->position[3][2]==1);
    CHECK(strstr(p->status,"APPLIED | portable preset (native UNAVAILABLE)")!=NULL);
}
static void run_interleaved_controls(Probe *p,JceScriptInstance mood)
{
    static const struct {
        const char *seed, *top, *second;
        int initial, next;
    } cases[] = {
        {"es_mood_spring_day", "es_btn_rain", NULL, 0, 7},
        {"es_mood_rainy_night", "es_btn_spring", NULL, 7, 2},
        {"es_mood_spring_day", "es_btn_autumn", NULL, 0, 5},
        {"es_mood_autumn_day", "es_btn_winter", NULL, 4, 3},
        {"es_mood_spring_night", "es_btn_daynight", NULL, 1, 1},
        {"es_mood_winter_day", "es_btn_rain", "es_btn_daynight", 2, 0},
    };
    for(size_t i=0;i<sizeof cases/sizeof cases[0];++i){
        apply_named(p,mood,cases[i].seed,cases[i].initial);
        CHECK(jce_script_call_named(p->script,cases[i].top,0));
        if(cases[i].second)CHECK(jce_script_call_named(p->script,cases[i].second,0));
        apply_named(p,mood,"es_mood_next",cases[i].next);
    }
}
static void old_stale_next(char *text)
{
    const char *wire="    apply(index, index == -1)\n";
    const char *replacement="    apply(index == -1 and ((M.current + 1) % 8) or index)\n";
    char *at=strstr(text,wire);
    CHECK(at!=NULL);if(!at)return;
    /* The caller owns expansion space; this restores only the old decision. */
    memmove(at+strlen(replacement),at+strlen(wire),strlen(at+strlen(wire))+1);
    memcpy(at,replacement,strlen(replacement));
}
static void missing_next_selector(char *text)
{
    const char *wire="state_key == \"next\" and (D.season * 2 + D.tod + 1) % 8 or ";
    char *at=strstr(text,wire);CHECK(at!=NULL);
    if(at)memmove(at,at+strlen(wire),strlen(at+strlen(wire))+1);
}
static void run(const char *assets,const char *director_name,bool old_ack,int sync_control)
{
    Probe p;JceScriptHost host;JceScriptInstance mood;char *director,*controller;int i;
    static const char *handlers[]={"es_mood_spring_day","es_mood_spring_night","es_mood_winter_day","es_mood_winter_night","es_mood_autumn_day","es_mood_autumn_night","es_mood_rainy_day","es_mood_rainy_night"};
    memset(&p,0,sizeof p);memset(&host,0,sizeof host);
    host.user=&p;host.find_by_name=find;host.get_position=get;host.set_position=set;
    host.ui_set_text=label;host.log=log_line;host.send_message=send;
    p.script=jce_script_create(&host);CHECK(p.script!=NULL);if(!p.script)return;
    director=source(assets,director_name);controller=source(assets,"es_mood_portable.lua");
    CHECK(director && controller);if(!director || !controller)goto end;
    if(old_ack)remove_ack_wire(director);
    if(sync_control==1){
        char *expanded=(char *)jce_malloc(strlen(controller)+128);
        CHECK(expanded!=NULL);if(!expanded)goto end;
        strcpy(expanded,controller);jce_free(controller);controller=expanded;
        old_stale_next(controller);
    }
    if(sync_control==2)missing_next_selector(director);
    p.director=jce_script_instantiate_source(p.script,director_name,director,6);
    mood=jce_script_instantiate_source(p.script,"es_mood_portable.lua",controller,1);
    CHECK(p.director && mood);if(!p.director || !mood)goto end;
    jce_script_call_start(p.script,mood);
    for(i=0;i<8;++i){
        CHECK(jce_script_call_named(p.script,handlers[i],0));
        jce_script_call_update(p.script,mood,.01f);
        CHECK(p.position[3][0]==(float)(i+1) && p.position[3][1]==(float)i && p.position[3][2]==1);
        CHECK(strstr(p.status,"APPLIED | portable preset (native UNAVAILABLE)")!=NULL);
        if(old_ack)goto end;
    }
    CHECK(p.messages==8 && p.errors==0);
    run_interleaved_controls(&p,mood);
    /* Missing method is a clean public no-op, and must not become success. */
    p.drop_receiver=true;CHECK(jce_script_call_named(p.script,"es_mood_next",0));
    jce_script_call_update(p.script,mood,5.01f);
    CHECK(strstr(p.status,"FAILED") && p.position[3][2]==-1);p.drop_receiver=false;
    /* Native plan readiness does not imply apply: shared receiver still owns ack. */
    p.position[4][0]=1;CHECK(jce_script_call_named(p.script,"es_mood_next",0));
    {float seq=p.position[1][0];int messages=p.messages;
     CHECK(p.position[1][2]==1 && p.position[3][0]==0);
     CHECK(jce_script_call_named(p.script,"es_mood_next",0));CHECK(p.position[1][0]==seq);
     set(&p,2,seq,3,1);jce_script_call_update(p.script,mood,.01f);
     CHECK(p.messages==messages+1);jce_script_call_update(p.script,mood,.01f);
     CHECK(strstr(p.status,"APPLIED | native director") && p.position[3][1]==3);}
    CHECK(jce_script_call_named(p.script,"es_mood_next",0));set(&p,2,p.position[1][0],-7,-1);
    jce_script_call_update(p.script,mood,.01f);CHECK(strstr(p.status,"FAILED") && p.position[3][2]==-1);
    CHECK(jce_script_call_named(p.script,"es_mood_next",0));set(&p,2,p.position[1][0],99,1);
    jce_script_call_update(p.script,mood,.01f);CHECK(strstr(p.status,"invalid plan state") && p.position[3][2]==-1);
    CHECK(jce_script_call_named(p.script,"es_mood_next",0));set(&p,2,p.position[1][0],-1,1);
    jce_script_call_update(p.script,mood,.01f);CHECK(strstr(p.status,"invalid plan state") && p.position[3][2]==-1);
    CHECK(jce_script_call_named(p.script,"es_mood_spring_day",0));
    set(&p,3,p.position[1][0],7,1);jce_script_call_update(p.script,mood,.01f);
    CHECK(strstr(p.status,"mismatched apply acknowledgement") && p.position[3][2]==-1);
    p.missing_ack=true;jce_script_call_start(p.script,mood);
    {int messages=p.messages;CHECK(strstr(p.status,"UNAVAILABLE"));CHECK(jce_script_call_named(p.script,"es_mood_spring_day",0));CHECK(p.messages==messages);}
    CHECK(p.errors==0);
end:
    jce_free(director);jce_free(controller);jce_script_destroy(p.script);
}

/* Execute the complete authored Lua producer with narrow public host ports.
 * These destinations check the submitted schema and mirror an external owner;
 * they are NOT a certificate for the engine's generic component JSON merger. */
static char *camera_get(void *user,JceScriptEntity entity,const char *type)
{
    Probe *p=(Probe *)user;char bytes[768];char *out;size_t n;
    if(entity!=7 || strcmp(type,"VirtualCamera")!=0)return NULL;
    ++p->camera_gets;if(!p->camera_component)return NULL;
    snprintf(bytes,sizeof bytes,
        "{\"posX\":%.17g,\"posY\":%.17g,\"posZ\":%.17g,"
        "\"lookX\":%.17g,\"lookY\":%.17g,\"lookZ\":%.17g,"
        "\"fov\":%.17g,\"priority\":%d,\"active\":%s}",
        p->camera_pose[0],p->camera_pose[1],p->camera_pose[2],
        p->camera_pose[3],p->camera_pose[4],p->camera_pose[5],
        p->camera_fov,p->camera_priority,p->camera_active?"true":"false");
    n=strlen(bytes)+1;out=(char *)jce_malloc(n);if(out)memcpy(out,bytes,n);return out;
}
static bool camera_has(void *user,JceScriptEntity entity,const char *type)
{
    Probe *p=(Probe *)user;
    if(entity!=7 || strcmp(type,"VirtualCamera")!=0)return false;
    ++p->camera_checks;return p->camera_component;
}
static bool camera_set(void *user,JceScriptEntity entity,const char *type,const char *text)
{
    static const char *pose[]={"posX","posY","posZ","lookX","lookY","lookZ"};
    Probe *p=(Probe *)user;JceJson *patch,*child;int fields=0,i;
    if(entity!=7 || strcmp(type,"VirtualCamera")!=0)return false;
    ++p->camera_sets;
    if(!p->camera_component){++p->camera_creates;return false;}
    patch=jce_json_parse(text,strlen(text));CHECK(patch!=NULL);if(!patch)return false;
    for(child=jce_json_first_child(patch);child;child=jce_json_next_sibling(child))++fields;
    if(!p->camera_reference){
        CHECK(fields==6);CHECK(!jce_json_has(patch,"fov"));
        CHECK(!jce_json_has(patch,"priority"));CHECK(!jce_json_has(patch,"active"));
    }
    for(i=0;i<6;++i){CHECK(jce_json_has(patch,pose[i]));p->camera_pose[i]=jce_json_get_number(patch,pose[i],p->camera_pose[i]);}
    /* External values are intentionally independent between producer calls. */
    p->camera_fov=jce_json_get_number(patch,"fov",p->camera_fov);
    p->camera_priority=jce_json_get_int(patch,"priority",p->camera_priority);
    p->camera_active=jce_json_get_bool(patch,"active",p->camera_active);
    jce_json_free(patch);return true;
}
static void camera_json_free(void *user,char *text){(void)user;jce_free(text);}
static void camera_delta(void *user,float out[2])
{Probe *p=(Probe *)user;out[0]=p->camera_dx;out[1]=p->camera_dy;}
static float camera_wheel(void *user){return ((Probe *)user)->camera_wheel;}
static bool camera_button(void *user,int button)
{return button==1 && ((Probe *)user)->camera_down;}
static bool replace_camera_wire(char *text,const char *before,const char *after,bool required)
{
    char *at=strstr(text,before);size_t old_n=strlen(before),new_n=strlen(after);
    if(required)CHECK(at!=NULL);
    if(!at)return false;
    CHECK(strstr(at+old_n,before)==NULL);CHECK(new_n<=old_n);
    if(new_n>old_n)return false;
    memmove(at+new_n,at+old_n,strlen(at+old_n)+1);memcpy(at,after,new_n);return true;
}
typedef struct{double pose[5][6];} CameraTrace;
static CameraTrace run_camera(const char *assets,const char *name,int mode,bool absent)
{
    /* mode0=current,1=old transport reference,2=ordinary old-read control,
     * 3=ordinary missing-existence-guard control. */
    static const char *owned=
        "  -- This director owns only the pose. Keep missing cameras absent and let\n"
        "  -- comp_set's public partial merge preserve live priority/fov/active fields.\n"
        "  if not jce.has_component(D.cam, \"VirtualCamera\") then return end\n"
        "  local vc = D.cam_pose\n"
        "  if not vc then vc = {}; D.cam_pose = vc end\n";
    static const char *old=
        "  local vc = comp(D.cam, \"VirtualCamera\")\n  if not vc then return end\n";
    static const float dt[]={.016f,.022f,.010f,.031f,.016f};
    Probe p;JceScriptHost host;CameraTrace trace;char *text=NULL;int i,j;
    memset(&p,0,sizeof p);memset(&host,0,sizeof host);memset(&trace,0,sizeof trace);
    p.camera_case=true;p.camera_entity=!absent;p.camera_component=true;p.camera_reference=mode==1;
    p.camera_pose[0]=18.25;p.camera_pose[1]=10.69;p.camera_pose[2]=27.32;
    host.user=&p;host.find_by_name=find;host.get_position=get;host.set_position=set;host.log=log_line;
    host.comp_get_json=camera_get;host.comp_set_json=camera_set;host.json_free=camera_json_free;
    host.has_component=camera_has;host.pointer_delta=camera_delta;
    host.pointer_wheel=camera_wheel;host.pointer_button=camera_button;
    p.script=jce_script_create(&host);CHECK(p.script!=NULL);if(!p.script)return trace;
    text=source(assets,name);CHECK(text!=NULL);if(!text)goto end;
    if(mode==1 || mode==2)replace_camera_wire(text,owned,old,mode==2);
    if(mode==3)replace_camera_wire(text,
        "  if not jce.has_component(D.cam, \"VirtualCamera\") then return end\n","",true);
    p.director=jce_script_instantiate_source(p.script,name,text,6);
    CHECK(p.director!=0);if(!p.director)goto end;
    jce_script_call_start(p.script,p.director);
    for(i=0;i<5;++i){
        const int before=p.camera_sets;
        p.camera_component=i!=3;p.camera_fov=45+i*3;p.camera_priority=120+i;p.camera_active=(i%2)==0;
        p.camera_down=i==1 || i==2;p.camera_dx=i==1?7:(i==2?-5:0);p.camera_dy=i==1?-3:(i==2?2:0);
        p.camera_wheel=i==2?1:0;
        jce_script_call_update(p.script,p.director,dt[i]);
        if(absent || !p.camera_component)CHECK(p.camera_sets==before);
        else CHECK(p.camera_sets==before+1);
        CHECK(p.camera_fov==45+i*3);CHECK(p.camera_priority==120+i);CHECK(p.camera_active==((i%2)==0));
        for(j=0;j<6;++j)trace.pose[i][j]=p.camera_pose[j];
    }
    CHECK(p.camera_creates==0);CHECK(p.errors==0);
    if(!p.camera_reference)CHECK(p.camera_gets==0);
    else if(!absent)CHECK(p.camera_gets==5);
    if(absent){CHECK(p.camera_checks==0);CHECK(p.camera_sets==0);}
    else if(!p.camera_reference)CHECK(p.camera_checks==5);
end:
    jce_free(text);jce_script_destroy(p.script);return trace;
}
static void run_camera_pair(const char *assets,const char *name)
{
    CameraTrace reference=run_camera(assets,name,1,false);
    CameraTrace actual=run_camera(assets,name,0,false);int i,j;
    for(i=0;i<5;++i)for(j=0;j<6;++j)CHECK(actual.pose[i][j]==reference.pose[i][j]);
    (void)run_camera(assets,name,0,true);
}

int main(int argc,char **argv)
{
    if(argc<2){fprintf(stderr,"assets path required\n");return 2;}
    if(argc>2 && strcmp(argv[2],"--old-camera-read")==0){
        (void)run_camera(argv[1],"es_director_portable.lua",2,false);return failures?1:0;
    }
    if(argc>2 && strcmp(argv[2],"--missing-camera-guard")==0){
        (void)run_camera(argv[1],"es_director_portable.lua",3,false);return failures?1:0;
    }
    if(argc>2 && (strcmp(argv[2],"--old-stale-next")==0 ||
                  strcmp(argv[2],"--missing-next-selector")==0)){
        run(argv[1],"es_director_portable.lua",false,
            strcmp(argv[2],"--old-stale-next")==0?1:2);
        return failures?1:0;
    }
    run(argv[1],"es_director.lua",argc>2 && strcmp(argv[2],"--old-missing-ack-wire")==0,0);
    if(failures)return 1;
    run(argv[1],"es_director_portable.lua",false,0);
    if(failures)return 1;
    run_camera_pair(argv[1],"es_director.lua");
    run_camera_pair(argv[1],"es_director_portable.lua");
    if(failures)return 1;
    puts("ES_MOOD_PROTOCOL_PASS actual_source_and_portable_lua eight_states plan_apply_ack unavailable_fail_controls interleaved_top_buttons_next camera_owned_pose_no_read");
    return 0;
}
