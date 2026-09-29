/*
 * jce_view_bands.h  Runtime overlap guard for bgfx view-id bands.
 *
 * bgfx view state is last-write-wins: two subsystems that bind different
 * framebuffers to the same view id do not conflict loudly, they silently
 * produce whichever came second.  JCE hands out view ids from three places
 * that do not talk to each other — the scene renderer's base-relative bands,
 * the postfx chain's re-basable range, and the absolute ids in jce_views.h —
 * and the failure mode when they meet is "something stopped rendering", with
 * nothing in any log pointing at view ids.
 *
 * That has now happened at least twice.  The omnidirectional-point-shadow tile
 * band had to relocate to base+100 because base+50/60/70 are the editor
 * overlay/preview/pick ids; the dual-shadow dynamic atlas then took base+21..25,
 * documented in-source as free, which were PostFX/TAA_Resolve, TAA_HistoryCopy
 * and Composite — the viewport rendered black and the feature was written off
 * as unmeasurable on discrete GPUs for months.
 *
 * So: every subsystem declares the range it is about to own, and an overlap is
 * reported by name, at the moment it happens, instead of being discovered as a
 * black screen.  Claims are per frame (a frame legitimately renders the scene
 * several times — editor viewport, game view, thumbnails — each with its own
 * base), and a subsystem re-claiming a range it already owns this frame is not
 * an error.
 */

#ifndef JCE_VIEW_BANDS_H
#define JCE_VIEW_BANDS_H

#include <jce/renderer/jce_views.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The declarations now live in <jce/renderer/jce_views.h>, beside the view-id
 * table they guard, so a NON-ENGINE owner (the editor's viewports) can declare
 * its bands too.  This header stays as the engine-internal include path. */

#ifdef __cplusplus
}
#endif

#endif /* JCE_VIEW_BANDS_H */
