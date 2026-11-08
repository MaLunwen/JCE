#ifndef JCE_SR_FOLIAGE_COOKED_H
#define JCE_SR_FOLIAGE_COOKED_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_scene_renderer.h>

#include "jce_sr_foliage_identity.h"

/* One capped component-asset byte owner is shared by placement and masks.
 * Relative active VFS content wins; isolated misses cannot reach host paths.
 * The caller releases the returned bytes with the matching owner below. */
typedef enum {
    SR_FOLIAGE_BLOB_NONE = 0,
    SR_FOLIAGE_BLOB_FS,
    SR_FOLIAGE_BLOB_ALLOC
} SrFoliageBlobOwner;
void *sr_foliage_read_asset(JceSceneRenderer *sr, const char *path,
    size_t max_size, size_t *out_size, SrFoliageBlobOwner *out_owner);
void sr_foliage_blob_free(void *data, SrFoliageBlobOwner owner);
/* Same bounded image input selection for decode and byte-checked memo use. */
void *sr_foliage_read_image_blob(JceSceneRenderer *sr, const char *path,
    size_t *out_size, SrFoliageBlobOwner *out_owner);

bool sr_foliage_try_load_cooked(JceSceneRenderer *sr,
                                const JceVegetationScatterComponent *scatter,
                                int slot);

#endif /* JCE_SR_FOLIAGE_COOKED_H */
