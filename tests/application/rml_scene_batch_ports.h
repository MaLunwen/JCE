/* Typed CPU dependency ports for the complete opt-in scene overlay header.
 * These ports do not instantiate native RmlUi, ECS, SDL or a renderer. */
#ifndef RML_SCENE_CONTROLS_PORTS_H
#define RML_SCENE_CONTROLS_PORTS_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define JCE_KEY_COUNT 512
#define JCE_KEY_F1 58
#define JCE_KEY_M 16
#define JCE_KEY_I 12
#define JCE_KEY_ESCAPE 41
#define JCE_VIEW_UI 250
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
typedef int JceKey;
typedef uint64_t JceEntity;
#define JCE_ENTITY_INVALID UINT64_C(0)
#define JCE_TAG_NAME_MAX 64
#define JCE_TAG_REGISTRY_MAX 1024
typedef struct { uint16_t tag_id; } JceTagComponent;
typedef struct { uint8_t layer; } JceLayerComponent;
static uint16_t jce_tag_intern(const char *name);
#define PORT_SCENE_MAX 128
typedef struct JceRenderer { int unused; } JceRenderer;
typedef struct JcePakArchive { int unused; } JcePakArchive;
typedef struct JceFileSystem { int unused; } JceFileSystem;
typedef struct JceWindow { bool relative,grab; int writes; } JceWindow;
typedef struct JceInput { bool keys[512],buttons[6]; int touches; float x,y; } JceInput;
typedef struct JceInputActions { float values[8]; int count; } JceInputActions;
typedef struct { char text[512]; } JceUITextComponent;
typedef struct { bool is_on; } JceUIToggleComponent;
typedef struct { float value,min_value,max_value; } JceUIProgressBarComponent;
typedef struct JceScene {
    char tags[PORT_SCENE_MAX][64]; JceUITextComponent text[PORT_SCENE_MAX];
    JceUIToggleComponent toggle[PORT_SCENE_MAX]; JceUIProgressBarComponent progress[PORT_SCENE_MAX];
    JceTagComponent tag_components[PORT_SCENE_MAX];
    uint32_t generations[PORT_SCENE_MAX];
    bool present[PORT_SCENE_MAX], missing_text[PORT_SCENE_MAX], missing_toggle[PORT_SCENE_MAX];
    unsigned dummy_count;
} JceScene;
typedef struct { uint32_t idx; } JceUIDocHandle;
typedef struct { uint32_t idx; } JceUIElementHandle;
typedef void (*jce_ui_event_fn)(JceUIElementHandle,const char *,void *);
typedef struct JceUIContext { int alive; } JceUIContext;
typedef int jce_allocator_t;
typedef struct { uint32_t width,height; JceRenderer *renderer; JcePakArchive *pak; } JceUIContextDesc;
static JceUIContext port_context;
static char port_ids[128][64],port_text[128][512],port_display[128][16];
static int port_element_count,port_text_writes,port_property_writes;
static bool port_fail_listener;
static int port_full_input,port_pointer_input;
static jce_allocator_t jce_allocator_default(void) { return 0; }
static JceUIContext *jce_ui_create(const JceUIContextDesc *d,jce_allocator_t a)
{ (void)d;(void)a;port_context.alive=1;return &port_context; }
static void jce_ui_destroy(JceUIContext *ui) { ui->alive=0; }
static bool jce_ui_doc_valid(JceUIDocHandle h) { return h.idx!=UINT32_MAX; }
static bool jce_ui_elem_valid(JceUIElementHandle h) { return h.idx!=UINT32_MAX; }
static JceUIDocHandle jce_ui_doc_load(JceUIContext *c,const char *n,const char *s,uint32_t z)
{ (void)c;(void)n;(void)s;(void)z;JceUIDocHandle h={0};return h; }
static void jce_ui_doc_show(JceUIContext *c,JceUIDocHandle d) { (void)c;(void)d; }
static JceUIElementHandle jce_ui_find_element(JceUIContext *c,JceUIDocHandle d,const char *id)
{
    (void)c;(void)d;JceUIElementHandle h={UINT32_MAX};
    for (int i=0;i<port_element_count;++i) if (!strcmp(port_ids[i],id)) { h.idx=(uint32_t)i;break; }
    return h;
}
static bool jce_ui_elem_on(JceUIContext *c,JceUIElementHandle e,const char *t,jce_ui_event_fn f,void *u)
{ (void)c;(void)e;(void)t;(void)f;(void)u;return !port_fail_listener; }
static void jce_ui_elem_set_text(JceUIContext *c,JceUIElementHandle e,const char *s)
{ (void)c;++port_text_writes;snprintf(port_text[e.idx],sizeof port_text[0],"%s",s); }
static void jce_ui_elem_set_property(JceUIContext *c,JceUIElementHandle e,const char *k,const char *s)
{ (void)c;++port_property_writes;if (!strcmp(k,"display")) snprintf(port_display[e.idx],sizeof port_display[0],"%s",s); }
static void jce_ui_set_dpi_scale(JceUIContext *c,float v) { (void)c;(void)v; }
static void jce_ui_resize(JceUIContext *c,uint32_t w,uint32_t h) { (void)c;(void)w;(void)h; }
static void jce_ui_process_input(JceUIContext *c,const JceInput *i) { (void)c;(void)i;++port_full_input; }
static void jce_ui_process_pointer_state(JceUIContext *c,float x,float y,bool d,bool v)
{ (void)c;(void)x;(void)y;(void)d;(void)v;++port_pointer_input; }
static void jce_ui_update(JceUIContext *c,float dt) { (void)c;(void)dt; }
static void jce_ui_render_view(JceUIContext *c,uint16_t v) { (void)c;(void)v; }
static unsigned port_scene_walks, port_entity_visits, port_tag_reads, port_text_reads;
static int port_profile_enabled, port_phase_count;
static char port_phases[16][24];
static bool port_alive(JceScene *s,JceEntity e)
{
    unsigned i=(unsigned)e;
    return i>0 && i<PORT_SCENE_MAX && (s->tags[i][0]||s->present[i]) &&
        e==((JceEntity)s->generations[i]<<32 | i);
}
static void jce_scene_each_entity(JceScene *s,void (*cb)(JceScene *,JceEntity,void *),void *u)
{
    ++port_scene_walks;
    for (unsigned i=0;i<s->dummy_count;++i) { ++port_entity_visits;cb(s,10000u+i,u); }
    for (unsigned i=1;i<PORT_SCENE_MAX;++i) if (s->tags[i][0]||s->present[i]) {
        ++port_entity_visits;cb(s,(JceEntity)s->generations[i]<<32 | i,u);
    }
}
static JceTagComponent *jce_scene_get_tag_component(JceScene *s,JceEntity e)
{
    ++port_tag_reads;
    if (!port_alive(s,e)||!s->tags[(unsigned)e][0]) return NULL;
    s->tag_components[(unsigned)e].tag_id=jce_tag_intern(s->tags[(unsigned)e]);
    return &s->tag_components[(unsigned)e];
}
static JceLayerComponent *jce_scene_get_layer_component(JceScene *s,JceEntity e)
{ (void)s;(void)e;return NULL; }
static JceEntity jce_scene_find_with_tag(JceScene *s,const char *tag);
static JceUITextComponent *jce_scene_get_ui_text(JceScene *s,JceEntity e)
{ ++port_text_reads;return port_alive(s,e)&&!s->missing_text[(unsigned)e]?&s->text[(unsigned)e]:NULL; }
static JceUIToggleComponent *jce_scene_get_ui_toggle(JceScene *s,JceEntity e)
{ return port_alive(s,e)&&!s->missing_toggle[(unsigned)e]?&s->toggle[(unsigned)e]:NULL; }
static JceUIProgressBarComponent *jce_scene_get_ui_progress_bar(JceScene *s,JceEntity e)
{ return port_alive(s,e)?&s->progress[(unsigned)e]:NULL; }
static int jce_perf_phase_enabled(void) { return port_profile_enabled; }
static uint64_t jce_time_perf_counter(void) { static uint64_t tick;return ++tick; }
static double jce_time_perf_to_ms(uint64_t a,uint64_t b) { return (double)(b-a); }
static void jce_perf_phase_add(const char *name,double ms)
{
    (void)ms;
    if (port_phase_count<16) snprintf(port_phases[port_phase_count++],24,"%s",name);
}
static void jce_input_mouse_pos(const JceInput *i,float *x,float *y) { *x=i->x;*y=i->y; }
static bool jce_input_key_down(const JceInput *i,JceKey k) { return k>=0&&k<512&&i->keys[k]; }
static bool jce_input_key_pressed(const JceInput *i,JceKey k) { return jce_input_key_down(i,k); }
static bool jce_input_mouse_button(const JceInput *i,int b) { return b>=1&&b<=5&&i->buttons[b]; }
static int jce_input_touch_count(const JceInput *i) { return i->touches; }
static int jce_actions_count(const JceInputActions *a) { return a->count; }
static float jce_action_value(const JceInputActions *a,int id) { return a->values[id]; }
static void jce_window_set_relative_mouse_mode(JceWindow *w,bool value) { w->relative=value;++w->writes; }
static void jce_window_set_mouse_grab(JceWindow *w,bool value) { w->grab=value;++w->writes; }
static void *jce_fs_read_all(JceFileSystem *f,const char *p,uint64_t *z) { (void)f;(void)p;*z=0;return NULL; }
static void jce_fs_buffer_free(void *p) { (void)p; }
#endif
