/* Authored presentation through the shared public JCE script host. */
#include "view.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_metrics.h"
#include <jce/os/core/jce_easing.h>
#define TD_PI 3.14159265358979323846f
static JceScriptEntity find(TdView *v, const char *name)
{
    JceScriptEntity e = 0;
    v->host.find_by_name(v->host.user,name,&e,1); return e;
}
static bool pool(TdView *v, JceScriptEntity *out, int count, const char *prefix)
{
    int i; char name[40];
    for (i=0;i<count;++i) {
        snprintf(name,sizeof name,"%s%04d",prefix,i); out[i]=find(v,name);
        if (!out[i]) return false;
    }
    return true;
}
bool td_view_init(TdView *v,const JceScriptHost *h)
{
    memset(v,0,sizeof *v); v->host=*h;
    v->camera=find(v,"MainCamera"); v->selection=find(v,"Selection"); v->canvas=find(v,"Canvas");
    v->distance=sqrtf(60*60+80*80+110*110); v->yaw=atan2f(60,110);
    v->pitch=atan2f(80,sqrtf(60*60+110*110)); v->width=1280; v->height=720;
    v->ui_opacity=1; v->cards_cell=-1; v->cards_time=.3f;
    return v->camera && v->selection && v->canvas && h->spawn &&
        pool(v,v->hud,TD_WIDGETS,"Hud");
}
static bool ensure_actor(TdView *v, JceScriptEntity *entity, int *old_kind,
                         int kind, const char *family, TdVec position)
{
    char path[80];
    if (*entity && old_kind && *old_kind != kind) {
        v->host.destroy_entity(v->host.user,*entity); *entity=0;
    }
    if (!*entity) {
        if (!strcmp(family,"blood")) snprintf(path,sizeof path,"prefabs/blood.prefab.json");
        else snprintf(path,sizeof path,"prefabs/%s-%d.prefab.json",family,kind);
        *entity=v->host.spawn(v->host.user,path,position.x,position.y,position.z);
        if (!*entity) { v->host.log(v->host.user,"Tower Defense actor prefab failed to spawn"); return false; }
    }
    if (old_kind) *old_kind=kind;
    return true;
}
static void visible(TdView *v,JceScriptEntity e,bool on)
{
    v->host.comp_set_json(v->host.user,e,"MeshRenderer",on?"{\"visible\":true}":"{\"visible\":false}");
}
static void rect(TdView *v,int i,float x,float y,float w,float h,int action)
{
    TdUiPresentation *p=&v->ui_desired[i];
    p->rect[0]=x;p->rect[1]=y;p->rect[2]=w;p->rect[3]=h;p->action=action;
    v->ui_actions[i]=action;memcpy(v->ui_rects[i],p->rect,sizeof p->rect);
}
static void art(TdView *v,int i,float x,float y,float w,float h,const char *path,uint32_t color,int action)
{
    rect(v,i,x,y,w,h,action);
    TdUiPresentation *p=&v->ui_desired[i];
    p->paint=true;p->raycast=action!=0;snprintf(p->sprite,sizeof p->sprite,"%s",path);
    p->color[0]=((color>>16)&255)/255.0f;p->color[1]=((color>>8)&255)/255.0f;
    p->color[2]=(color&255)/255.0f;p->color[3]=(color>>24)/255.0f;
    if(action) {
        p->feedback_set=true;
        for(int k=0;k<12;++k)p->feedback[k]=(k%4)==3?p->color[3]:1;
        p->feedback[12]=p->color[3];
    }
}
/* One patch per changed component, after the desired frame is complete.
 * Each successful callback alone commits its corresponding published fields.
 * Failed callbacks invalidate that component for retry; no failed value is
 * committed, and the other independently accepted components remain valid. */
static bool ui_publish(TdView *v)
{
    enum { IMAGE_RECT=1, IMAGE_PAINT=2, TEXT_RECT=4, BUTTON=8,
           FEEDBACK=16, ACTION=32, TEXT_CLEAR=64, ENABLED=128 };
    JceScriptHost *h=&v->host;
    bool succeeded=true;
    for(int i=0;i<TD_WIDGETS;++i) {
        TdUiPresentation *d=&v->ui_desired[i],*p=&v->ui_published[i];
        unsigned short *valid=&v->ui_valid[i];
        bool geometry= !(*valid&IMAGE_RECT) || memcmp(d->rect,p->rect,sizeof d->rect);
        bool new_role= !(*valid&ACTION) || p->action!=d->action;
        bool paint=d->paint && (!(*valid&IMAGE_PAINT) || p->rect[0]==-10000 || new_role ||
            strcmp(d->sprite,p->sprite) || memcmp(d->color,p->color,sizeof d->color) || d->raycast!=p->raycast);
        bool text_geometry=!(*valid&TEXT_RECT) || memcmp(d->rect,v->ui_text_rect[i],sizeof d->rect);
        bool interactable=d->action!=0;
        bool button=!(*valid&BUTTON) || interactable!=v->ui_button_interactable[i];
        bool colors=d->feedback_set && (!(*valid&FEEDBACK) || memcmp(d->feedback,p->feedback,sizeof d->feedback));
        bool enable=h->set_component_enabled && (!(*valid&ENABLED) || v->ui_button_enabled[i]!=interactable);
        if(!geometry && !paint && !text_geometry && !button && !colors && !new_role &&
           (*valid&TEXT_CLEAR) && !enable) continue;
        char layout[384],fill[384],feedback[768],json[1200];
        int layout_n=snprintf(layout,sizeof layout,
            "\"anchorMinX\":0,\"anchorMaxX\":0,\"anchorMinY\":1,\"anchorMaxY\":1,\"pivotX\":0,\"pivotY\":1,\"anchoredX\":%.3f,\"anchoredY\":%.3f,\"sizeW\":%.3f,\"sizeH\":%.3f",
            d->rect[0],-d->rect[1],d->rect[2],d->rect[3]);
        bool layout_ok=layout_n>0 && (size_t)layout_n<sizeof layout;
        if(geometry || paint) {
            int fill_n=snprintf(fill,sizeof fill,
                "\"spritePath\":\"%s\",\"colorR\":%.5f,\"colorG\":%.5f,\"colorB\":%.5f,\"colorA\":%.5f,\"raycastTarget\":%s",
                d->sprite,d->color[0],d->color[1],d->color[2],d->color[3],d->raycast?"true":"false");
            int n=snprintf(json,sizeof json,"{%s%s%s}",geometry?layout:"",geometry&&paint?",":"",paint?fill:"");
            if(layout_ok && fill_n>0 && (size_t)fill_n<sizeof fill && n>0 && (size_t)n<sizeof json &&
               h->comp_set_json(h->user,v->hud[i],"UIImage",json)) {
                if(geometry){memcpy(p->rect,d->rect,sizeof p->rect);*valid|=IMAGE_RECT;}
                if(paint){memcpy(p->sprite,d->sprite,sizeof p->sprite);memcpy(p->color,d->color,sizeof p->color);p->raycast=d->raycast;*valid|=IMAGE_PAINT;}
            } else { *valid&=(unsigned short)~(IMAGE_RECT|IMAGE_PAINT); succeeded=false; }
        }
        if(text_geometry) {
            int n=snprintf(json,sizeof json,"{%s}",layout);
            if(layout_ok && n>0 && (size_t)n<sizeof json && h->comp_set_json(h->user,v->hud[i],"UIText",json)) {
                memcpy(v->ui_text_rect[i],d->rect,sizeof d->rect);*valid|=TEXT_RECT;
            } else { *valid&=(unsigned short)~(TEXT_RECT); succeeded=false; }
        }
        if(button || colors) {
            int count=snprintf(feedback,sizeof feedback,
                "\"normalR\":%.6f,\"normalG\":%.6f,\"normalB\":%.6f,\"normalA\":%.6f,\"highlightR\":%.6f,\"highlightG\":%.6f,\"highlightB\":%.6f,\"highlightA\":%.6f,\"pressedR\":%.6f,\"pressedG\":%.6f,\"pressedB\":%.6f,\"pressedA\":%.6f,\"disabledA\":%.6f",
                d->feedback[0],d->feedback[1],d->feedback[2],d->feedback[3],d->feedback[4],d->feedback[5],d->feedback[6],d->feedback[7],d->feedback[8],d->feedback[9],d->feedback[10],d->feedback[11],d->feedback[12]);
            char state[32];snprintf(state,sizeof state,"\"interactable\":%s",interactable?"true":"false");
            int n=snprintf(json,sizeof json,"{%s%s%s}",button?state:"",button&&colors?",":"",colors?feedback:"");
            if(count>0 && (size_t)count<sizeof feedback && n>0 && (size_t)n<sizeof json &&
               h->comp_set_json(h->user,v->hud[i],"UIButton",json)) {
                if(button){v->ui_button_interactable[i]=interactable;*valid|=BUTTON;}
                if(colors){memcpy(p->feedback,d->feedback,sizeof p->feedback);*valid|=FEEDBACK;}
            } else { *valid&=(unsigned short)~(BUTTON|FEEDBACK); succeeded=false; }
        }
        if(!(*valid&TEXT_CLEAR)){h->ui_set_text(h->user,v->hud[i],"");*valid|=TEXT_CLEAR;}
        if(new_role){h->set_position(h->user,v->hud[i],(float)d->action,0,0);p->action=d->action;*valid|=ACTION;}
        if(enable) {
            h->set_component_enabled(h->user,v->hud[i],"UIButton",interactable);
            v->ui_button_enabled[i]=interactable;*valid|=ENABLED;
        }
    }
    return succeeded;
}
static void label(TdView *v,int i,float x,float y,const char *name,int action)
{
    char path[80]; float w,h;
    td_ui_metric(name,&w,&h); snprintf(path,sizeof path,"ui/%s.png",name);
    art(v,i,x,y,w,h,path,0xffffffff,!strcmp(name,"cross")?action:0);
}
static void button(TdView *v,int i,float x,float y,float w,int action,bool blue)
{
    const char *background=blue?(w<85?"ui/button-blue-small.png":"ui/button-blue.png"):"ui/button-white.png";
    art(v,i,x,y,w,33.333f,background,0xffffffff,action);
    TdUiPresentation *p=&v->ui_desired[i];
    p->feedback[4]=blue?50.0f/66:235.0f/255;p->feedback[5]=blue?118.0f/139:235.0f/255;p->feedback[6]=blue?177.0f/202:235.0f/255;
    p->feedback[8]=blue?50.0f/66:230.0f/255;p->feedback[9]=blue?118.0f/139:230.0f/255;p->feedback[10]=blue?177.0f/202:230.0f/255;
}
static int number(TdView *v,int i,float right,float y,int value,const char *prefix,float unused)
{
    char text[24],path[80],name[40]; int k; float total=0;
    (void)unused;
    /* Authored numeric sprites contain digits only, including a zero glyph. */
    if (value < 0) value = 0;
    snprintf(text,sizeof text,"%d",value);
    for(k=0;text[k];++k) total+=text[k]=='1'?6.6f:13.2f;
    for(k=0;text[k] && i<TD_WIDGETS;++k) {
        float w,h,advance=text[k]=='1'?6.6f:13.2f;
        snprintf(name,sizeof name,"%s-%c",prefix,text[k]);td_ui_metric(name,&w,&h);
        snprintf(path,sizeof path,"ui/%s.png",name);
        art(v,i++,right-total,y,w,h,path,0xffffffff,0); total-=advance;
    }
    return i;
}
int td_view_ui_action(TdView *v,float x,float y)
{
    int i;
    for(i=TD_WIDGETS-1;i>=0;--i) {
        const float *r=v->ui_rects[i];
        if(v->ui_actions[i] && x>=r[0] && y>=r[1] && x<r[0]+r[2] && y<r[1]+r[3])
            return v->ui_actions[i];
    }
    return 0;
}
bool td_view_ui_contains(TdView *v,float x,float y)
{
    int i;
    for(i=0;i<TD_WIDGETS;++i) {
        const float *r=v->ui_rects[i];
        if(x>=r[0] && y>=r[1] && x<r[0]+r[2] && y<r[1]+r[3]) return true;
    }
    return false;
}
void td_view_pan(TdView *v,float dx,float dy)
{
    float scale=2*v->distance*tanf(20*TD_PI/180)/v->height;
    float sy=sinf(v->yaw),cy=cosf(v->yaw),sp=sinf(v->pitch),cp=cosf(v->pitch);
    v->pan_x+=scale*(-dx*cy-dy*sp*sy);
    v->pan_y+=scale*dy*cp;
    v->pan_z+=scale*(dx*sy-dy*sp*cy);
}
void td_view_camera(TdView *v)
{
    char json[120]; float cp=cosf(v->pitch),sp=sinf(v->pitch);
    v->host.set_position(v->host.user,v->camera,v->pan_x+v->distance*cp*sinf(v->yaw),
        v->pan_y+v->distance*sp,v->pan_z+v->distance*cp*cosf(v->yaw));
    v->host.set_rotation(v->host.user,v->camera,-v->pitch*180/TD_PI,v->yaw*180/TD_PI,0);
    snprintf(json,sizeof json,"{\"refResX\":%.3f,\"refResY\":%.3f}",v->width,v->height);
    v->host.comp_set_json(v->host.user,v->canvas,"Canvas",json);
}
int td_view_pick(TdView *v,float x,float y)
{
    if (!v || !isfinite(x) || !isfinite(y) || v->width<=0 || v->height<=0 ||
        x<0 || y<0 || x>=v->width || y>=v->height) return -1;
    float sy=sinf(v->yaw),cy=cosf(v->yaw),sp=sinf(v->pitch),cp=cosf(v->pitch);
    float tan_fov=tanf(20*TD_PI/180),nx=(2*x/v->width-1)*v->width/v->height*tan_fov;
    float ny=(1-2*y/v->height)*tan_fov;
    float ox=v->pan_x+v->distance*cp*sy,oy=v->pan_y+v->distance*sp,oz=v->pan_z+v->distance*cp*cy;
    float dx=-cp*sy+nx*cy-ny*sp*sy,dy=-sp+ny*cp,dz=-cp*cy-nx*sy-ny*sp*cy;
    float distance,wx,wz; int cx,cz;
    if(fabsf(dy)<0.000001f) return -1;
    distance=-oy/dy; if(distance<=0) return -1;
    wx=ox+distance*dx; wz=oz+distance*dz;
    if(wx<-49.5f||wx>=49.5f||wz<-49.5f||wz>=49.5f) return -1;
    cx=(int)floorf((wx+49.5f)/9); cz=(int)floorf((49.5f-wz)/9);
    return cx*11+cz;
}
void td_view_render(TdView *v,const TdGame *g,int menu,int selected,const char *notice)
{
    int i; JceScriptHost *h=&v->host; (void)notice;
    for(i=0;i<TD_CELLS;++i) {
        const TdTower *t=&g->towers[i]; bool on=!menu && t->active;
        if (on) {
            TdVec position=td_cell_position(i);
            if (!ensure_actor(v,&v->towers[i],&v->tower_kind[i],t->kind,"tower",position)) continue;
            if (!v->tower_visible[i]) visible(v,v->towers[i],true);
        } else if (v->towers[i] && v->tower_visible[i]) visible(v,v->towers[i],false);
        v->tower_visible[i]=on;
    }
    for(i=0;i<TD_ENEMIES;++i) {
        const TdEnemy *e=&g->enemies[i]; bool on=!menu && e->active;
        if (on) {
            if (!ensure_actor(v,&v->enemies[i],&v->enemy_kind[i],e->kind,"enemy",e->position)) continue;
            if (!v->enemy_visible[i]) visible(v,v->enemies[i],true);
        } else if (v->enemies[i] && v->enemy_visible[i]) visible(v,v->enemies[i],false);
        v->enemy_visible[i]=on;
        if(on) {
            h->set_position(h->user,v->enemies[i],e->position.x,e->position.y,e->position.z);
            h->set_rotation(h->user,v->enemies[i],0,e->rotation*180/TD_PI,0);
        }
    }
    for(i=0;i<TD_BULLETS;++i) {
        const TdBullet *b=&g->bullets[i]; bool on=!menu && b->active;
        if (on && !ensure_actor(v,&v->bullets[i],&v->bullet_kind[i],b->kind!=0,"bullet",b->position)) continue;
        if(v->bullets[i] && on!=v->bullet_visible[i])
            h->comp_set_json(h->user,v->bullets[i],"BillboardRenderer",on?"{\"visible\":true}":"{\"visible\":false}");
        v->bullet_visible[i]=on;
        if(on) {
            char json[100]; float alpha=b->lost_frames>80?(180-b->lost_frames)/100.0f:1;
            snprintf(json,sizeof json,"{\"colorA\":%.5f}",fmaxf(0,alpha));
            h->comp_set_json(h->user,v->bullets[i],"BillboardRenderer",json);
            h->set_position(h->user,v->bullets[i],b->position.x,b->position.y,b->position.z);
        }
    }
    visible(v,v->selection,!menu && selected>=0);
    for(i=0;i<TD_BLOOD;++i) {
        bool on=!menu && i<g->blood_count;
        if(on && !ensure_actor(v,&v->blood[i],NULL,0,"blood",g->blood[i].position)) continue;
        if(v->blood[i] && on!=v->blood_visible[i]) visible(v,v->blood[i],on);
        v->blood_visible[i]=on;
        if(on) {
            const TdBlood *b=&g->blood[i];
            h->set_position(h->user,v->blood[i],b->position.x,b->position.y,b->position.z);
            h->set_rotation(h->user,v->blood[i],0,b->rotation*180/TD_PI,0);
        }
    }
    if(selected>=0) { TdVec c=td_cell_position(selected); h->set_position(h->user,v->selection,c.x,0.001f,c.z); }
}
bool td_view_ui(TdView *v,const TdGame *g,int menu,int selected,const char *notice)
{
    int i,n=0; float w=v->width,h=v->height; bool desired=!menu && selected>0 && selected<120 && !g->towers[selected].active; (void)notice;
    v->ui_opacity=1;
    if(desired!=v->cards_open) {
        v->cards_open=desired; v->cards_present=true; v->cards_time=0;
    }
    if(desired) v->cards_cell=selected;
    if(!desired && v->cards_time>=.3f) v->cards_present=false;
    if(menu) {v->cards_present=false;v->cards_open=false;v->cards_time=.3f;}
    for(i=0;i<TD_WIDGETS;++i) {
        memset(&v->ui_desired[i],0,sizeof v->ui_desired[i]);
        rect(v,i,-10000,0,1,1,0);
    }
    if(menu) {
        if(menu==2) art(v,n++,0,0,w,h,"",0xff000000,0);
        else {
            float cover=fmaxf(w/1920,h/1080),bw=1920*cover,bh=1080*cover;
            art(v,n++,(w-bw)/2,(h-bh)/2,bw,bh,"textures/background.jpg",0xffffffff,0);
        }
        if(menu==1 || menu==3) {
            label(v,n++,w/2-157,39.5f,"title",0);
            button(v,n++,w/2-76.52f,109.594f,61.22f,1,true);
            label(v,n++,w/2-64.52f,116.26f,"play",1);
            button(v,n++,w/2-6.55f,109.594f,83,3,false);
            label(v,n++,w/2+5.45f,116.26f,"credits",3);
            label(v,n++,w/2-266,h-51.428f,"credit-line",0);
            art(v,n++,w/2+116.1875f,h-46.7604f,138.6042f,22,"",0,30);
        } else {
            button(v,n++,15,20,129.82f,4,false);
            label(v,n++,27,26.667f,"back",4);
            button(v,n++,151,20,131.7f,2,true);
            label(v,n++,163,26.667f,"level",2);
            label(v,n++,w/2-320,h-51.428f,"level-line",0);
        }
        if(menu==3) {
            art(v,n++,w/2-300,30,600,296.667f,"ui/modal.png",0xffffffff,0);
            label(v,n++,w/2-284,46,"credits-title",0);
            label(v,n++,w/2+271.479f,45.667f,"cross",3);
            label(v,n++,w/2-278,101.43f,"programmer",0);
            label(v,n++,w/2-105,101.43f,"mathieu",0);
            label(v,n++,w/2-242,132.857f,"designer",0);
            label(v,n++,w/2-105,132.857f,"mathieu",0);
            label(v,n++,w/2-105,164.286f,"timothy",0);
            label(v,n++,w/2-257,195.714f,"thanks",0);
            label(v,n++,w/2-105,195.714f,"pracalic",0);
            art(v,n++,w/2+20.2604f,200.9896f,189.4896f,22,"",0,31);
            button(v,n++,w/2+213,278,72,3,false);
            label(v,n++,w/2+225,284.667f,"close",3);
        }
        return ui_publish(v);
    }
    button(v,n++,w/2-65.2421875f,20,130.484375f,4,false);
    label(v,n++,w/2-52.25f,27,"back",4);
    {
        float fraction=w>=1200?12:(w>=992?6:(w>=768?4:1));
        float rw=w/fraction-30,x=w-w/fraction+15;
        const char *icons[]={"resources","points","lives","wave"};
        int values[]={g->resources,g->score,g->lives,g->wave};
        for(i=0;i<4;++i) {
            char path[80]; float y=20+i*41.421875f;
            art(v,n++,x,y,rw,41.421875f,"ui/frame.png",0xffffffff,0);
            snprintf(path,sizeof path,"textures/%s.png",icons[i]);
            art(v,n++,x+5,y+11.84375f,16,16,path,0xffffffff,0);
            if(i==3) {
                n=number(v,n,x+rw-5,y+5,2,"digit",0);
                art(v,n++,x+rw-5-13.203125f-6.125f,y+5,6.125f,32,"ui/slash.png",0xffffffff,0);
                n=number(v,n,x+rw-5-13.203125f-6.125f,y+5,values[i],"digit",0);
            } else n=number(v,n,x+rw-5,y+5,values[i],"digit",0);
        }
        button(v,n++,x,205.6875f,rw,5,true);
        label(v,n++,x+12,212.6875f,"wave",5);
    }
    if(v->cards_present) {
        float x=w/2-140, t=fminf(1,v->cards_time/.3f), offset=0, opacity=1, u;
        /* The source's two CSS keyframe segments use the shared CSS ease. */
        if(v->cards_open) {
            if(t<.8f) {u=jce_ease_cubic_bezier(t/.8f,.25f,.1f,.25f,1);offset=73.3333f*(1-1.1f*u);opacity=.8f*u;}
            else {u=jce_ease_cubic_bezier((t-.8f)/.2f,.25f,.1f,.25f,1);offset=-7.3333f*(1-u);opacity=.8f+.2f*u;}
        } else {
            if(t<.2f) {u=jce_ease_cubic_bezier(t/.2f,.25f,.1f,.25f,1);offset=-7.3333f*u;opacity=1-.2f*u;}
            else {u=jce_ease_cubic_bezier((t-.2f)/.8f,.25f,.1f,.25f,1);offset=-7.3333f+80.6666f*u;opacity=.8f*(1-u);}
        }
        v->ui_opacity=opacity;
        for(i=0;i<3;++i) {
            char path[80]; int action=v->cards_open?10+i:0;
            art(v,n++,x+i*93.3333f,h-93.3333f+offset,73.3333f,73.3333f,"ui/frame.png",0xffffffff,action);
            snprintf(path,sizeof path,"textures/tower-0%d.png",i+1);
            art(v,n++,x+i*93.3333f+4.6667f,h-88.6667f+offset,64,64,path,0xffffffff,action);
            art(v,n++,x+i*93.3333f+4.6667f,h-56+offset,64,31,"",0x80000000,action);
            n=number(v,n,x+i*93.3333f+37+(i==2?16:8),h-54+offset,td_balance[i].cost,g->resources<td_balance[i].cost?"cost-red":"cost",16);
        }
        v->ui_opacity=1;
    }
    return ui_publish(v);
}
