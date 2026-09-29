/*
 * api.h  Master umbrella header for the JCE game engine.
 *
 * Include this single header to access all public engine APIs.
 * For selective inclusion, use the per-layer sub-headers instead
 * (e.g. <jce/api_core.h>, <jce/api_graphics.h>).
 *
 * Architecture layers (bottom → top):
 *
 *   Layer 1 — Core Utilities      <jce/api_core.h>
 *   Layer 2 — OS Abstraction      <jce/api_platform.h>
 *   Layer 3 — Graphics            <jce/api_graphics.h>
 *             Animation           <jce/api_animation.h>
 *             Resource Loader     <jce/api_resource.h>
 *             Resource Streaming  <jce/api_streaming.h>
 *             Audio               <jce/api_audio.h>
 *   Layer 4 — Render Abstraction  <jce/api_render.h>
 *   Layer 5 — Scene               <jce/api_scene.h>
 *   Layer 6 — Application         <jce/api_app.h>
 *   Layer 7 — Game (user layer)
 *
 *   Cross-cutting:
 *             Physics             <jce/api_physics.h>
 *             Networking          <jce/api_net.h>
 *             UI                  <jce/api_ui.h>
 *             AI                  <jce/api_ai.h>
 *             LLM authoring       <jce/api_llm.h>
 *
 * Dependency rule: lower layers NEVER depend on upper layers.
 * This header is completely SDL-free.
 */

#ifndef JCE_API_H
#define JCE_API_H

/* ── Layer 0: Compatibility baseline (compile-time assertions) ────── */
#include <jce/jce_compat.h>

/* ── Layer 1: Core Utilities ─────────────────────────────────────── */
#ifdef __cplusplus
extern "C" {
#endif

#include <jce/api_core.h>

/* ── Layer 2: OS / Platform Abstraction ──────────────────────────── */
#include <jce/api_platform.h>

/* ── Layer 3: Graphics, Animation, Resources, Audio ──────────────── */
#include <jce/api_animation.h>
#include <jce/api_audio.h>
#include <jce/api_graphics.h>
#include <jce/api_resource.h>
#include <jce/api_streaming.h>

/* ── Layer 4: Render Abstraction ─────────────────────────────────── */
#include <jce/api_render.h>

/* ── Layer 5: Scene ──────────────────────────────────────────────── */
#include <jce/api_scene.h>

/* ── Layer 5: World subsystems (ECS world, spawn, weather, ToD, …) ─ */
#include <jce/api_world.h>

/* ── Layer 5: Runtime (PlayerLoop, game-module bridge) ───────────── */
#include <jce/api_runtime.h>

/* ── Layer 6: Application ────────────────────────────────────────── */
#include <jce/api_app.h>

/* ── Cross-cutting systems ───────────────────────────────────────── */
#include <jce/api_ai.h>
#include <jce/api_llm.h>
#include <jce/api_net.h>
#include <jce/api_physics.h>
#include <jce/api_script.h>
#include <jce/api_ui.h>

/* ── Completed 2026-08-31 ────────────────────────────────────────────
 *
 * This file is what §4 means by "用户代码只需 #include <jce/api.h> 即可获得
 * 整套引擎", and five umbrellas that exist in the tree were missing from it.
 * api_input.h is named in §4's own table; api_middleware.h claims to
 * aggregate ALL middleware and reached nobody through this file; api_save.h
 * and api_video.h are new here because those two subsystems had no umbrella
 * at all.
 *
 * Nothing is included twice in practice -- every umbrella carries its own
 * include guard -- and the ordering is irrelevant for the same reason.  They
 * are listed separately from the block above so that a later reader can see
 * WHEN the entry point stopped being partial.
 */
/* ai_dispatch is a PRIVATE module (spec C.8).  JCESDKInstall.cmake EXCLUDES
 * api_ai_dispatch.h and middleware/ai_dispatch/ from the package whenever
 * JCE_ENABLE_AI_DISPATCH is OFF -- which is the default -- so an
 * unconditional include here compiles in-tree and then breaks every SDK
 * consumer at the first line of api.h.  Guarded exactly as api_middleware.h
 * has always guarded it. */
#if defined(JCE_ENABLE_AI_DISPATCH) && JCE_ENABLE_AI_DISPATCH
#include <jce/api_ai_dispatch.h>
#endif
#include <jce/api_input.h>
#include <jce/api_middleware.h>
#include <jce/api_save.h>
#include <jce/api_video.h>
/* Self-description: what the engine accepts and what it is doing, as JSON.
 * Unconditional -- unlike ai_dispatch it is not a private module, it pulls no
 * third-party type into a public header, and the SDK installs it like any
 * other application header. */
#include <jce/api_introspect.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_H */
