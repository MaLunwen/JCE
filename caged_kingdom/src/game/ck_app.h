/*
 * ck_app.h  Caged Kingdom application lifecycle interface.
 *
 * All game/demo logic lives behind this interface.
 * The engine calls init/update/draw/event via the JceAppDesc
 * returned by ck_app_get_desc().
 */

#ifndef CK_APP_H
#define CK_APP_H

#include <SDL3/SDL.h>
#include <jce/app/jce_app_interface.h>

/* Opaque game state. */
typedef struct CkApp CkApp;

/* Return the application descriptor (IApp callbacks). */
JceAppDesc ck_app_get_desc(void);

#endif /* CK_APP_H */
