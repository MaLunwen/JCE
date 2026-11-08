#ifndef JCE_SR_WATER_CAUSTIC_ATLAS_H
#define JCE_SR_WATER_CAUSTIC_ATLAS_H

#include <bgfx/c99/bgfx.h>
#include <jce/middleware/scene/jce_water_fft.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/resource/jce_pak_loader.h>

enum { SR_WATER_CAUSTIC_VIEWPORTS = 2 };

typedef struct SrWaterCausticAtlas SrWaterCausticAtlas;

typedef struct SrWaterCausticTile {
    bgfx_texture_handle_t texture;
    float uv_origin_x;
    float uv_origin_z;
    float uv_extent;
} SrWaterCausticTile;

typedef enum SrWaterCausticBand {
    SR_WATER_CAUSTIC_PRIMARY,
    SR_WATER_CAUSTIC_SECONDARY,
    SR_WATER_CAUSTIC_TERTIARY,
    SR_WATER_CAUSTIC_BAND_COUNT,
} SrWaterCausticBand;

typedef struct SrWaterCausticSource {
    const JceWaterFft *fft; /* borrowed for the call */
    bgfx_texture_handle_t displacement;
    uint64_t revision;
} SrWaterCausticSource;

/* Count eligible primaries before traversing this view. The reservation is
 * deliberately conservative when visibility or source admission rejects one. */
uint8_t sr_water_caustic_atlas_primary_reservation(JceScene *scene);

/* Queue before binding water draw state. The view must already be reserved.
 * The first queue in a frame latches the primary reservation. Missing or
 * invalid reservations reserve all slots for primary. True publishes primary.
 * The caller supplies displacement handles already updated from the matching
 * source revision. Optional bands are admitted in order, with invalid tiles
 * marking unavailable bands. */
bool sr_water_caustic_atlas_queue_layers(SrWaterCausticAtlas **owner, const JcePakArchive *pak,
    uint16_t view, uint32_t frame, const JceScene *scene, JceEntity entity,
    uint8_t primary_reservation, const SrWaterCausticSource sources[SR_WATER_CAUSTIC_BAND_COUNT],
    float dispersion_x, float dispersion_z, SrWaterCausticTile tiles[SR_WATER_CAUSTIC_BAND_COUNT]);

void sr_water_caustic_atlas_bind(const SrWaterCausticAtlas *atlas,
    const SrWaterCausticTile *tile, float still_y);
void sr_water_caustic_atlas_destroy(SrWaterCausticAtlas *atlas);

#endif
