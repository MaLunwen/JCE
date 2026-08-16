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

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Drop all claims. Call once per frame, before any view assignment. */
void jce_view_bands_begin_frame(void);

/* Declare that `owner` owns bgfx views [first, first + count).
 * `owner` must be a long-lived pointer (a string literal).
 *
 * Returns true when the range was free (or already held by the same owner).
 * On overlap: logs an error naming BOTH owners and the offending view ids,
 * once per distinct pair per frame, and returns false. The caller is not
 * expected to do anything with the result — proceeding reproduces the old
 * last-write-wins behaviour, which is strictly better than refusing to render
 * — but a test can assert on it. */
bool jce_view_bands_claim(const char *owner,
                                           uint16_t first, uint16_t count);

/* Number of overlaps detected since the last begin_frame. 0 is the healthy
 * value; the shipped guard test asserts it across the standard scenes. */
uint32_t jce_view_bands_conflict_count(void);

/* Off by default in dist builds, on otherwise; JCE_VIEW_BAND_CHECK=0/1
 * overrides. When off, claim() is a no-op that returns true. */
bool jce_view_bands_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_VIEW_BANDS_H */
