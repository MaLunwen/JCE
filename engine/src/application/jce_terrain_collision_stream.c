/*
 * jce_terrain_collision_stream.c -- see the header.
 */

#include "jce_terrain_collision_stream.h"

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define STREAM_MAX_BODIES 256u

struct JceTerrainCollisionStream {
    JceTerrainCollisionStreamDesc desc;

    struct {
        bool              used;
        JceTerrainTileKey key;
        JceBodyHandle     body;
    } slot[STREAM_MAX_BODIES];

    uint32_t cap;         /* effective max_bodies, <= STREAM_MAX_BODIES */
    uint64_t created;
};

JceTerrainCollisionStream *jce_terrain_collision_stream_create(
    const JceTerrainCollisionStreamDesc *desc)
{
    if (!desc || !desc->world) return NULL;
    /* One source or the other, never neither. */
    if (!desc->store && !desc->sample_fn) return NULL;
    if (desc->tiles_x == 0u || desc->tiles_z == 0u) return NULL;
    if (!(desc->tile_world_size > 0.0f)) return NULL;

    JceTerrainCollisionStream *s = (JceTerrainCollisionStream *)
        JCE_CALLOC(1, sizeof(JceTerrainCollisionStream));
    if (!s) return NULL;

    s->desc = *desc;
    if (!(s->desc.radius > 0.0f))
        s->desc.radius = 1.5f * desc->tile_world_size;
    if (!(s->desc.hysteresis > 0.0f))
        s->desc.hysteresis = 0.25f;
    if (!(s->desc.friction > 0.0f))
        s->desc.friction = 0.8f;

    s->cap = desc->max_bodies ? desc->max_bodies : 64u;
    if (s->cap > STREAM_MAX_BODIES) s->cap = STREAM_MAX_BODIES;
    return s;
}

static void stream_drop_slot(JceTerrainCollisionStream *s, uint32_t i)
{
    if (!s->slot[i].used) return;
    jce_physics_body_destroy(s->desc.world, s->slot[i].body);
    memset(&s->slot[i], 0, sizeof(s->slot[i]));
}

void jce_terrain_collision_stream_destroy(JceTerrainCollisionStream *s)
{
    if (!s) return;
    for (uint32_t i = 0; i < STREAM_MAX_BODIES; i++) stream_drop_slot(s, i);
    JCE_FREE(s);
}

static int stream_find(const JceTerrainCollisionStream *s,
                       JceTerrainTileKey key)
{
    for (uint32_t i = 0; i < STREAM_MAX_BODIES; i++)
        if (s->slot[i].used &&
            s->slot[i].key.x == key.x && s->slot[i].key.z == key.z)
            return (int)i;
    return -1;
}

/* Distance from `focus` to the CENTRE of tile (tx,tz), on XZ only: a collider
 * is needed because the player is horizontally near it, not because they are
 * at its altitude. */
static float stream_tile_dist(const JceTerrainCollisionStream *s,
                              uint32_t tx, uint32_t tz, jce_vec3 focus)
{
    const float ts = s->desc.tile_world_size;
    const float cx = s->desc.origin.x + ((float)tx + 0.5f) * ts;
    const float cz = s->desc.origin.z + ((float)tz + 0.5f) * ts;
    const float dx = focus.x - cx;
    const float dz = focus.z - cz;
    return sqrtf(dx * dx + dz * dz);
}

/* Build the body from a caller-supplied height block.  Shared by both sources
 * so the geometry maths exists once -- a second copy is how the store path and
 * the sampler path would end up disagreeing about where the ground is. */
static bool stream_make_body(JceTerrainCollisionStream *s, JceTerrainTileKey key,
                             const float *heights, uint32_t sw, uint32_t sh)
{
    if (!heights || sw < 2u || sh < 2u) return false;

    JceHeightfieldBodyDesc hd;
    memset(&hd, 0, sizeof hd);
    hd.position = jce_v3(
        s->desc.origin.x + (float)key.x * s->desc.tile_world_size,
        s->desc.origin.y,
        s->desc.origin.z + (float)key.z * s->desc.tile_world_size);
    hd.rotation    = jce_q_identity();
    hd.heights     = heights;           /* COPIED by the bridge */
    hd.samples_x   = sw;
    hd.samples_z   = sh;
    /* A tile stores one sample past its own cell range, so the spacing is over
     * (samples - 1) cells.  Dividing by `samples` shrinks every tile and leaves
     * a strip of nothing along its far edge -- invisible at a tile centre and
     * only detectable at a seam. */
    hd.cell_size_x = s->desc.tile_world_size / (float)(sw - 1u);
    hd.cell_size_z = s->desc.tile_world_size / (float)(sh - 1u);
    hd.min_height  = s->desc.min_height;
    hd.max_height  = s->desc.max_height;
    hd.diagonal    = s->desc.diagonal;
    hd.friction    = s->desc.friction;
    hd.restitution = s->desc.restitution;
    hd.smooth_internal_edges = s->desc.smooth_internal_edges;

    const JceBodyHandle body =
        jce_physics_body_create_heightfield(s->desc.world, &hd);
    if (!jce_body_valid(body)) return false;

    for (uint32_t i = 0; i < STREAM_MAX_BODIES; i++) {
        if (s->slot[i].used) continue;
        s->slot[i].used = true;
        s->slot[i].key  = key;
        s->slot[i].body = body;
        s->created++;
        return true;
    }
    /* No free slot: destroy rather than leak a body nothing tracks. */
    jce_physics_body_destroy(s->desc.world, body);
    return false;
}

static bool stream_spawn_sampled(JceTerrainCollisionStream *s,
                                 JceTerrainTileKey key)
{
    const uint32_t span = s->desc.sample_span ? s->desc.sample_span : 33u;
    float *h = (float *)JCE_MALLOC((size_t)span * span * sizeof(float));
    if (!h) return false;

    const float ox = s->desc.origin.x + (float)key.x * s->desc.tile_world_size;
    const float oz = s->desc.origin.z + (float)key.z * s->desc.tile_world_size;

    bool ok = s->desc.sample_fn(s->desc.sample_ctx, key.x, key.z, span,
                                ox, oz, s->desc.tile_world_size, h);
    if (ok) ok = stream_make_body(s, key, h, span, span);
    JCE_FREE(h);
    return ok;
}

static bool stream_spawn(JceTerrainCollisionStream *s, JceTerrainTileKey key)
{
    if (s->desc.sample_fn) return stream_spawn_sampled(s, key);

    JceTerrainTileView view;
    if (!jce_terrain_store_pin_tile(s->desc.store, s->desc.handle, key, &view))
        return false;   /* tile missing or unreadable: no body, no pretence */

    /* Unpin immediately after the body exists -- the bridge copied the
     * samples, so holding the pin would only couple physics residency to the
     * store's byte budget. */
    const bool ok = stream_make_body(s, key, view.heights,
                                     view.sample_width, view.sample_height);
    jce_terrain_store_unpin_tile(s->desc.store, &view);
    return ok;
}

void jce_terrain_collision_stream_update(JceTerrainCollisionStream *s,
                                         jce_vec3 focus)
{
    if (!s) return;

    const float keep = s->desc.radius * (1.0f + s->desc.hysteresis);

    /* Evict first, so the slots freed this tick are available to the tiles
     * that just came into range. */
    for (uint32_t i = 0; i < STREAM_MAX_BODIES; i++) {
        if (!s->slot[i].used) continue;
        if (stream_tile_dist(s, s->slot[i].key.x, s->slot[i].key.z, focus) > keep)
            stream_drop_slot(s, i);
    }

    uint32_t live = 0;
    for (uint32_t i = 0; i < STREAM_MAX_BODIES; i++)
        if (s->slot[i].used) live++;

    for (uint32_t tz = 0; tz < s->desc.tiles_z; tz++) {
        for (uint32_t tx = 0; tx < s->desc.tiles_x; tx++) {
            if (live >= s->cap) return;
            if (stream_tile_dist(s, tx, tz, focus) > s->desc.radius) continue;

            JceTerrainTileKey key = { tx, tz };
            if (stream_find(s, key) >= 0) continue;   /* already resident */
            if (stream_spawn(s, key)) live++;
        }
    }
}

uint32_t jce_terrain_collision_stream_active(const JceTerrainCollisionStream *s)
{
    if (!s) return 0u;
    uint32_t n = 0;
    for (uint32_t i = 0; i < STREAM_MAX_BODIES; i++) if (s->slot[i].used) n++;
    return n;
}

bool jce_terrain_collision_stream_has_tile(const JceTerrainCollisionStream *s,
                                           JceTerrainTileKey key)
{
    return s && stream_find(s, key) >= 0;
}

uint64_t jce_terrain_collision_stream_created(
    const JceTerrainCollisionStream *s)
{
    return s ? s->created : 0u;
}
