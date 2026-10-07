/* SDK application: public services own input, Canvas owns pixels, the project owns rules. */
#include "view.h"
#include "controller.h"
#include <jce/script_vm/jce_script_vm_c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void say(const char *format,...);
#define SAY(...) say(__VA_ARGS__)
typedef struct {
    MinesBoard board;
    MinesView view;
    JceScriptHost host;
    JceScriptEntity intent,custom_entity,theme,proposal,color,version;
    int serial,revision;bool used;
    int64_t records[4];
    int menu,difficulty,custom[3],last_second;
    uint32_t seed;
    int64_t best;
    bool dirty,save_ok,trace,self_test;
} MinesApp;
static MinesApp game;
static MinesBestGet record_get;
static MinesBestSet record_set;
static int ai_request,quit_request;
#include <stdarg.h>
static void say(const char *format,...) {
    char buf[1500];va_list args;va_start(args,format);vsnprintf(buf,sizeof buf,format,args);va_end(args);
    if(game.host.log) game.host.log(game.host.user,buf);
}
static JceScriptEntity find(const char *name) {
    JceScriptEntity e=0;game.host.find_by_name(game.host.user,name,&e,1);return e;
}
static const int presets[3][3]={{9,9,10},{16,16,40},{30,16,99}};

static void record_key(char out[64])
{
    snprintf(out,64,"best_%dx%d_%d",game.board.width,game.board.height,game.board.mines);
}

static void trace_board(const char *action,int index)
{
    char layout[MINES_CAPACITY+1]; int i;
    if(!game.trace) return;
    for(i=0;i<game.board.width*game.board.height;++i) {
        const MinesCell *c=&game.board.cells[i];
        layout[i]=c->mine?'*':(char)('0'+c->adjacent);
    }
    layout[i]=0;
    SAY("MINES_TRACE action=%s index=%d state=%d width=%d height=%d mines=%d flags=%d revealed=%d time=%.3f seed=%u layout=%s",
        action,index,game.board.state,game.board.width,game.board.height,game.board.mines,
        game.board.flags,game.board.revealed,game.board.elapsed,game.board.seed,layout);
}

static void start(int difficulty)
{
    const int *cfg=difficulty<3?presets[difficulty]:game.custom;
    char key[64];
    if(!mines_reset(&game.board,cfg[0],cfg[1],cfg[2],game.seed++)) return;
    game.difficulty=difficulty;game.menu=0;game.dirty=true;game.last_second=-1;
    record_key(key);game.best=record_get?record_get(key):game.records[difficulty];
    trace_board("start",-1);
}

static void save_win(void)
{
    int64_t ms=(int64_t)(game.board.elapsed*1000.0);
    char key[64];
    if(ms<1) ms=1;
    if(!game.best || ms<game.best) {
        game.best=ms;record_key(key);
        game.records[game.difficulty]=ms;
        game.save_ok=record_set?record_set(key,ms)!=0:true;
        SAY("MINES_BEST key=%s milliseconds=%lld persisted=%d",key,(long long)ms,game.save_ok);
    }
}

static void command(int action,int button)
{
    MinesState before=game.board.state;
    if(action>=1000 && !game.menu) {
        int index=action-1000;bool changed;
        if(button==1 && game.view.flag_mode) button=3;
        if(button==3) changed=mines_flag(&game.board,index);
        else if(button==2) changed=mines_chord(&game.board,index);
        else changed=mines_reveal(&game.board,index);
        if(changed) {
            game.dirty=true;
            if(before!=MINES_WON && game.board.state==MINES_WON) save_win();
            trace_board(button==3?"flag":button==2?"chord":"reveal",index);
        }
        return;
    }
    if(button!=1) return;
    if(action>=1 && action<=3) start(action-1);
    else if(action==4) { game.menu=2;game.dirty=true; }
    else if(action==5) start(game.difficulty);
    else if(action==6) { game.menu=1;game.dirty=true;trace_board("menu",-1); }
    else if(action==7) quit_request=1;
    else if(action==8) start(3);
    else if(action==9) { game.view.flag_mode=!game.view.flag_mode;game.dirty=true; }
    else if(action==11) { ai_request=1;SAY("MINES_AI_REQUEST"); }
    else if(action>=20 && action<=25) {
        int slot=(action-20)/2,delta=(action%2)?1:-1;
        static const int minimum[3]={5,5,1},maximum[3]={30,16,471};
        int max=slot==2?game.custom[0]*game.custom[1]-9:maximum[slot];
        int value=game.custom[slot]+delta;
        if(value>=minimum[slot] && value<=max) game.custom[slot]=value;
        if(game.custom[2]>game.custom[0]*game.custom[1]-9) game.custom[2]=game.custom[0]*game.custom[1]-9;
        game.host.set_position(game.host.user,game.custom_entity,(float)game.custom[0],(float)game.custom[1],(float)game.custom[2]);
        game.dirty=true;
    }
}


void mines_records_backend(MinesBestGet get,MinesBestSet set) {record_get=get;record_set=set;}
void mines_pointer_action(float x,float y,int button) {
    if(game.used) command(mines_view_hit(&game.view,x,y),button);
}
int mines_take_ai_request(void) {int result=ai_request;ai_request=0;return result;}
int mines_take_quit_request(void) {int result=quit_request;quit_request=0;return result;}
void mines_apply_ai(int width,int height,int mines,int r,int g,int b) {
    JceScriptHost *h=&game.host;
    if(!game.used) return;
    h->set_position(h->user,game.proposal,(float)width,(float)height,(float)mines);
    h->set_position(h->user,game.color,r/255.0f,g/255.0f,b/255.0f);
    {float current[3]={0};h->get_position(h->user,game.version,current);
        game.revision=(int)current[0]+1;h->set_position(h->user,game.version,(float)game.revision,0,0);}
}
static void *create(const JceCScriptContext *ctx) {
    size_t size;
    if(!ctx || ctx->struct_size<sizeof *ctx || !ctx->host || game.used) return NULL;
    memset(&game,0,sizeof game);game.used=true;
    size=ctx->host_size<sizeof game.host?ctx->host_size:sizeof game.host;
    memcpy(&game.host,ctx->host,size);
    if(!game.host.comp_set_json || !game.host.ui_set_text || !game.host.find_by_name) {game.used=false;return NULL;}
    return &game;
}
static void destroy(void *self) {(void)self;memset(&game,0,sizeof game);ai_request=quit_request=0;}
static JceCStatus on_start(void *self) {
    const char *seed=getenv("MINES_SEED");(void)self;
    game.menu=1;game.dirty=true;game.save_ok=true;
    game.custom[0]=20;game.custom[1]=16;game.custom[2]=50;
    game.seed=seed?(uint32_t)strtoul(seed,NULL,10):(uint32_t)time(NULL);
    game.trace=getenv("MINES_TRACE")!=NULL;
    if(!mines_view_init(&game.view,&game.host)) return "Minesweeper widget pool is incomplete";
    game.intent=find("InputIntent");game.custom_entity=find("CustomSettings");game.theme=find("Theme");
    game.proposal=find("AiProposal");game.color=find("AiColor");game.version=find("AiVersion");
    if(!game.intent || !game.custom_entity || !game.theme || !game.proposal || !game.color || !game.version)
        return "Minesweeper data components are missing";
    SAY("MINES_BOOT C rules + Lua director + authored Canvas; records=%s",record_get?"persistent":"session");
    return JCE_C_OK;
}
static JceCStatus on_update(void *self,float dt) {
    float value[3];uint32_t color;(void)self;
    if(!game.menu) mines_tick(&game.board,dt);
    if(game.host.get_position(game.host.user,game.intent,value) && (int)value[2]!=game.serial) {
        game.serial=(int)value[2];command((int)value[0],(int)value[1]);
    }
    if(game.host.get_position(game.host.user,game.custom_entity,value)) {
        int w=(int)value[0],h=(int)value[1],m=(int)value[2];
        if(w>=5 && w<=30 && h>=5 && h<=16 && m>=1 && m<=w*h-9 &&
          (w!=game.custom[0] || h!=game.custom[1] || m!=game.custom[2])) {
            game.custom[0]=w;game.custom[1]=h;game.custom[2]=m;game.dirty=true;
            SAY("MINES_CONFIG width=%d height=%d mines=%d",w,h,m);
        }
    }
    if(game.host.get_position(game.host.user,game.theme,value)) {
        int r=(int)(value[0]*255+0.5f),g=(int)(value[1]*255+0.5f),b=(int)(value[2]*255+0.5f);
        if(r>=0 && r<=255 && g>=0 && g<=255 && b>=0 && b<=255) {
            color=0xff000000u|((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
            if(color!=game.view.background) {game.view.background=color;game.dirty=true;}
        }
    }
    if((int)game.board.elapsed!=game.last_second) {game.last_second=(int)game.board.elapsed;game.dirty=true;}
    if(game.dirty) {
        mines_view_refresh(&game.view,&game.board,game.menu,game.difficulty,game.custom,game.best,game.save_ok);
        game.dirty=false;
    }
    return JCE_C_OK;
}
JCE_C_SCRIPT_CLASS_BEGIN(MinesController,"MinesController",create,destroy)
    JCE_C_ON_START(on_start)
    JCE_C_ON_UPDATE(on_update)
JCE_C_SCRIPT_CLASS_END()
JCE_C_MODULE_BEGIN()
    JCE_C_MODULE_CLASS(MinesController)
JCE_C_MODULE_GLOBALS()
JCE_C_MODULE_END("mines",mines_module)
