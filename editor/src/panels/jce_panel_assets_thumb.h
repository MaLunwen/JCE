/*
 * jce_panel_assets_thumb.h  Editor-side thumbnail cache for the Asset Browser.
 *
 * Lazily decodes image / .mat.json assets to a small GPU texture so the
 * asset grid can render real previews instead of a colored placeholder.
 * Everything runs on the main thread (bgfx resource creation must), but
 * decoding cost is amortised across frames via jce_thumb_pump().
 *
 * Layer: editor (Layer 6) only.  No engine, no client.
 */

#ifndef JCE_PANEL_ASSETS_THUMB_H
#define JCE_PANEL_ASSETS_THUMB_H

#include <stdbool.h>
#include <stdint.h>

extern "C" {
#include <jce/renderer/jce_texture_types.h>
}

enum JceThumbState {
    JCE_THUMB_NONE    = 0,  /* not requested */
    JCE_THUMB_PENDING = 1,  /* queued, decode budget pending */
    JCE_THUMB_READY   = 2,  /* texture valid, ImageButton OK */
    JCE_THUMB_FAILED  = 3   /* permanent failure, fall back to placeholder */
};

struct JceThumb {
    JceTexture handle;
    int        w, h;
    uint8_t    state;
};

/* Returns true and fills *out when an entry exists for this asset (in any
 * state).  Returns false when no entry exists yet — first call also enqueues
 * a PENDING entry for the next pump.  abs_path must be host-absolute. */
bool jce_thumb_request(const char *abs_path, JceThumb *out);

/* Decode up to `budget` pending entries.  Call from the editor main loop
 * once per frame.  budget <= 0 is a no-op. */
void jce_thumb_pump(int budget);

/* Destroy all cached GPU textures.  Call from editor shutdown. */
void jce_thumb_shutdown(void);

/* Return true when a thumbnail can usefully be produced for this path
 * (extension check only — no I/O).  Used by the grid item to decide
 * whether to call jce_thumb_request at all. */
bool jce_thumb_is_eligible(const char *path);

#endif /* JCE_PANEL_ASSETS_THUMB_H */
