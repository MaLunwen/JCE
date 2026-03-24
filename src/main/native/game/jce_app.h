/*
 * jce_app.h  Application lifecycle interface.
 *
 * All game/demo logic lives behind this interface.
 * The engine calls init/update/draw/event via the JceAppDesc
 * returned by jce_app_get_desc().
 */

#ifndef JCE_APP_H
#define JCE_APP_H

#include <SDL3/SDL.h>
#include "app/jce_app_interface.h"

/* Opaque game state. */
typedef struct JceApp JceApp;

/* Return the application descriptor (IApp callbacks). */
JceAppDesc jce_app_get_desc(void);

/* Legacy direct-call API (used when no JceAppDesc is registered).
   Kept for backward compatibility. */
JceApp *jce_app_create(const JceServices *svc);
void    jce_app_destroy(JceApp *app);
void    jce_app_update(JceApp *app);
void    jce_app_event(JceApp *app, const SDL_Event *event);
bool    jce_app_should_quit(const JceApp *app);

#endif /* JCE_APP_H */
