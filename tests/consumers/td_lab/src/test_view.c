/* Presentation contracts through public SDK callbacks and camera matrices. */
#include "view.h"
#include <jce/renderer/jce_camera.h>
#include <jce/os/core/jce_json.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static int checks, spawns, mesh_rewrites;
static int digit_sprites, zero_digit_sprites, invalid_digit_sprites;
static const char *rect_keys[]={"anchorMinX","anchorMaxX","anchorMinY","anchorMaxY","pivotX","pivotY","anchoredX","anchoredY","sizeW","sizeH"};
static const char *color_keys[]={"colorR","colorG","colorB","colorA"};
static const char *feedback_keys[]={"normalR","normalG","normalB","normalA","highlightR","highlightG","highlightB","highlightA","pressedR","pressedG","pressedB","pressedA","disabledA"};
typedef struct {
    double image_rect[10],text_rect[10],color[4],feedback[13];
    char sprite[128],text[64];
    bool interactable,raycast,enabled;
    int action;
} UiObserved;
static UiObserved observed[TD_WIDGETS];
static unsigned ui_calls[TD_WIDGETS][3],text_calls,position_calls,enabled_calls,invalid_json;
static int refused_slot=-1,refused_component=-1;
#define CHECK(x) do { ++checks; if (!(x)) { printf("TD_VIEW FAIL line=%d: %s\n",__LINE__,#x); return 1; } } while(0)
static int find(void *u,const char *name,JceScriptEntity *out,int cap) {
    int index=0; (void)u; if(cap<1)return 0;
    if(sscanf(name,"Hud%d",&index)==1)*out=100+index;
    else *out=!strcmp(name,"MainCamera")?1:(!strcmp(name,"Selection")?2:3);
    return 1;
}
static bool set_json(void *u,JceScriptEntity e,const char *type,const char *json) {
    (void)u;
    int slot=e>=100 && e<100+TD_WIDGETS?(int)e-100:-1;
    int component=!strcmp(type,"UIImage")?0:(!strcmp(type,"UIText")?1:(!strcmp(type,"UIButton")?2:-1));
    if(slot>=0 && component>=0) {
        ++ui_calls[slot][component];
        if(slot==refused_slot && component==refused_component) {refused_slot=refused_component=-1;return false;}
        JceJson *patch=jce_json_parse_strict(json,0);
        if(!patch || !jce_json_is_object(patch)) {++invalid_json;jce_json_free(patch);return false;}
        UiObserved *o=&observed[slot];
        if(component<2) {
            double *rect=component==0?o->image_rect:o->text_rect;
            for(int i=0;i<10;++i)rect[i]=jce_json_get_number(patch,rect_keys[i],rect[i]);
        }
        if(component==0) {
            for(int i=0;i<4;++i)o->color[i]=jce_json_get_number(patch,color_keys[i],o->color[i]);
            o->raycast=jce_json_get_bool(patch,"raycastTarget",o->raycast);
            const char *sprite=jce_json_get_string(patch,"spritePath",NULL);
            if(sprite)snprintf(o->sprite,sizeof o->sprite,"%s",sprite);
        }
        if(component==2) {
            o->interactable=jce_json_get_bool(patch,"interactable",o->interactable);
            for(int i=0;i<13;++i)o->feedback[i]=jce_json_get_number(patch,feedback_keys[i],o->feedback[i]);
        }
        jce_json_free(patch);
    }
    if(!strcmp(type,"MeshRenderer")&&strstr(json,"meshPath"))++mesh_rewrites;
    if(!strcmp(type,"UIImage")) {
        const char *digit=strstr(json,"\"spritePath\":\"ui/digit-");
        if(digit) {
            digit+=strlen("\"spritePath\":\"ui/digit-"); ++digit_sprites;
            if(*digit=='0') ++zero_digit_sprites;
            if(*digit<'0' || *digit>'9' || strncmp(digit+1,".png\"",5))
                ++invalid_digit_sprites;
        }
    }
    return true;
}
static void text(void *u,JceScriptEntity e,const char *s){(void)u;if(e>=100&&e<100+TD_WIDGETS){++text_calls;snprintf(observed[e-100].text,sizeof observed[0].text,"%s",s);}}
static void xyz(void *u,JceScriptEntity e,float x,float y,float z){(void)u;(void)y;(void)z;if(e>=100&&e<100+TD_WIDGETS){++position_calls;observed[e-100].action=(int)x;}}
static void enabled(void *u,JceScriptEntity e,const char *type,bool on){(void)u;if(e>=100&&e<100+TD_WIDGETS&&!strcmp(type,"UIButton")){++enabled_calls;observed[e-100].enabled=on;}}
static void log_line(void *u,const char *s){(void)u;puts(s);}
static JceScriptEntity spawn(void *u,const char *path,float x,float y,float z){(void)u;(void)path;(void)x;(void)y;(void)z;return 1000+(++spawns);}
static void destroy(void *u,JceScriptEntity e){(void)u;(void)e;}
static void clear_call_counts(void) {
    memset(ui_calls,0,sizeof ui_calls);text_calls=position_calls=enabled_calls=0;
}
static unsigned json_call_count(void) {
    unsigned total=0;for(int i=0;i<TD_WIDGETS;++i)for(int c=0;c<3;++c)total+=ui_calls[i][c];return total;
}
static int check_published_geometry_and_actions(TdView *v) {
    for(int i=0;i<TD_WIDGETS;++i) {
        const UiObserved *o=&observed[i];
        CHECK(memcmp(o->image_rect,o->text_rect,sizeof o->image_rect)==0);
        CHECK(fabs(o->image_rect[6]-v->ui_rects[i][0])<.0011);
        CHECK(fabs(-o->image_rect[7]-v->ui_rects[i][1])<.0011);
        CHECK(fabs(o->image_rect[8]-v->ui_rects[i][2])<.0011);
        CHECK(fabs(o->image_rect[9]-v->ui_rects[i][3])<.0011);
        CHECK(o->action==v->ui_actions[i]);CHECK(o->interactable==(o->action!=0));
        CHECK(o->enabled==(o->action!=0));CHECK(o->text[0]=='\0');
        for(int c=0;c<3;++c)CHECK(ui_calls[i][c]<=1);
    }
    for(int i=0;i<TD_WIDGETS;++i)if(v->ui_rects[i][0]>=0) {
        const double *r=observed[i].image_rect;
        float x=(float)(r[6]+r[8]*.5),y=(float)(-r[7]+r[9]*.5);int expected=0;
        for(int j=0;j<TD_WIDGETS;++j) {
            const double *q=observed[j].image_rect;
            if(observed[j].action && x>=q[6] && y>=-q[7] && x<q[6]+q[8] && y<-q[7]+q[9])expected=observed[j].action;
        }
        CHECK(td_view_ui_contains(v,x,y));CHECK(td_view_ui_action(v,x,y)==expected);
    }
    CHECK(invalid_json==0);return 0;
}
static int check_presentation_publication(JceScriptHost *h) {
    static TdView v;static TdGame g;
    CHECK(td_view_init(&v,h));td_reset(&g,7);clear_call_counts();
    td_view_ui(&v,&g,1,-1,"");
    CHECK(check_published_geometry_and_actions(&v)==0);
    CHECK(observed[0].image_rect[8]==1280 && observed[0].image_rect[9]==720);
    CHECK(!strcmp(observed[0].sprite,"textures/background.jpg"));
    static UiObserved before[TD_WIDGETS];memcpy(before,observed,sizeof before);
    clear_call_counts();td_view_ui(&v,&g,1,-1,"");
    CHECK(json_call_count()==0);CHECK(text_calls==0 && position_calls==0 && enabled_calls==0);
    CHECK(memcmp(before,observed,sizeof before)==0);
    /* Exercise actual host patches across menu conversion, gameplay and reflow. */
    const int menus[]={3,2,0,1,0};
    for(unsigned step=0;step<sizeof menus/sizeof menus[0];++step) {
        clear_call_counts();td_view_ui(&v,&g,menus[step],-1,"");
        CHECK(check_published_geometry_and_actions(&v)==0);
    }
    /* Hover/pressed UIImage tint is runtime-owned. Identical authored color
     * must not be republished by unrelated dirty UI or geometry-only reflow. */
    observed[0].color[0]=.125;observed[0].color[1]=.375;observed[0].color[2]=.625;
    clear_call_counts();++g.resources;td_view_ui(&v,&g,0,-1,"");
    CHECK(observed[0].color[0]==.125 && observed[0].color[1]==.375 && observed[0].color[2]==.625);
    CHECK(check_published_geometry_and_actions(&v)==0);
    v.width=1600;v.height=900;clear_call_counts();td_view_ui(&v,&g,0,-1,"");
    CHECK(observed[0].color[0]==.125 && observed[0].color[1]==.375 && observed[0].color[2]==.625);
    CHECK(check_published_geometry_and_actions(&v)==0);
    /* Cards animate geometry, keep selection-switch timing and then retire
     * every formerly active tail slot without stale hit targets. */
    clear_call_counts();td_view_ui(&v,&g,0,60,"");CHECK(v.cards_open);
    CHECK(check_published_geometry_and_actions(&v)==0);
    int first_card=-1;for(int i=0;i<TD_WIDGETS;++i)if(v.ui_actions[i]==10){first_card=i;break;}
    CHECK(first_card>=0);float previous_y=v.ui_rects[first_card][1];v.cards_time=.2f;
    clear_call_counts();td_view_ui(&v,&g,0,61,"");CHECK(v.cards_time==.2f && v.cards_cell==61);
    CHECK(v.ui_rects[first_card][1]<previous_y);
    CHECK(check_published_geometry_and_actions(&v)==0);
    clear_call_counts();td_view_ui(&v,&g,0,-1,"");v.cards_time=.31f;
    clear_call_counts();td_view_ui(&v,&g,0,-1,"");CHECK(!v.cards_present);
    CHECK(check_published_geometry_and_actions(&v)==0);
    for(int component=0;component<3;++component) {
        CHECK(td_view_init(&v,h));td_reset(&g,7);clear_call_counts();
        refused_slot=0;refused_component=component;td_view_ui(&v,&g,1,-1,"");
        CHECK(refused_slot==-1);CHECK(ui_calls[0][component]==1);
        clear_call_counts();td_view_ui(&v,&g,1,-1,"");
        CHECK(ui_calls[0][component]==1);CHECK(json_call_count()==1);
        CHECK(check_published_geometry_and_actions(&v)==0);
        clear_call_counts();td_view_ui(&v,&g,1,-1,"");CHECK(json_call_count()==0);
    }
    return 0;
}
static void count_published_digits(void) {
    digit_sprites=zero_digit_sprites=invalid_digit_sprites=0;
    for(int i=0;i<TD_WIDGETS;++i)if(observed[i].image_rect[6]>=0) {
        const char *sprite=observed[i].sprite;
        if(strncmp(sprite,"ui/digit-",9))continue;
        const char *digit=sprite+9;++digit_sprites;
        if(*digit=='0')++zero_digit_sprites;
        if(*digit<'0'||*digit>'9'||strcmp(digit+1,".png"))++invalid_digit_sprites;
    }
}
int main(void) {
    static TdView v; static TdGame g;
    JceScriptHost h={0};h.find_by_name=find;h.comp_set_json=set_json;
    h.ui_set_text=text;h.set_position=xyz;h.set_rotation=xyz;
    h.set_component_enabled=enabled;
    h.spawn=spawn;h.destroy_entity=destroy;h.log=log_line;
    CHECK(td_view_init(&v,&h));td_reset(&g,7);
    td_view_ui(&v,&g,0,60,"");CHECK(v.cards_open);v.cards_time=.2f;
    td_view_ui(&v,&g,0,61,"");CHECK(v.cards_time==.2f);CHECK(v.cards_cell==61);
    CHECK(td_place(&g,60,0));td_view_render(&v,&g,0,-1,"");CHECK(spawns==1);
    td_view_render(&v,&g,0,-1,"");CHECK(spawns==1);CHECK(mesh_rewrites==0);
    td_view_ui(&v,&g,0,-1,"");CHECK(!v.cards_open);v.cards_time=.31f;
    td_view_ui(&v,&g,0,-1,"");CHECK(!v.cards_present);
    /* Inspect actual public host sprite requests, including INT_MIN. */
    g.resources=INT_MIN; g.score=-1; g.lives=-200; g.wave=INT_MIN;
    digit_sprites=zero_digit_sprites=invalid_digit_sprites=0;
    td_view_ui(&v,&g,0,-1,"");
    count_published_digits();
    CHECK(digit_sprites==5 && zero_digit_sprites==4 && invalid_digit_sprites==0);
    g.resources=g.score=g.lives=g.wave=INT_MAX;
    digit_sprites=zero_digit_sprites=invalid_digit_sprites=0;
    td_view_ui(&v,&g,0,-1,"");
    count_published_digits();
    CHECK(digit_sprites==41 && zero_digit_sprites==0 && invalid_digit_sprites==0);
    td_reset(&g,7); digit_sprites=zero_digit_sprites=invalid_digit_sprites=0;
    td_view_ui(&v,&g,0,-1,"");
    count_published_digits();
    CHECK(digit_sprites==8 && zero_digit_sprites==3 && invalid_digit_sprites==0);
    CHECK(check_presentation_publication(&h)==0);
    CHECK(td_view_pick(&v,-1,360)==-1);CHECK(td_view_pick(&v,640,-1)==-1);
    CHECK(td_view_pick(&v,1280,360)==-1);CHECK(td_view_pick(&v,NAN,360)==-1);
    int visible=0;
    for(int size=0;size<3;++size)for(int pose=0;pose<5;++pose) {
        v.width=size==0?1280:(size==1?640:1024);v.height=size==0?720:(size==1?480:1024);
        v.yaw=.15f+pose*.4f;v.pitch=.25f+pose*.15f;v.distance=149;
        v.pan_x=pose*2;v.pan_y=0;v.pan_z=-pose;
        JceCameraDesc d={0};d.mode=JCE_CAMERA_PERSPECTIVE;d.up=jce_v3(0,1,0);
        d.target=jce_v3(v.pan_x,0,v.pan_z);d.fov_deg=40;d.near_plane=.1f;d.far_plane=1000;
        d.position=jce_v3(v.pan_x+v.distance*cosf(v.pitch)*sinf(v.yaw),v.distance*sinf(v.pitch),v.pan_z+v.distance*cosf(v.pitch)*cosf(v.yaw));
        JceCamera *cam=jce_camera_create(&d);CHECK(cam!=NULL);
        jce_mat4 view=jce_camera_view(cam),proj=jce_camera_proj(cam,v.width/v.height,false),vp=jce_m4_multiply(&proj,&view);
        for(int cell=0;cell<TD_CELLS;++cell) {
            TdVec pos=td_cell_position(cell);jce_vec4 clip=jce_m4_mul_v4(&vp,(jce_vec4){pos.x,0,pos.z,1});
            float x=(clip.x/clip.w+1)*v.width*.5f,y=(1-clip.y/clip.w)*v.height*.5f;
            if(x>=0&&x<v.width&&y>=0&&y<v.height){++visible;CHECK(td_view_pick(&v,x,y)==cell);}
        }
        jce_camera_destroy(cam);
    }
    printf("TD_VIEW PASS checks=%d projected_tiles=%d spawn_reuse=%d mesh_rewrites=%d\n",checks,visible,spawns,mesh_rewrites);
    return 0;
}
