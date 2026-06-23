/*
 * api_middleware.h  Aggregate header for cross-cutting middleware
 * subsystems (parity with The-Forge `IMiddleware.h`).
 *
 * Pulls in physics, AI, networking, UI, animation, audio, video —
 * i.e. everything that is neither low-level OS nor pure graphics.
 * (Resource streaming is a resource-tier subsystem — see api_resource.h.)
 * Use when the client wants the full middleware surface without listing
 * each layer header individually.
 */

#ifndef JCE_API_MIDDLEWARE_H
#define JCE_API_MIDDLEWARE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/api_ai.h>
#include <jce/api_animation.h>
#include <jce/api_audio.h>
#include <jce/api_net.h>
#include <jce/api_physics.h>
#include <jce/api_ui.h>
#include <jce/api_world.h>

#ifdef __cplusplus
}
#endif
#endif /* JCE_API_MIDDLEWARE_H */
