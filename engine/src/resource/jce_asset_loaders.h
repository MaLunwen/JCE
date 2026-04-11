/*
 * jce_asset_loaders.h  Type-dispatch sync loaders and async finalization.
 *
 * Split from jce_asset_manager_new.c so that the manager file focuses on
 * lifecycle, public API, and the frame pump, while this module owns all
 * per-type loading/finalization/destruction logic.
 */

#ifndef JCE_ASSET_LOADERS_H
#define JCE_ASSET_LOADERS_H

#include "jce_asset_manager_new.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- synchronous type-dispatch loader ---- */
void load_slot_sync(JceAssetManager *mgr,
                    JceAssetSlot *slot,
                    const char *asset_path,
                    JceAssetType type,
                    const JceAssetLoadParams *params);

/* ---- async finalization (called on main thread) ---- */
void finalize_texture(JceAssetManager *mgr, JceAssetSlot *slot,
                      JceAsyncRequest *req);
void finalize_audio(JceAssetManager *mgr, JceAssetSlot *slot,
                    JceAsyncRequest *req);
void finalize_raw(JceAssetManager *mgr, JceAssetSlot *slot,
                  JceAsyncRequest *req);

/* ---- per-type payload destruction ---- */
void destroy_slot_payload(JceAssetManager *mgr, JceAssetSlot *slot);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ASSET_LOADERS_H */
