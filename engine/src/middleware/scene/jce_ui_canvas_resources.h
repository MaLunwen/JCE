/*
 * jce_ui_canvas_resources.h -- how a canvas RESOLVES a font, in one place.
 *
 * WHY THIS EXISTS.  jce_ui_canvas.c is capped at 3,000 lines by
 * tools/lint/check_file_size.py and has been split for that reason before --
 * jce_ui_canvas_widgets.h records the same story and the same rule for
 * choosing WHERE: take the block with the smallest interface, not the biggest
 * block.  Adding the font fallback chain pushed the file to 3,014.
 *
 * What moved is the PROCESS-SCOPED resolution config and the ladder that
 * descends it: the project asset root on the host filesystem, then the active
 * VFS (bundle Play and shipped bundle boots), then the canvas pak.  What
 * stayed is the per-canvas CACHE, because that lives on struct JceUICanvas,
 * which is private to jce_ui_canvas.c and worth keeping that way -- so the
 * ladder takes the pak it needs as a parameter instead.
 *
 * A font and its fallbacks descend the SAME ladder.  A fallback resolved by a
 * different rule than the font it backs is the editor-vs-shipped divergence
 * this tree keeps finding, one level down: the primary from the project and
 * its fallback from the editor's own pak, visible in exactly one of the two
 * builds.
 */
#ifndef JCE_UI_CANVAS_RESOURCES_H
#define JCE_UI_CANVAS_RESOURCES_H

#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/renderer/jce_text.h>

#include <stdbool.h>

typedef struct JcePakArchive JcePakArchive;

/* The generation that invalidates every per-canvas resource cache.  Bumped by
 * each of the three process-scoped setters; a canvas compares its own copy
 * against it and drops what it holds. */
unsigned uc_resource_generation(void);

/* The default font path a UIText with no font_path of its own gets, or "" for
 * the engine's built-in face. */
const char *uc_resource_default_font(void);

/* Absolute host path of this machine's UI font, or false if none is
   readable.  What an unspecified UIText font resolves to, so that the
   engine's silent default is the platform's own UI face rather than the
   hand-drawn glyph set that ships with the sample content. */
bool uc_resource_system_ui_font(char *out, size_t out_size);

/* The project source-assets root, or "" when unset (pak-only boots).  Read by
 * the SPRITE ladder as well as the font one -- it is the same first tier, and
 * a second copy of the string is how the two would start disagreeing. */
const char *uc_resource_asset_root(void);

/* Open ONE font path down the three-tier ladder.  NULL when no tier has it. */
JceFont *uc_open_font_path(const JcePakArchive *pak, const char *p,
                           int px, bool sdf);

/* Attach the configured fallback chain to a freshly opened font.  Calls back
 * into uc_get_font, so the canvas's cache must already hold `f`'s slot. */
void uc_attach_fallbacks(JceUICanvas *uc, JceFont *f, int px, bool sdf);

#endif /* JCE_UI_CANVAS_RESOURCES_H */
