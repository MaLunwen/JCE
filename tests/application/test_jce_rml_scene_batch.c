/* Complete production-header CPU controls. The tag registry and tag-find
 * owner are extracted unchanged; ECS iteration, UI, input and clocks are
 * counted typed ports. These are operation/semantic checks, not FPS tests. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "rml_scene_legacy_cases.h"

static unsigned failures;
#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#expression); ++failures; \
} } while (0)

static void reset_counts(void)
{
    port_scene_walks=port_entity_visits=port_tag_reads=port_text_reads=0;
}

static void make_bulk(JceRmlSceneOverlay *o,JceScene *s)
{
    char manifest[8192];size_t used=0;
    reset_ports();memset(s,0,sizeof *s);port_element_count=0;
    for (int i=0;i<77;++i) {
        snprintf(port_ids[port_element_count++],64,"element_%d",i);
        if (i<58) {
            const char *kind=i<52?"text":i<57?"visible":"modal";
            used+=(size_t)snprintf(manifest+used,sizeof manifest-used,"%s|element_%d|value_%d\n",kind,i,i);
            snprintf(s->tags[i+1],64,"value_%d",i);
            snprintf(s->text[i+1].text,512,"content_%d",i);
        } else {
            used+=(size_t)snprintf(manifest+used,sizeof manifest-used,"command|element_%d|commands\n",i);
        }
    }
    used+=(size_t)snprintf(manifest+used,sizeof manifest-used,"key|67|commands\n");
    const int keys[]={41,80,79,82,81,40};
    for (int i=0;i<6;++i)
        used+=(size_t)snprintf(manifest+used,sizeof manifest-used,"modal-key|%d|commands\n",keys[i]);
    snprintf(s->tags[59],64,"commands");s->dummy_count=2018-59;
    CHECK(open_overlay(o,manifest));CHECK(o->bind_count==84);
}

static void budget_and_input(void)
{
    JceRmlSceneOverlay o;JceScene s;JceInput input;memset(&input,0,sizeof input);
    make_bulk(&o,&s);reset_counts();
    jce_rml_scene_overlay_sync(&o,&s);
    printf("sync walks=%u visits=%u tag_reads=%u\n",port_scene_walks,port_entity_visits,port_tag_reads);
    CHECK(port_scene_walks==1);CHECK(port_entity_visits==2018);CHECK(port_text_writes==52);
    for (int i=0;i<52;++i) CHECK(!strcmp(port_text[i],s.text[i+1].text));
    const int previous_writes=port_text_writes;
    jce_rml_scene_overlay_sync(&o,&s);CHECK(port_text_writes==previous_writes);
    reset_counts();CHECK(!jce_rml_scene_overlay_wants_input(&o,&s));
    printf("wants_input walks=%u text_reads=%u\n",port_scene_walks,port_text_reads);
    CHECK(port_scene_walks==1);CHECK(port_text_reads==1);
    reset_counts();jce_rml_scene_overlay_prepare_input(&o,&s,&input);
    printf("idle_prepare walks=%u\n",port_scene_walks);CHECK(port_scene_walks==0);
    const int keys[]={41,80,79,82,81,40};
    for (int i=0;i<6;++i) input.keys[keys[i]]=true;
    reset_counts();jce_rml_scene_overlay_prepare_input(&o,&s,&input);
    CHECK(port_scene_walks==1);CHECK(!s.text[59].text[0]);
    for (int i=78;i<84;++i) CHECK(o.binds[i].key_held);
    s.toggle[58].is_on=true;reset_counts();
    jce_rml_scene_overlay_prepare_input(&o,&s,&input);
    CHECK(!s.text[59].text[0]);CHECK(port_scene_walks==0);
    memset(&input,0,sizeof input);jce_rml_scene_overlay_prepare_input(&o,&s,&input);
    for (int i=0;i<6;++i) input.keys[keys[i]]=true;
    reset_counts();jce_rml_scene_overlay_prepare_input(&o,&s,&input);
    printf("six_modal_edges walks=%u\n",port_scene_walks);
    CHECK(port_scene_walks==2);CHECK(!strcmp(s.text[59].text,"key:41"));
    s.text[59].text[0]=0;jce_rml_scene_overlay_prepare_input(&o,&s,&input);
    CHECK(!s.text[59].text[0]);
    jce_rml_scene_overlay_close(&o);
}

static void dynamic_first_match(void)
{
    JceRmlSceneOverlay o;JceScene a,b;memset(&a,0,sizeof a);memset(&b,0,sizeof b);
    reset_ports();CHECK(open_overlay(&o,"text|value|dup\n"));
    snprintf(a.tags[1],64,"dup");snprintf(a.tags[2],64,"dup");
    snprintf(a.text[1].text,512,"first");snprintf(a.text[2].text,512,"second");
    jce_rml_scene_overlay_sync(&o,&a);CHECK(!strcmp(port_text[2],"first"));
    a.missing_text[1]=true;snprintf(a.text[2].text,512,"must not select second");
    int writes=port_text_writes;jce_rml_scene_overlay_sync(&o,&a);CHECK(writes==port_text_writes);
    snprintf(a.tags[1],64,"retagged");jce_rml_scene_overlay_sync(&o,&a);
    CHECK(!strcmp(port_text[2],"must not select second"));
    a.tags[2][0]=0;jce_rml_scene_overlay_sync(&o,&a);
    a.generations[2]=7;snprintf(a.tags[2],64,"dup");snprintf(a.text[2].text,512,"recreated");
    jce_rml_scene_overlay_sync(&o,&a);CHECK(!strcmp(port_text[2],"recreated"));
    snprintf(b.tags[2],64,"dup");snprintf(b.text[2].text,512,"other scene");
    jce_rml_scene_overlay_sync(&o,&b);CHECK(!strcmp(port_text[2],"other scene"));
    jce_rml_scene_overlay_close(&o);CHECK(!o.ui&&!o.scene&&!o.bind_count);
    CHECK(open_overlay(&o,"text|value|later\n"));jce_rml_scene_overlay_sync(&o,&a);
    snprintf(a.tags[3],64,"later");snprintf(a.text[3].text,512,"appeared");
    jce_rml_scene_overlay_sync(&o,&a);CHECK(!strcmp(port_text[2],"appeared"));
    jce_rml_scene_overlay_close(&o);

    reset_ports();memset(&a,0,sizeof a);CHECK(open_overlay(&o,"modal|panel|dup\n"));
    snprintf(a.tags[1],64,"dup");snprintf(a.tags[2],64,"dup");
    a.missing_toggle[1]=true;a.toggle[2].is_on=true;
    CHECK(!jce_rml_scene_overlay_wants_input(&o,&a));
    a.tags[1][0]=0;CHECK(jce_rml_scene_overlay_wants_input(&o,&a));
    jce_rml_scene_overlay_close(&o);
}

static void untagged_and_failure(void)
{
    JceRmlSceneOverlay o;JceScene s;memset(&s,0,sizeof s);reset_ports();
    s.present[1]=true;snprintf(s.text[1].text,512,"untagged first");
    CHECK(open_overlay(&o,"text|value|Untagged\n"));
    jce_rml_scene_overlay_sync(&o,&s);CHECK(!strcmp(port_text[2],"untagged first"));
    jce_rml_scene_overlay_close(&o);reset_ports();
    /* Exercise the actual process tag registry refusing a new name. */
    for (int i=1;i<JCE_TAG_REGISTRY_MAX;++i) {
        s_tags[i].in_use=true;snprintf(s_tags[i].name,JCE_TAG_NAME_MAX,"occupied_%d",i);
    }
    CHECK(open_overlay(&o,"text|value|cannot_intern\n"));reset_counts();
    jce_rml_scene_overlay_sync(&o,&s);CHECK(port_text_writes==0);CHECK(port_text_reads==0);
    jce_rml_scene_overlay_close(&o);s_tags_initialized=0;
}

static void profiling(void)
{
    JceRmlSceneOverlay o;JceScene s;setup(&o,&s);
    port_profile_enabled=0;port_phase_count=0;
    jce_rml_scene_overlay_frame(&o,&s,250,1280,720,.016f,false,0,0,false,NULL);
    CHECK(port_phase_count==0);port_profile_enabled=1;
    jce_rml_scene_overlay_frame(&o,&s,250,1280,720,.016f,false,0,0,false,NULL);
    CHECK(port_phase_count==2);CHECK(!strcmp(port_phases[0],"rml_sync"));
    CHECK(!strcmp(port_phases[1],"rml_render"));port_profile_enabled=0;
    jce_rml_scene_overlay_close(&o);
}

int main(int argc,char **argv)
{
    if (argc>1&&!strcmp(argv[1],"--budget-only")) { budget_and_input();return failures?1:0; }
    CHECK(legacy_semantics()==0);budget_and_input();dynamic_first_match();
    untagged_and_failure();profiling();
    if (failures) { fprintf(stderr,"RML_BATCH_CPU_FAIL failures=%u\n",failures);return 1; }
    puts("RML_BATCH_CPU_OK: complete C header; counted dependency ports; no native UI/FPS claim");
    return 0;
}
