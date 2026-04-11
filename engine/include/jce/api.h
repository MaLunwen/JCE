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
 *
 * Dependency rule: lower layers NEVER depend on upper layers.
 * This header is completely SDL-free.
 */

#ifndef JCE_API_H
#define JCE_API_H

/* ── Layer 1: Core Utilities ─────────────────────────────────────── */
#include <jce/api_core.h>

/* ── Layer 2: OS / Platform Abstraction ──────────────────────────── */
#include <jce/api_platform.h>

/* ── Layer 3: Graphics, Animation, Resources, Audio ──────────────── */
#include <jce/api_graphics.h>
#include <jce/api_animation.h>
#include <jce/api_resource.h>
#include <jce/api_streaming.h>
#include <jce/api_audio.h>

/* ── Layer 4: Render Abstraction ─────────────────────────────────── */
#include <jce/api_render.h>

/* ── Layer 5: Scene ──────────────────────────────────────────────── */
#include <jce/api_scene.h>

/* ── Layer 6: Application ────────────────────────────────────────── */
#include <jce/api_app.h>

/* ── Cross-cutting systems ───────────────────────────────────────── */
#include <jce/api_physics.h>
#include <jce/api_net.h>
#include <jce/api_ui.h>
#include <jce/api_ai.h>

#endif /* JCE_API_H */
