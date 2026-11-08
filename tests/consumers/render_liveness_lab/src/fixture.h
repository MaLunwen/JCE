#ifndef JCE_RENDER_LIVENESS_FIXTURE_H
#define JCE_RENDER_LIVENESS_FIXTURE_H
#include <jce/api_app.h>
bool lab_init(const JceServices *services, void *user);
void lab_draw(const JceServices *services, void *user);
void lab_exit(void *user);
#endif
