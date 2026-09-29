#ifndef MINES_CONTROLLER_H
#define MINES_CONTROLLER_H
#include <stdint.h>
typedef int64_t (*MinesBestGet)(const char *key);
typedef int (*MinesBestSet)(const char *key,int64_t value);
void mines_records_backend(MinesBestGet get,MinesBestSet set);
void mines_pointer_action(float x,float y,int button);
int mines_take_ai_request(void);
int mines_take_quit_request(void);
void mines_apply_ai(int width,int height,int mines,int r,int g,int b);
#endif
