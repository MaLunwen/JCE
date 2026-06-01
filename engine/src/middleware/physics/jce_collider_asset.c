/*
 * jce_collider_asset.c  Serialize / deserialize / instantiate cooked
 *                       compound colliders.
 *
 * Blob layout (little-endian, fixed field order ⇒ deterministic):
 *
 *   off  size  field
 *   0    4     magic 'J','C','O','L'
 *   4    4     version (=1)
 *   8    4     child_count
 *   per child:
 *     4         shape (JceShapeType as u32)
 *     12        position  (3 × f32)
 *     16        rotation  (4 × f32)
 *     12        half_extents (3 × f32)
 *     4         vertex_count
 *     4         index_count
 *     vc*12     vertices (vertex_count × 3 × f32)
 *     ic*4      indices  (index_count × u32)
 */

#include <jce/middleware/physics/jce_collider_asset.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>

#include <string.h>

#define LOG_TAG "collider_asset"

#define JCOL_MAGIC0 'J'
#define JCOL_MAGIC1 'C'
#define JCOL_MAGIC2 'O'
#define JCOL_MAGIC3 'L'
#define JCOL_VERSION 1u

/* ---- little-endian primitive encoders ----------------------------- */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void put_f32(uint8_t *p, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    put_u32(p, bits);
}

static float get_f32(const uint8_t *p)
{
    uint32_t bits = get_u32(p);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

/* Per-child fixed header bytes (everything before the variable arrays). */
#define CHILD_FIXED_BYTES (4u + 12u + 16u + 12u + 4u + 4u)
#define BLOB_HEADER_BYTES (4u + 4u + 4u)

/* ---- serialize ---------------------------------------------------- */

bool jce_collider_serialize(const JceCookedCollider *c,
                            void                   **out_bytes,
                            uint32_t                *out_size)
{
    if (!c || !out_bytes || !out_size) return false;
    *out_bytes = NULL;
    *out_size = 0;

    uint64_t total = BLOB_HEADER_BYTES;
    for (uint32_t i = 0; i < c->child_count; i++) {
        const JceCookedChild *ch = &c->children[i];
        total += CHILD_FIXED_BYTES;
        total += (uint64_t)ch->vertex_count * 3u * 4u;
        total += (uint64_t)ch->index_count * 4u;
    }
    if (total > 0xFFFFFFFFull) {
        LOG_ERROR(LOG_TAG, "cooked collider too large to serialize");
        return false;
    }

    uint8_t *buf = (uint8_t *)jce_malloc((size_t)total);
    if (!buf) return false;

    uint8_t *p = buf;
    p[0] = JCOL_MAGIC0; p[1] = JCOL_MAGIC1;
    p[2] = JCOL_MAGIC2; p[3] = JCOL_MAGIC3;
    p += 4;
    put_u32(p, JCOL_VERSION); p += 4;
    put_u32(p, c->child_count); p += 4;

    for (uint32_t i = 0; i < c->child_count; i++) {
        const JceCookedChild *ch = &c->children[i];
        put_u32(p, (uint32_t)ch->shape); p += 4;
        put_f32(p, ch->position[0]); p += 4;
        put_f32(p, ch->position[1]); p += 4;
        put_f32(p, ch->position[2]); p += 4;
        put_f32(p, ch->rotation[0]); p += 4;
        put_f32(p, ch->rotation[1]); p += 4;
        put_f32(p, ch->rotation[2]); p += 4;
        put_f32(p, ch->rotation[3]); p += 4;
        put_f32(p, ch->half_extents[0]); p += 4;
        put_f32(p, ch->half_extents[1]); p += 4;
        put_f32(p, ch->half_extents[2]); p += 4;
        put_u32(p, ch->vertex_count); p += 4;
        put_u32(p, ch->index_count); p += 4;
        for (uint32_t v = 0; v < ch->vertex_count * 3u; v++) {
            put_f32(p, ch->vertices[v]); p += 4;
        }
        for (uint32_t k = 0; k < ch->index_count; k++) {
            put_u32(p, ch->indices[k]); p += 4;
        }
    }

    *out_bytes = buf;
    *out_size = (uint32_t)total;
    return true;
}

/* ---- deserialize -------------------------------------------------- */

bool jce_collider_deserialize(const void        *bytes,
                              uint32_t           size,
                              JceCookedCollider *out)
{
    if (!bytes || !out) return false;
    out->children = NULL;
    out->child_count = 0;

    const uint8_t *p = (const uint8_t *)bytes;
    if (size < BLOB_HEADER_BYTES) return false;
    if (p[0] != JCOL_MAGIC0 || p[1] != JCOL_MAGIC1 ||
        p[2] != JCOL_MAGIC2 || p[3] != JCOL_MAGIC3) {
        LOG_ERROR(LOG_TAG, "bad collider blob magic");
        return false;
    }
    if (get_u32(p + 4) != JCOL_VERSION) {
        LOG_ERROR(LOG_TAG, "unsupported collider blob version");
        return false;
    }
    uint32_t child_count = get_u32(p + 8);
    uint32_t off = BLOB_HEADER_BYTES;

    JceCookedChild *children = NULL;
    if (child_count > 0) {
        children = (JceCookedChild *)jce_malloc(
            (size_t)child_count * sizeof(JceCookedChild));
        if (!children) return false;
        memset(children, 0, (size_t)child_count * sizeof(JceCookedChild));
    }

    for (uint32_t i = 0; i < child_count; i++) {
        if (off + CHILD_FIXED_BYTES > size) goto fail;
        JceCookedChild *ch = &children[i];
        ch->shape = (uint8_t)get_u32(p + off); off += 4;
        ch->position[0] = get_f32(p + off); off += 4;
        ch->position[1] = get_f32(p + off); off += 4;
        ch->position[2] = get_f32(p + off); off += 4;
        ch->rotation[0] = get_f32(p + off); off += 4;
        ch->rotation[1] = get_f32(p + off); off += 4;
        ch->rotation[2] = get_f32(p + off); off += 4;
        ch->rotation[3] = get_f32(p + off); off += 4;
        ch->half_extents[0] = get_f32(p + off); off += 4;
        ch->half_extents[1] = get_f32(p + off); off += 4;
        ch->half_extents[2] = get_f32(p + off); off += 4;
        ch->vertex_count = get_u32(p + off); off += 4;
        ch->index_count = get_u32(p + off); off += 4;

        uint64_t vbytes = (uint64_t)ch->vertex_count * 3u * 4u;
        uint64_t ibytes = (uint64_t)ch->index_count * 4u;
        if ((uint64_t)off + vbytes + ibytes > size) goto fail;

        if (ch->vertex_count > 0) {
            ch->vertices = (float *)jce_malloc((size_t)vbytes);
            if (!ch->vertices) goto fail;
            for (uint32_t v = 0; v < ch->vertex_count * 3u; v++) {
                ch->vertices[v] = get_f32(p + off); off += 4;
            }
        }
        if (ch->index_count > 0) {
            ch->indices = (uint32_t *)jce_malloc((size_t)ibytes);
            if (!ch->indices) goto fail;
            for (uint32_t k = 0; k < ch->index_count; k++) {
                ch->indices[k] = get_u32(p + off); off += 4;
            }
        }
    }

    out->children = children;
    out->child_count = child_count;
    return true;

fail:
    {
        JceCookedCollider tmp;
        tmp.children = children;
        tmp.child_count = child_count;
        jce_collider_cooked_free(&tmp);
    }
    return false;
}

/* ---- instantiate -------------------------------------------------- */

JceBodyHandle jce_collider_instantiate(JcePhysicsWorld               *world,
                                       const JceCookedCollider       *c,
                                       const JceColliderInstanceDesc *desc)
{
    if (!world || !c || !desc || c->child_count == 0)
        return JCE_BODY_INVALID;

    JceColliderChild *children = (JceColliderChild *)jce_malloc(
        (size_t)c->child_count * sizeof(JceColliderChild));
    if (!children) return JCE_BODY_INVALID;
    memset(children, 0, (size_t)c->child_count * sizeof(JceColliderChild));

    for (uint32_t i = 0; i < c->child_count; i++) {
        const JceCookedChild *src = &c->children[i];
        JceColliderChild *dst = &children[i];
        dst->shape = (JceShapeType)src->shape;
        dst->position.x = src->position[0];
        dst->position.y = src->position[1];
        dst->position.z = src->position[2];
        dst->rotation.x = src->rotation[0];
        dst->rotation.y = src->rotation[1];
        dst->rotation.z = src->rotation[2];
        dst->rotation.w = src->rotation[3];
        dst->half_extents.x = src->half_extents[0];
        dst->half_extents.y = src->half_extents[1];
        dst->half_extents.z = src->half_extents[2];
        dst->vertices = src->vertices;
        dst->vertex_count = src->vertex_count;
        dst->indices = src->indices;
        dst->index_count = src->index_count;
    }

    JceCompoundBodyDesc cd;
    memset(&cd, 0, sizeof(cd));
    cd.type = desc->type;
    cd.position = desc->position;
    cd.rotation = desc->rotation;
    cd.mass = desc->mass;
    cd.friction = desc->friction;
    cd.restitution = desc->restitution;
    cd.linear_damping = desc->linear_damping;
    cd.angular_damping = desc->angular_damping;
    cd.collision_group = desc->collision_group;
    cd.collision_mask = desc->collision_mask;
    cd.is_trigger = desc->is_trigger;
    cd.children = children;
    cd.child_count = c->child_count;

    JceBodyHandle body = jce_physics_body_create_compound(world, &cd);
    jce_free(children);
    return body;
}
