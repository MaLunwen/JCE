/* Complete production-header CPU controls. External UI/scene/input owners are
 * typed ports, so this does not certify native Rml rendering, SDK or devices. */
#include "rml_scene_batch_ports.h"
#include "rml_tag_lookup_owner.h"
#define JCE_RML_SCENE_HOST_RUNTIME
#include <jce/application/jce_rml_scene_overlay.h>
#include <assert.h>

static JceRenderer renderer;
static void reset_ports(void)
{
    memset(&port_context,0,sizeof port_context);
    memset(port_ids,0,sizeof port_ids);memset(port_text,0,sizeof port_text);
    memset(port_display,0,sizeof port_display);
    const char *ids[]={"panel","hud","value","increase","decrease"};
    port_element_count=5;
    for (int i=0;i<5;++i) snprintf(port_ids[i],sizeof port_ids[i],"%s",ids[i]);
    port_text_writes=port_property_writes=0;port_fail_listener=false;
}
static bool open_overlay(JceRmlSceneOverlay *o,const char *bindings)
{
    memset(o,0,sizeof *o);
    return jce_rml_scene_overlay_open(o,&renderer,NULL,"<rml/>",6,bindings,strlen(bindings),1280,720);
}
static void setup(JceRmlSceneOverlay *o,JceScene *scene)
{
    reset_ports();memset(scene,0,sizeof *scene);
    snprintf(scene->tags[1],64,"modal");snprintf(scene->tags[2],64,"hud");
    snprintf(scene->tags[3],64,"value");snprintf(scene->tags[4],64,"commands");
    scene->toggle[2].is_on=true;snprintf(scene->text[3].text,512,"retained child state");
    assert(open_overlay(o,"modal|panel|modal\nvisible|hud|hud\ntext|value|value\ncommand|increase|commands\ncommand|decrease|commands\nkey|67|commands\nmodal-key|41|commands\n"));
    jce_rml_scene_overlay_sync(o,scene);
}
static int legacy_semantics(void)
{
    JceRmlSceneOverlay o;JceScene scene;JceInput input;JceInputActions actions;
    memset(&input,0,sizeof input);memset(&actions,0,sizeof actions);
    setup(&o,&scene);
    assert(jce_rml_scene_overlay_has_modal_controls(&o));
    assert(!strcmp(port_display[0],"none")&&!strcmp(port_display[1],"block"));
    int text_writes=port_text_writes;
    scene.toggle[1].is_on=true;scene.toggle[2].is_on=false;jce_rml_scene_overlay_sync(&o,&scene);
    assert(!strcmp(port_display[0],"block")&&!strcmp(port_display[1],"none"));
    assert(text_writes==port_text_writes&&!strcmp(port_text[2],"retained child state"));
    assert(jce_rml_scene_overlay_wants_input(&o,&scene));
    scene.toggle[1].is_on=false;
    input.keys[41]=true;jce_rml_scene_overlay_prepare_input(&o,&scene,&input);
    assert(!scene.text[4].text[0]&&!jce_rml_scene_overlay_wants_input(&o,&scene));
    scene.toggle[1].is_on=true;jce_rml_scene_overlay_prepare_input(&o,&scene,&input);
    assert(!scene.text[4].text[0]); /* held before opening is not a new menu edge */
    input.keys[41]=false;jce_rml_scene_overlay_prepare_input(&o,&scene,&input);
    input.keys[41]=true;jce_rml_scene_overlay_prepare_input(&o,&scene,&input);
    assert(!strcmp(scene.text[4].text,"key:41"));scene.text[4].text[0]=0;
    input.keys[41]=false;scene.toggle[1].is_on=false;
    port_full_input=port_pointer_input=0;input.x=200;input.y=100;
    jce_rml_scene_overlay_frame(&o,&scene,250,1280,720,.016f,true,20,10,false,&input);
    assert(port_pointer_input==1&&port_full_input==0);
    jce_rml_scene_overlay_frame(&o,&scene,250,1280,720,.016f,true,200,100,false,&input);
    assert(port_pointer_input==1&&port_full_input==1);
    JceUIElementHandle button={3};jce_rml_scene_overlay_click(button,"click",&o);
    assert(!strcmp(scene.text[4].text,"increase"));
    button.idx=4;jce_rml_scene_overlay_click(button,"click",&o);
    assert(!strcmp(scene.text[4].text,"increase"));
    assert(jce_rml_scene_overlay_wants_input(&o,&scene));
    scene.text[4].text[0]=0;input.keys[67]=true;
    jce_rml_scene_overlay_prepare_input(&o,&scene,&input);
    assert(!strcmp(scene.text[4].text,"key:67"));
    assert(jce_rml_scene_overlay_blocks_gameplay(&o,&scene,&input,&actions));
    scene.text[4].text[0]=0;jce_rml_scene_overlay_prepare_input(&o,&scene,&input);
    assert(!scene.text[4].text[0]); /* no held repeat after consuming the edge */
    assert(jce_rml_scene_overlay_blocks_gameplay(&o,&scene,&input,&actions));
    input.keys[67]=false;input.keys[26]=true; /* held movement remains quarantined */
    assert(jce_rml_scene_overlay_blocks_gameplay(&o,&scene,&input,&actions));
    input.keys[26]=false;input.buttons[1]=true;
    assert(jce_rml_scene_overlay_blocks_gameplay(&o,&scene,&input,&actions));
    input.buttons[1]=false;input.touches=1;
    assert(jce_rml_scene_overlay_blocks_gameplay(&o,&scene,&input,&actions));
    input.touches=0;actions.count=1;actions.values[0]=.5f;
    assert(jce_rml_scene_overlay_blocks_gameplay(&o,&scene,&input,&actions));
    actions.values[0]=0;
    assert(jce_rml_scene_overlay_blocks_gameplay(&o,&scene,&input,&actions)); /* neutral frame */
    assert(!jce_rml_scene_overlay_blocks_gameplay(&o,&scene,&input,&actions));
    jce_rml_scene_overlay_prepare_input(&o,&scene,&input);input.keys[67]=true;
    jce_rml_scene_overlay_prepare_input(&o,&scene,&input);assert(!strcmp(scene.text[4].text,"key:67"));
    scene.text[4].text[0]=0;input.keys[67]=false;jce_rml_scene_overlay_prepare_input(&o,&scene,NULL);
    jce_rml_scene_overlay_close(&o);

    const char *invalid[]={"visible|hud|same\ntext|value|same\n","key|-1|commands\n",
        "key|512|commands\n","key|67x|commands\n","key|67|commands\nkey|067|commands\n",
        "command|missing|commands\n","key|67|commands\ntext|value|commands\n"};
    for (size_t i=0;i<sizeof invalid/sizeof invalid[0];++i) {
        reset_ports();assert(!open_overlay(&o,invalid[i]));assert(!o.ui);
    }
    reset_ports();port_fail_listener=true;assert(!open_overlay(&o,"command|increase|commands\n"));assert(!o.ui);
    reset_ports();assert(open_overlay(&o,"text|value|value\n"));
    assert(!jce_rml_scene_overlay_has_modal_controls(&o));jce_rml_scene_overlay_close(&o);

    setup(&o,&scene);memset(&input,0,sizeof input);memset(&actions,0,sizeof actions);
    JceWindow window;memset(&window,0,sizeof window);window.relative=window.grab=true;
    bool captured=true;scene.toggle[1].is_on=true;
    assert(jce_rml_scene_overlay_runtime_input(&o,&scene,&input,&actions,&window,&captured,true));
    assert(!captured&&!window.relative&&!window.grab);
    scene.toggle[1].is_on=false;
    assert(jce_rml_scene_overlay_runtime_input(&o,&scene,&input,&actions,&window,&captured,false));
    assert(!jce_rml_scene_overlay_runtime_input(&o,&scene,&input,&actions,&window,&captured,false));
    assert(!captured&&o.capture_owned); /* focus/Alt veto retains a pending restore */
    assert(!jce_rml_scene_overlay_runtime_input(&o,&scene,&input,&actions,&window,&captured,true));
    assert(captured&&window.relative&&window.grab&&!o.capture_owned);
    jce_rml_scene_overlay_close(&o);
    puts("RML_SCENE_CONTROLS_CPU_OK: visibility, mailbox, keys, capture, release quarantine; native UI unverified");
    return 0;
}
