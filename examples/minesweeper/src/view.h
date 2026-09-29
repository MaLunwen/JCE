#ifndef MINES_VIEW_H
#define MINES_VIEW_H
#include <jce/api_script.h>
#include "board.h"
#define MINES_WIDGETS 530
typedef struct {
    JceScriptHost host;
    JceScriptEntity widgets[MINES_WIDGETS];
    int count,actions[MINES_WIDGETS];
    float rects[MINES_WIDGETS][4];
    uint32_t background;
    bool flag_mode;
} MinesView;
bool mines_view_init(MinesView *v,const JceScriptHost *host);
void mines_view_refresh(MinesView *v,const MinesBoard *b,int menu,int difficulty,
                        const int custom[3],int64_t best,bool save_ok);
int mines_view_hit(const MinesView *v,float x,float y);
#endif
