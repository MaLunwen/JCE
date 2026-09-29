/* Public ECS Canvas consumer; layout and hit testing share the same rects. */
#include "view.h"
#include <stdio.h>
#include <string.h>

static void widget(MinesView *v,float x,float y,float w,float h,
                   const char *text,float size,uint32_t fill,uint32_t ink,int action)
{
    char json[1600];int i=v->count++;
    JceScriptHost *hptr=&v->host;
    const char *layout="\"anchorMinX\":0,\"anchorMaxX\":0,\"anchorMinY\":1,\"anchorMaxY\":1,\"pivotX\":0,\"pivotY\":1";
    snprintf(json,sizeof json,"{%s,\"anchoredX\":%.2f,\"anchoredY\":%.2f,\"sizeW\":%.2f,\"sizeH\":%.2f,\"colorR\":%.4f,\"colorG\":%.4f,\"colorB\":%.4f,\"colorA\":%.4f}",
        layout,x,-y,w,h,((fill>>16)&255)/255.0,((fill>>8)&255)/255.0,(fill&255)/255.0,(fill>>24)/255.0);
    hptr->comp_set_json(hptr->user,v->widgets[i],"UIImage",json);
    snprintf(json,sizeof json,"{\"fontSize\":%.2f,\"alignment\":1,\"colorR\":%.4f,\"colorG\":%.4f,\"colorB\":%.4f,\"colorA\":1}",
        size,((ink>>16)&255)/255.0,((ink>>8)&255)/255.0,(ink&255)/255.0);
    hptr->comp_set_json(hptr->user,v->widgets[i],"UIText",json);
    snprintf(json,sizeof json,"{%s,\"anchoredX\":%.2f,\"anchoredY\":%.2f,\"sizeW\":%.2f,\"sizeH\":%.2f}",layout,x,-y,w,h);
    hptr->comp_set_json(hptr->user,v->widgets[i],"UIText",json);
    hptr->ui_set_text(hptr->user,v->widgets[i],text);
    snprintf(json,sizeof json,"{\"interactable\":%s,\"onClickHandler\":\"mines_ui_click\"}",action?"true":"false");
    hptr->comp_set_json(hptr->user,v->widgets[i],"UIButton",json);
    {
        const char *states[]={"normal","highlight","pressed","disabled"};int state;
        for(state=0;state<4;++state) {
            const char *s=states[state];
            snprintf(json,sizeof json,"{\"%sR\":%.4f,\"%sG\":%.4f,\"%sB\":%.4f,\"%sA\":%.4f}",
                s,((fill>>16)&255)/255.0,s,((fill>>8)&255)/255.0,s,(fill&255)/255.0,s,0.0);
            hptr->comp_set_json(hptr->user,v->widgets[i],"UIButton",json);
        }
    }
    hptr->set_position(hptr->user,v->widgets[i],(float)action,0,0);
    v->rects[i][0]=x;v->rects[i][1]=y;v->rects[i][2]=w;v->rects[i][3]=h;v->actions[i]=action;
}

bool mines_view_init(MinesView *v,const JceScriptHost *host)
{
    int i;memset(v,0,sizeof *v);v->host=*host;v->background=0xff202830;
    for(i=0;i<MINES_WIDGETS;++i) {
        char name[32];snprintf(name,sizeof name,"Widget%03d",i);
        if(host->find_by_name(host->user,name,&v->widgets[i],1)!=1) return false;
    }
    return true;
}

void mines_view_refresh(MinesView *v,const MinesBoard *b,int menu,
                        int difficulty,const int custom[3],int64_t best,bool save_ok)
{
    static const char *names[]={"BEGINNER","INTERMEDIATE","EXPERT","CUSTOM"};
    static const uint32_t digits[]={0xff222222,0xff1746bd,0xff18723b,0xffc93232,
        0xff292b83,0xff802020,0xff147c83,0xff111111,0xff616161};
    int i, previous=v->count;
    char label[160];
    v->count=0;
    widget(v,0,0,1280,720,"",1,v->background,0,0);
    widget(v,40,24,1200,58,"MINESWEEPER",38,0,0xfff2f4f6,0);
    widget(v,40,78,1200,26,"A clear board. A careful choice.",15,0,0xffa6bec9,0);
    if(menu) {
        widget(v,380,142,520,64,"Choose your board",24,0,0xffeef3f5,0);
        widget(v,410,220,460,62,"BEGINNER     9 x 9     /     10 mines",22,0xffd7dde0,0xff202830,1);
        widget(v,410,298,460,62,"INTERMEDIATE     16 x 16     /     40 mines",20,0xffd7dde0,0xff202830,2);
        widget(v,410,376,460,62,"EXPERT     30 x 16     /     99 mines",22,0xffd7dde0,0xff202830,3);
        widget(v,410,454,460,62,"CUSTOM BOARD",22,0xffd7dde0,0xff202830,4);
        widget(v,510,550,260,54,"QUIT",20,0xff384a57,0xfff3f5f7,7);
        widget(v,260,650,760,32,"Left: reveal   |   Right: flag   |   Middle: chord",19,0,0xffb7c9d2,0);
        widget(v,250,685,780,30,"Your first click and its neighbours are always safe.",18,0,0xffb7c9d2,0);
    } else {
        int cols=b->width,rows=b->height;
        float cell=rows<=9?46.0f:27.0f, bx=(1280-cols*cell)*0.5f, by=195;
        int seconds=(int)b->elapsed;
        if(seconds>999) seconds=999;
        snprintf(label,sizeof label,"MINES  %03d",b->mines-b->flags);
        widget(v,50,125,250,46,label,25,0xff10181d,0xffff6363,0);
        snprintf(label,sizeof label,"TIME  %03d",seconds);
        widget(v,980,125,250,46,label,25,0xff10181d,0xffff6363,0);
        widget(v,332,125,190,46,"MENU [Esc]",18,0xffd7dde0,0xff202830,6);
        widget(v,536,125,230,46,"RESTART [N]",18,0xffd7dde0,0xff202830,5);
        widget(v,780,125,160,46,v->flag_mode?"FLAG MODE":"REVEAL MODE",15,0xffc6d0d6,0xff202830,9);
        widget(v,30,188,180,42,"AI REMIX",18,0xff80d6b5,0xff203630,11);
        widget(v,bx-8,by-8,cols*cell+16,rows*cell+16,"",1,0xff747d84,0,0);
        for(i=0;i<cols*rows;++i) {
            const MinesCell *c=&b->cells[i];
            uint32_t fill=c->revealed?0xffb5bcc1:0xffe2e5e7,ink=0xff222222;
            const char *s=""; char digit[2]={0,0};
            if(c->exploded) { fill=0xffef5350; s="*"; }
            else if(b->state==MINES_LOST && c->flagged && !c->mine) { s="X";ink=0xffad2626; }
            else if(b->state==MINES_LOST && c->mine) s="*";
            else if(c->flagged || (b->state==MINES_WON && c->mine)) { s="F";ink=0xffbd2424; }
            else if(c->revealed && c->adjacent) { digit[0]=(char)('0'+c->adjacent);s=digit;ink=digits[c->adjacent]; }
            widget(v,bx+(i%cols)*cell+1,by+(i/cols)*cell+1,cell-2,cell-2,s,
                   cell*0.62f,fill,ink,1000+i);
        }
        snprintf(label,sizeof label,"%s   |   %s   |   Safe cells %d / %d",
            b->state==MINES_READY?"READY":b->state==MINES_PLAYING?"PLAYING":b->state==MINES_WON?"YOU WIN":"GAME OVER",
            names[difficulty],b->revealed,cols*rows-b->mines);
        widget(v,80,640,1120,32,label,22,0,b->state==MINES_WON?0xff80e6b1:0xffe8eef1,0);
        if(best>0) snprintf(label,sizeof label,"BEST %.2f s   |   Left reveal / Right flag / Middle chord",best/1000.0);
        else snprintf(label,sizeof label,"BEST --   |   Left reveal / Right flag / Middle chord");
        widget(v,80,678,1120,28,save_ok?label:"Record could not be saved; this session is still playable.",17,0,0xffb7c9d2,0);
    }
    /* Custom configuration uses a separate menu page; dimensions remain bounded. */
    if(menu==2) {
        widget(v,350,190,580,440,"",1,0xff293844,0,0);
        for(i=0;i<3;++i) {
            snprintf(label,sizeof label,"%s: %d",i==0?"WIDTH":i==1?"HEIGHT":"MINES",custom[i]);
            widget(v,465,230+i*84,350,56,label,23,0,0xfff1f4f5,0);
            widget(v,380,230+i*84,66,56,"-",28,0xffc6d0d6,0xff202830,20+i*2);
            widget(v,830,230+i*84,66,56,"+",28,0xffc6d0d6,0xff202830,21+i*2);
        }
        widget(v,400,506,220,62,"START",22,0xff80d6b5,0xff203630,8);
        widget(v,660,506,220,62,"BACK",22,0xffc6d0d6,0xff202830,6);
    }
    for(i=v->count;i<previous;++i) {
        v->host.comp_set_json(v->host.user,v->widgets[i],"UIImage","{\"anchoredX\":-10000,\"colorA\":0}");
        v->host.ui_set_text(v->host.user,v->widgets[i],"");
        v->host.comp_set_json(v->host.user,v->widgets[i],"UIButton","{\"interactable\":false}");
    }
}

int mines_view_hit(const MinesView *v,float x,float y)
{
    int i;
    for(i=v->count-1;i>=0;--i) {
        const float *r=v->rects[i];
        if(x>=r[0] && y>=r[1] && x<r[0]+r[2] && y<r[1]+r[3]) return v->actions[i];
    }
    return 0;
}
