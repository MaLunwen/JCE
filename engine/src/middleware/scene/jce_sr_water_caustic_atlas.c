#include "jce_sr_water_caustic_atlas.h"
#include "jce_water_caustic_config.h"
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_shaders.h>
#include "os/core/jce_memory.h"
#include <math.h>

enum {
    GRID = 64,
    GRID_PAD = 16,
    GRID_SIDE = GRID + 2 * GRID_PAD + 1,
    TILE = 128,
    TILE_APRON = 2,
    TILE_STRIDE = TILE + 2 * TILE_APRON,
    TILES_PER_SIDE = 4,
    ATLAS_SIDE = TILE_STRIDE * TILES_PER_SIDE,
    TILE_COUNT = TILES_PER_SIDE * TILES_PER_SIDE,
};

typedef struct CausticTileKey {
    JceEntity entity;
    SrWaterCausticBand band;
    uint64_t revision;
    bgfx_texture_handle_t displacement;
    int resolution;
    float patch;
    float offset_x, offset_z;
} CausticTileKey;

struct SrWaterCausticAtlas {
    bgfx_texture_handle_t texture;
    bgfx_frame_buffer_handle_t target;
    bgfx_program_handle_t program;
    bgfx_uniform_handle_t displacement;
    bgfx_uniform_handle_t projection;
    bgfx_uniform_handle_t tile;
    bgfx_uniform_handle_t channel;
    bgfx_uniform_handle_t consumer_map;
    const JceScene *scene;
    JceMesh *grid;
    uint32_t frame;
    uint16_t view;
    uint8_t primary_reservation;
    uint8_t primary_used, detail_used;
    bool frame_valid;
    bool origin_bottom_left;
    /* Primaries grow from slot 0. Both detail bands share the opposite end. */
    CausticTileKey key[TILE_COUNT];
};

void sr_water_caustic_atlas_destroy(SrWaterCausticAtlas *atlas)
{
    if (!atlas) return;
    if (BGFX_HANDLE_IS_VALID(atlas->target)) bgfx_destroy_frame_buffer(atlas->target);
    if (BGFX_HANDLE_IS_VALID(atlas->texture)) bgfx_destroy_texture(atlas->texture);
    if (BGFX_HANDLE_IS_VALID(atlas->program)) bgfx_destroy_program(atlas->program);
    if (BGFX_HANDLE_IS_VALID(atlas->displacement)) bgfx_destroy_uniform(atlas->displacement);
    if (BGFX_HANDLE_IS_VALID(atlas->projection)) bgfx_destroy_uniform(atlas->projection);
    if (BGFX_HANDLE_IS_VALID(atlas->tile)) bgfx_destroy_uniform(atlas->tile);
    if (BGFX_HANDLE_IS_VALID(atlas->channel)) bgfx_destroy_uniform(atlas->channel);
    if (BGFX_HANDLE_IS_VALID(atlas->consumer_map)) bgfx_destroy_uniform(atlas->consumer_map);
    if (atlas->grid) jce_mesh_destroy(atlas->grid);
    JCE_FREE(atlas);
}

static JceMesh *create_grid(void)
{
    const uint32_t vertex_count = GRID_SIDE * GRID_SIDE;
    const uint32_t index_count = (GRID_SIDE - 1) * (GRID_SIDE - 1) * 6;
    JceMeshVertex *vertices = JCE_CALLOC(vertex_count, sizeof(*vertices));
    uint32_t *indices = JCE_MALLOC(index_count * sizeof(*indices));
    if (!vertices || !indices) {
        JCE_FREE(vertices);
        JCE_FREE(indices);
        return NULL;
    }
    for (int z = 0; z < GRID_SIDE; ++z) {
        for (int x = 0; x < GRID_SIDE; ++x) {
            JceMeshVertex *v = &vertices[z * GRID_SIDE + x];
            v->pos[0] = (float)(x - GRID_PAD) / GRID;
            v->pos[2] = (float)(z - GRID_PAD) / GRID;
            v->normal[1] = 1.0f;
            v->uv[0] = v->pos[0];
            v->uv[1] = v->pos[2];
            v->tangent[0] = v->tangent[3] = 1.0f;
        }
    }
    uint32_t at = 0;
    for (int z = 0; z < GRID_SIDE - 1; ++z) {
        for (int x = 0; x < GRID_SIDE - 1; ++x) {
            const uint32_t a = (uint32_t)(z * GRID_SIDE + x);
            const uint32_t b = a + 1;
            const uint32_t c = a + GRID_SIDE;
            const uint32_t d = c + 1;
            indices[at++] = a;
            indices[at++] = b;
            indices[at++] = c;
            indices[at++] = b;
            indices[at++] = d;
            indices[at++] = c;
        }
    }
    JceMesh *mesh = jce_mesh_create(vertices, vertex_count, indices, index_count);
    JCE_FREE(vertices);
    JCE_FREE(indices);
    return mesh;
}

static SrWaterCausticAtlas *create_atlas(const JcePakArchive *pak)
{
    const uint64_t flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || caps->limits.maxTextureSize < ATLAS_SIDE ||
        !(caps->formats[BGFX_TEXTURE_FORMAT_RGBA16F] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER) ||
        !bgfx_is_texture_valid(0, false, 1, BGFX_TEXTURE_FORMAT_RGBA16F, flags))
        return NULL;

    SrWaterCausticAtlas *a = JCE_CALLOC(1, sizeof(*a));
    if (!a) return NULL;
    a->origin_bottom_left = caps->originBottomLeft;
    a->target.idx = UINT16_MAX;
    a->texture = bgfx_create_texture_2d(ATLAS_SIDE, ATLAS_SIDE, false,
        1, BGFX_TEXTURE_FORMAT_RGBA16F, flags, NULL, 0);
    if (BGFX_HANDLE_IS_VALID(a->texture))
        a->target = bgfx_create_frame_buffer_from_handles(1, &a->texture, false);
    a->program.idx = shader_load_program(pak, "water_caustic").idx;
    a->displacement = bgfx_create_uniform("s_water_disp", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    a->projection = bgfx_create_uniform("u_caustic_project", BGFX_UNIFORM_TYPE_VEC4, 1);
    a->tile = bgfx_create_uniform("u_caustic_tile", BGFX_UNIFORM_TYPE_VEC4, 1);
    a->channel = bgfx_create_uniform("u_caustic_channel", BGFX_UNIFORM_TYPE_VEC4, 1);
    a->consumer_map = bgfx_create_uniform("u_water_caustic_map", BGFX_UNIFORM_TYPE_VEC4, 1);
    a->grid = create_grid();
    if (!BGFX_HANDLE_IS_VALID(a->target) || !BGFX_HANDLE_IS_VALID(a->program) ||
        !BGFX_HANDLE_IS_VALID(a->displacement) || !BGFX_HANDLE_IS_VALID(a->projection) ||
        !BGFX_HANDLE_IS_VALID(a->tile) || !BGFX_HANDLE_IS_VALID(a->channel) || !BGFX_HANDLE_IS_VALID(a->consumer_map) || !a->grid) {
        sr_water_caustic_atlas_destroy(a);
        return NULL;
    }
    return a;
}

static bool displacement_fits_grid(const JceWaterFft *fft, float patch, int n)
{
    const float *dx = jce_water_fft_disp_x_data(fft);
    const float *dz = jce_water_fft_disp_z_data(fft);
    if (!dx || !dz) return false;
    /* The padded lattice draws the neighbouring periods into each apron.
     * Leave two atlas pixels between the admitted displacement and its edge. */
    const float limit = patch * ((float)GRID_PAD / GRID - (float)TILE_APRON / TILE);
    for (int i = 0; i < n * n; ++i)
        if (!(fabsf(dx[i]) <= limit && fabsf(dz[i]) <= limit)) return false;
    return true;
}

static void bind_frame(SrWaterCausticAtlas *a, uint16_t view, uint32_t frame,
    const JceScene *scene, uint8_t primary_reservation)
{
    a->frame_valid = true;
    a->frame = frame;
    a->view = view;
    a->scene = scene;
    a->primary_reservation = primary_reservation > 0 && primary_reservation <= TILE_COUNT
        ? primary_reservation : TILE_COUNT;
    a->primary_used = 0;
    a->detail_used = 0;
    bgfx_set_view_name(view, "Scene/WaterCausticRGB", INT32_MAX);
    bgfx_set_view_frame_buffer(view, a->target);
    bgfx_set_view_rect(view, 0, 0, ATLAS_SIDE, ATLAS_SIDE);
    bgfx_set_view_scissor(view, 0, 0, 0, 0);
    bgfx_set_view_clear(view, BGFX_CLEAR_COLOR, 0, 1.0f, 0);
    bgfx_set_view_mode(view, BGFX_VIEW_MODE_SEQUENTIAL);
}

static void submit_tile(const SrWaterCausticAtlas *a, uint16_t ix, uint16_t iz,
    float inv_patch, float offset_x, float offset_z, bgfx_texture_handle_t displacement)
{
    const bgfx_vertex_buffer_handle_t vertices = {(uint16_t)jce_mesh_get_vbh(a->grid)};
    const bgfx_index_buffer_handle_t indices = {(uint16_t)jce_mesh_get_ibh(a->grid)};
    const float tile[4] = {
        (float)(ix + TILE_APRON) / ATLAS_SIDE,
        (float)(iz + TILE_APRON) / ATLAS_SIDE,
        (float)TILE / ATLAS_SIDE, (float)TILE,
    };
    /* bgfx scissor rectangles use a top-left origin even on OpenGL. The
     * shader atlas UVs follow the texture origin of the active backend. */
    const uint16_t scissor_y = a->origin_bottom_left
        ? (uint16_t)(ATLAS_SIDE - iz - TILE_STRIDE) : iz;
    for (int channel = 0; channel < 3; ++channel) {
        const float sign = 1.0f - (float)channel;
        const float projection[4] = {
            offset_x * inv_patch * sign, offset_z * inv_patch * sign, inv_patch, 0.0f,
        };
        float colour[4] = {0};
        colour[channel] = 1.0f;
        bgfx_set_scissor(ix, scissor_y, TILE_STRIDE, TILE_STRIDE);
        bgfx_set_vertex_buffer(0, vertices, 0, UINT32_MAX);
        bgfx_set_index_buffer(indices, 0, jce_mesh_index_count(a->grid));
        bgfx_set_uniform(a->projection, projection, 1);
        bgfx_set_uniform(a->tile, tile, 1);
        bgfx_set_uniform(a->channel, colour, 1);
        bgfx_set_texture(0, a->displacement, displacement, 0);
        bgfx_set_state(BGFX_STATE_WRITE_RGB |
            BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ONE), 0);
        bgfx_submit(a->view, a->program, 0, BGFX_DISCARD_ALL);
    }
}

static int band_slot(bool primary, int index)
{
    return primary ? index : TILE_COUNT - 1 - index;
}

static int primary_slots_protected(const SrWaterCausticAtlas *a, bool primary)
{
    if (!primary && a->primary_used < a->primary_reservation)
        return a->primary_reservation;
    return a->primary_used;
}

static bool queue_band(SrWaterCausticAtlas **owner, const JcePakArchive *pak,
    uint16_t view, uint32_t frame, const JceScene *scene, JceEntity entity,
    uint8_t primary_reservation, SrWaterCausticBand band, const SrWaterCausticSource *source,
    float offset_x, float offset_z, SrWaterCausticTile *tile)
{
    if (!source->fft || !BGFX_HANDLE_IS_VALID(source->displacement))
        return false;
    const float patch = jce_water_fft_patch_size(source->fft);
    const int resolution = jce_water_fft_resolution(source->fft);
    if (!(patch >= 0.01f && isfinite(patch)) || resolution < 2 || resolution > 1024)
        return false;

    const bool primary = band == SR_WATER_CAUSTIC_PRIMARY;
    SrWaterCausticAtlas *a = *owner;
    const bool same_frame = a && a->frame_valid && a->frame == frame;
    int slot = -1;
    if (same_frame) {
        if (a->view != view || a->scene != scene)
            return false; /* a second view cannot clear this frame's first atlas */
        const int used = primary ? a->primary_used : a->detail_used;
        for (int i = 0; i < used; ++i) {
            const int index = band_slot(primary, i);
            const CausticTileKey *key = &a->key[index];
            if (key->entity != entity || key->band != band) continue;
            if (key->revision != source->revision || key->displacement.idx != source->displacement.idx ||
                key->patch != patch || key->resolution != resolution ||
                key->offset_x != offset_x || key->offset_z != offset_z)
                return false; /* earlier draws already refer to this tile */
            slot = index;
            break;
        }
        if (slot < 0 && primary_slots_protected(a, primary) + a->detail_used >= TILE_COUNT)
            return false;
    }
    if (slot < 0 && !displacement_fits_grid(source->fft, patch, resolution)) return false;
    if (!a) {
        a = create_atlas(pak);
        if (!a) return false;
        *owner = a;
    }
    if (!same_frame) bind_frame(a, view, frame, scene, primary_reservation);
    const bool first = slot < 0;
    if (first) {
        uint8_t *used = primary ? &a->primary_used : &a->detail_used;
        slot = band_slot(primary, (*used)++);
        a->key[slot] = (CausticTileKey){entity, band, source->revision, source->displacement,
                                     resolution, patch, offset_x, offset_z};
    }
    const uint16_t ix = (uint16_t)((slot % TILES_PER_SIDE) * TILE_STRIDE);
    const uint16_t iz = (uint16_t)((slot / TILES_PER_SIDE) * TILE_STRIDE);
    if (first) submit_tile(a, ix, iz, 1.0f / patch, offset_x, offset_z, source->displacement);
    *tile = (SrWaterCausticTile){
        a->texture, (float)(ix + TILE_APRON) / ATLAS_SIDE,
        (float)(iz + TILE_APRON) / ATLAS_SIDE, (float)TILE / ATLAS_SIDE,
    };
    return true;
}

static bool distinct_sources(const SrWaterCausticSource *a, const SrWaterCausticSource *b)
{
    return a->fft && a->fft != b->fft && a->displacement.idx != b->displacement.idx &&
        jce_water_fft_patch_size(a->fft) != jce_water_fft_patch_size(b->fft);
}

bool sr_water_caustic_atlas_queue_layers(SrWaterCausticAtlas **owner, const JcePakArchive *pak,
    uint16_t view, uint32_t frame, const JceScene *scene, JceEntity entity,
    uint8_t primary_reservation, const SrWaterCausticSource sources[SR_WATER_CAUSTIC_BAND_COUNT],
    float dispersion_x, float dispersion_z, SrWaterCausticTile tiles[SR_WATER_CAUSTIC_BAND_COUNT])
{
    if (!tiles) return false;
    const SrWaterCausticTile invalid = {{UINT16_MAX}, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < SR_WATER_CAUSTIC_BAND_COUNT; ++i) tiles[i] = invalid;
    if (!owner || !pak || !scene || !entity || view == UINT16_MAX || !sources ||
        !isfinite(dispersion_x) || !isfinite(dispersion_z)) return false;

    const SrWaterCausticSource *primary = &sources[SR_WATER_CAUSTIC_PRIMARY];
    const SrWaterCausticSource *secondary = &sources[SR_WATER_CAUSTIC_SECONDARY];
    const SrWaterCausticSource *tertiary = &sources[SR_WATER_CAUSTIC_TERTIARY];
    if (!queue_band(owner, pak, view, frame, scene, entity, primary_reservation,
                    SR_WATER_CAUSTIC_PRIMARY, primary, dispersion_x, dispersion_z,
                    &tiles[SR_WATER_CAUSTIC_PRIMARY])) return false;
    if (!distinct_sources(secondary, primary) ||
        !queue_band(owner, pak, view, frame, scene, entity, primary_reservation,
                    SR_WATER_CAUSTIC_SECONDARY, secondary, dispersion_x, dispersion_z,
                    &tiles[SR_WATER_CAUSTIC_SECONDARY])) return true;
    if (distinct_sources(tertiary, primary) && distinct_sources(tertiary, secondary)) {
        (void)queue_band(owner, pak, view, frame, scene, entity, primary_reservation,
                        SR_WATER_CAUSTIC_TERTIARY, tertiary, dispersion_x, dispersion_z,
                        &tiles[SR_WATER_CAUSTIC_TERTIARY]);
    }
    return true;
}

void sr_water_caustic_atlas_bind(const SrWaterCausticAtlas *atlas,
    const SrWaterCausticTile *tile, float still_y)
{
    const float data[4] = {
        tile->uv_origin_x, tile->uv_origin_z, tile->uv_extent, still_y,
    };
    bgfx_set_uniform(atlas->consumer_map, data, 1);
}

typedef struct CausticScan {
    int water_id;
    int caustic_id;
    uint8_t count;
} CausticScan;

static void scan_water(JceScene *scene, JceEntity entity, void *user)
{
    CausticScan *scan = user;
    if (scan->count >= TILE_COUNT ||
        !jce_scene_comp_enabled(scene, entity, scan->water_id)) return;
    const JceWaterComponent *water = jce_scene_get_water(scene, entity);
    if (!water || !water->visible || water->water_mode != JCE_WATER_MODE_FFT ||
        !(water->clarity > 0.0f) || !(water->caustics > 0.0f)) return;
    const JceWaterCausticsComponent *p = jce_scene_get_water_caustics(scene, entity);
    if (jce_water_caustics_valid(p) && jce_scene_comp_enabled(scene, entity, scan->caustic_id))
        ++scan->count;
}

uint8_t sr_water_caustic_atlas_primary_reservation(JceScene *scene)
{
    if (!scene) return 0;
    CausticScan scan = {jce_component_find("Water"), jce_component_find("WaterCaustics"), 0};
    jce_scene_each_water(scene, scan_water, &scan);
    return scan.count;
}
