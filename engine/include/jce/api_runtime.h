/*
 * api_runtime.h  Layer 5 — Runtime / Editor↔Game Bridge.
 *
 * Per-frame orchestration (PlayerLoop phases) and the game-module
 * registry that lets the editor drive a game in-process during Play.
 */

#ifndef JCE_API_RUNTIME_H
#define JCE_API_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/runtime/jce_coroutine.h>
#include <jce/runtime/jce_ai_scene_director.h>
#include <jce/runtime/jce_scene_generation.h>
#include <jce/runtime/jce_game_module.h>
#include <jce/runtime/jce_player_loop.h>

#ifdef __cplusplus
}
#endif
#endif /* JCE_API_RUNTIME_H */
