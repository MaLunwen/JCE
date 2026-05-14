/*
 * jce_net_replication.c  Snapshot & RPC wire codec.
 *
 * Implementation notes:
 *   - Wire format is little-endian.  All hosts JCE supports today are
 *     little-endian (x86, x86_64, ARM running LE) so we just memcpy.
 *     If big-endian is ever a target, swap macros go in the read/write
 *     helpers below.
 *   - `mask` is a 32-bit bitmask of present fields.  Decoder skips
 *     unknown bits gracefully so the protocol can grow without
 *     breaking.
 *   - Capacity overrun returns 0 from any encode function, leaving the
 *     output buffer in an undefined-but-bounded state.
 */

#include <jce/middleware/net/jce_net_replication.h>

#include <string.h>

/* ── Wire helpers (little-endian, no alignment requirement) ───────── */

static bool w_u8 (uint8_t  v, uint8_t **p, const uint8_t *end)
{ if (*p >= end) return false; *(*p)++ = v; return true; }

static bool w_u16(uint16_t v, uint8_t **p, const uint8_t *end)
{
    if (*p + 2 > end) return false;
    (*p)[0] = (uint8_t)(v       & 0xFFu);
    (*p)[1] = (uint8_t)((v>>8)  & 0xFFu);
    *p += 2;
    return true;
}

static bool w_u32(uint32_t v, uint8_t **p, const uint8_t *end)
{
    if (*p + 4 > end) return false;
    (*p)[0] = (uint8_t)(v        & 0xFFu);
    (*p)[1] = (uint8_t)((v >> 8) & 0xFFu);
    (*p)[2] = (uint8_t)((v >>16) & 0xFFu);
    (*p)[3] = (uint8_t)((v >>24) & 0xFFu);
    *p += 4;
    return true;
}

static bool w_f32(float f, uint8_t **p, const uint8_t *end)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return w_u32(u, p, end);
}

static bool w_bytes(const uint8_t *src, uint32_t n, uint8_t **p,
                    const uint8_t *end)
{
    if (*p + n > end) return false;
    memcpy(*p, src, n);
    *p += n;
    return true;
}

static bool r_u16(uint16_t *out, const uint8_t **p, const uint8_t *end)
{
    if (*p + 2 > end) return false;
    *out = (uint16_t)((*p)[0] | ((uint16_t)(*p)[1] << 8));
    *p += 2;
    return true;
}

static bool r_u32(uint32_t *out, const uint8_t **p, const uint8_t *end)
{
    if (*p + 4 > end) return false;
    *out = (uint32_t)((*p)[0]
        | ((uint32_t)(*p)[1] << 8)
        | ((uint32_t)(*p)[2] << 16)
        | ((uint32_t)(*p)[3] << 24));
    *p += 4;
    return true;
}

static bool r_f32(float *out, const uint8_t **p, const uint8_t *end)
{
    uint32_t u;
    if (!r_u32(&u, p, end)) return false;
    memcpy(out, &u, 4);
    return true;
}

static bool r_bytes(uint8_t *dst, uint32_t n, const uint8_t **p,
                    const uint8_t *end)
{
    if (*p + n > end) return false;
    memcpy(dst, *p, n);
    *p += n;
    return true;
}

/* ── Per-entity encode (mask + present fields) ────────────────────── */

static bool encode_entity(const JceNetEntitySnap *e,
                          uint8_t **p, const uint8_t *end)
{
    if (!w_u32(e->entity_id, p, end)) return false;
    if (!w_u32(e->mask,      p, end)) return false;
    /* v2: owner_peer_id appended after mask. */
    if (!w_u16(e->owner_peer_id, p, end)) return false;
    if (!w_u16(0u, p, end)) return false; /* pad / reserved */

    if (e->mask & JCE_NET_FIELD_TRANSFORM) {
        if (!w_f32(e->position.x, p, end)) return false;
        if (!w_f32(e->position.y, p, end)) return false;
        if (!w_f32(e->position.z, p, end)) return false;
        if (!w_f32(e->rotation.x, p, end)) return false;
        if (!w_f32(e->rotation.y, p, end)) return false;
        if (!w_f32(e->rotation.z, p, end)) return false;
        if (!w_f32(e->rotation.w, p, end)) return false;
        if (!w_f32(e->scale.x,    p, end)) return false;
        if (!w_f32(e->scale.y,    p, end)) return false;
        if (!w_f32(e->scale.z,    p, end)) return false;
    }
    if (e->mask & JCE_NET_FIELD_VELOCITY) {
        if (!w_f32(e->linear_velocity.x,  p, end)) return false;
        if (!w_f32(e->linear_velocity.y,  p, end)) return false;
        if (!w_f32(e->linear_velocity.z,  p, end)) return false;
        if (!w_f32(e->angular_velocity.x, p, end)) return false;
        if (!w_f32(e->angular_velocity.y, p, end)) return false;
        if (!w_f32(e->angular_velocity.z, p, end)) return false;
    }
    if (e->mask & JCE_NET_FIELD_HEALTH) {
        if (!w_f32(e->health, p, end)) return false;
    }
    if (e->mask & JCE_NET_FIELD_ANIM_STATE) {
        if (!w_bytes((const uint8_t *)e->anim_state, 32, p, end)) return false;
        if (!w_f32(e->anim_time, p, end)) return false;
    }
    if (e->mask & JCE_NET_FIELD_INPUT) {
        for (int i = 0; i < 6; ++i)
            if (!w_f32(e->input[i], p, end)) return false;
    }
    return true;
}

static bool decode_entity(JceNetEntitySnap *e, uint32_t protocol_version,
                          const uint8_t **p, const uint8_t *end)
{
    if (!r_u32(&e->entity_id, p, end)) return false;
    if (!r_u32(&e->mask,      p, end)) return false;
    /* v2 stream carries owner_peer_id + pad after mask.  v1 has none
     * — default to server ownership so legacy senders keep working. */
    if (protocol_version >= JCE_NET_PROTOCOL_VERSION) {
        uint16_t owner = 0, pad = 0;
        if (!r_u16(&owner, p, end)) return false;
        if (!r_u16(&pad,   p, end)) return false;
        e->owner_peer_id = owner;
        e->_pad = pad;
    } else {
        e->owner_peer_id = JCE_NET_PEER_SERVER;
        e->_pad = 0;
    }

    if (e->mask & JCE_NET_FIELD_TRANSFORM) {
        if (!r_f32(&e->position.x, p, end)) return false;
        if (!r_f32(&e->position.y, p, end)) return false;
        if (!r_f32(&e->position.z, p, end)) return false;
        if (!r_f32(&e->rotation.x, p, end)) return false;
        if (!r_f32(&e->rotation.y, p, end)) return false;
        if (!r_f32(&e->rotation.z, p, end)) return false;
        if (!r_f32(&e->rotation.w, p, end)) return false;
        if (!r_f32(&e->scale.x,    p, end)) return false;
        if (!r_f32(&e->scale.y,    p, end)) return false;
        if (!r_f32(&e->scale.z,    p, end)) return false;
    }
    if (e->mask & JCE_NET_FIELD_VELOCITY) {
        if (!r_f32(&e->linear_velocity.x,  p, end)) return false;
        if (!r_f32(&e->linear_velocity.y,  p, end)) return false;
        if (!r_f32(&e->linear_velocity.z,  p, end)) return false;
        if (!r_f32(&e->angular_velocity.x, p, end)) return false;
        if (!r_f32(&e->angular_velocity.y, p, end)) return false;
        if (!r_f32(&e->angular_velocity.z, p, end)) return false;
    }
    if (e->mask & JCE_NET_FIELD_HEALTH) {
        if (!r_f32(&e->health, p, end)) return false;
    }
    if (e->mask & JCE_NET_FIELD_ANIM_STATE) {
        if (!r_bytes((uint8_t *)e->anim_state, 32, p, end)) return false;
        e->anim_state[31] = '\0';
        if (!r_f32(&e->anim_time, p, end)) return false;
    }
    if (e->mask & JCE_NET_FIELD_INPUT) {
        for (int i = 0; i < 6; ++i)
            if (!r_f32(&e->input[i], p, end)) return false;
    }
    return true;
}

/* ── Snapshot encode / decode ────────────────────────────────────── */

uint32_t jce_net_snapshot_max_bytes(uint32_t entity_count)
{
    /* Header (12) + per-entity worst case (8 + transform 40 + velocity 24
     * + health 4 + anim 36 + input 24 = 136). */
    const uint32_t kHeader = 12;
    /* Per-entity worst case: 8 (id+mask) + 4 (owner+pad, v2)
     * + transform 40 + velocity 24 + health 4 + anim 36 + input 24. */
    const uint32_t kPerEntityWorst = 8 + 4 + 40 + 24 + 4 + 36 + 24;
    return kHeader + entity_count * kPerEntityWorst;
}

uint32_t jce_net_snapshot_encode(const JceNetEntitySnap *entities,
                                  uint32_t count,
                                  uint32_t server_tick,
                                  uint8_t *out, uint32_t out_capacity)
{
    if (!entities && count > 0) return 0;
    if (!out || out_capacity == 0) return 0;
    uint8_t *p = out;
    const uint8_t *end = out + out_capacity;
    if (!w_u32(JCE_NET_PROTOCOL_VERSION, &p, end)) return 0;
    if (!w_u16((uint16_t)count, &p, end)) return 0;
    if (!w_u16(0u, &p, end)) return 0;
    if (!w_u32(server_tick, &p, end)) return 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!encode_entity(&entities[i], &p, end)) return 0;
    return (uint32_t)(p - out);
}

uint32_t jce_net_snapshot_decode(const uint8_t *in, uint32_t size,
                                  uint32_t *out_server_tick,
                                  JceNetEntitySnap *out_entities,
                                  uint32_t max_entities)
{
    if (!in || size < 12) return 0;
    const uint8_t *p = in;
    const uint8_t *end = in + size;
    uint32_t version = 0;
    uint16_t entity_count = 0;
    uint16_t reserved = 0;
    uint32_t tick = 0;
    if (!r_u32(&version, &p, end)) return 0;
    /* Accept v1 (legacy) and v2 (current) wire blobs.  v1 entries
     * default owner_peer_id to JCE_NET_PEER_SERVER on the receive
     * side so legacy senders interoperate. */
    if (version != JCE_NET_PROTOCOL_VERSION
        && version != JCE_NET_PROTOCOL_V1) return 0;
    if (!r_u16(&entity_count, &p, end)) return 0;
    if (!r_u16(&reserved, &p, end))    return 0;
    if (!r_u32(&tick, &p, end))        return 0;
    if (out_server_tick) *out_server_tick = tick;

    uint32_t to_write = entity_count > max_entities ? max_entities : entity_count;
    for (uint32_t i = 0; i < to_write; ++i) {
        memset(&out_entities[i], 0, sizeof(out_entities[i]));
        if (!decode_entity(&out_entities[i], version, &p, end)) return 0;
    }
    /* Skip remaining entities the caller couldn't accommodate so the
     * decoder leaves `p` at the end-of-buffer position. */
    for (uint32_t i = to_write; i < entity_count; ++i) {
        JceNetEntitySnap discard;
        memset(&discard, 0, sizeof(discard));
        if (!decode_entity(&discard, version, &p, end)) return 0;
    }
    return to_write;
}

/* ── Delta encoding ──────────────────────────────────────────────── */

static bool entity_field_differs(const JceNetEntitySnap *a,
                                 const JceNetEntitySnap *b,
                                 uint32_t field_bit)
{
    /* Only bother diffing when both have the field set; missing
     * field implies "no information sent". */
    if ((a->mask & field_bit) == 0 && (b->mask & field_bit) == 0) return false;
    if ((a->mask & field_bit) != (b->mask & field_bit)) return true;
    /* memcmp on the relevant subset. */
    switch (field_bit) {
        case JCE_NET_FIELD_TRANSFORM:
            return memcmp(&a->position, &b->position, sizeof(a->position)) != 0
                || memcmp(&a->rotation, &b->rotation, sizeof(a->rotation)) != 0
                || memcmp(&a->scale,    &b->scale,    sizeof(a->scale))    != 0;
        case JCE_NET_FIELD_VELOCITY:
            return memcmp(&a->linear_velocity,  &b->linear_velocity,  sizeof(a->linear_velocity))  != 0
                || memcmp(&a->angular_velocity, &b->angular_velocity, sizeof(a->angular_velocity)) != 0;
        case JCE_NET_FIELD_HEALTH:    return a->health != b->health;
        case JCE_NET_FIELD_ANIM_STATE:
            return strcmp(a->anim_state, b->anim_state) != 0
                || a->anim_time != b->anim_time;
        case JCE_NET_FIELD_INPUT:
            return memcmp(a->input, b->input, sizeof(a->input)) != 0;
        default: return false;
    }
}

uint32_t jce_net_snapshot_encode_delta(const JceNetEntitySnap *previous,
                                        uint32_t prev_count,
                                        const JceNetEntitySnap *current,
                                        uint32_t cur_count,
                                        uint32_t server_tick,
                                        uint8_t *out, uint32_t out_capacity)
{
    if (!out || out_capacity < 12) return 0;

    /* Build a temporary work buffer of "snap entries to send".  Caller
     * promises lists are sorted by entity_id; merge-walk them once.
     * We emit:
     *   - removed: entity in prev but not cur → mask=0
     *   - added:   entity in cur  but not prev → full mask present
     *   - changed: entity in both, any field differs → only changed bits
     */
    /* Reserve space inline rather than allocate.  Worst case is
     * prev_count + cur_count entries; cap at 4096 to keep the stack
     * frame finite — projects with more entities should chunk. */
    enum { kCap = 4096 };
    JceNetEntitySnap diff[kCap];
    uint32_t diff_n = 0;

    uint32_t i = 0, j = 0;
    while ((i < prev_count || j < cur_count) && diff_n < kCap) {
        const JceNetEntitySnap *a = (i < prev_count) ? &previous[i] : NULL;
        const JceNetEntitySnap *b = (j < cur_count)  ? &current[j]  : NULL;
        uint32_t aid = a ? a->entity_id : UINT32_MAX;
        uint32_t bid = b ? b->entity_id : UINT32_MAX;

        if (aid < bid) {
            /* Removed. */
            JceNetEntitySnap r = *a;
            r.mask = 0;
            diff[diff_n++] = r;
            i++;
        } else if (bid < aid) {
            /* Added — send full present mask. */
            diff[diff_n++] = *b;
            j++;
        } else {
            /* Same id — diff fields. */
            uint32_t fields[5] = {
                JCE_NET_FIELD_TRANSFORM, JCE_NET_FIELD_VELOCITY,
                JCE_NET_FIELD_HEALTH,    JCE_NET_FIELD_ANIM_STATE,
                JCE_NET_FIELD_INPUT
            };
            uint32_t mask = 0;
            for (int k = 0; k < 5; ++k)
                if (entity_field_differs(a, b, fields[k]))
                    mask |= fields[k] & b->mask;
            if (mask != 0) {
                JceNetEntitySnap entry = *b;
                entry.mask = mask;
                diff[diff_n++] = entry;
            }
            i++; j++;
        }
    }

    return jce_net_snapshot_encode(diff, diff_n, server_tick, out, out_capacity);
}

/* ── RPC ─────────────────────────────────────────────────────────── */

uint32_t jce_net_rpc_encode(const JceNetRpc *rpc,
                             uint8_t *out, uint32_t out_capacity)
{
    if (!rpc || !out) return 0;
    uint8_t *p = out;
    const uint8_t *end = out + out_capacity;
    if (!w_u32(JCE_NET_PROTOCOL_VERSION, &p, end)) return 0;
    if (!w_u32(rpc->rpc_id,               &p, end)) return 0;
    if (!w_u32(rpc->sender,               &p, end)) return 0;
    if (!w_u16((uint16_t)rpc->channel,    &p, end)) return 0;
    if (!w_u16(rpc->payload_size,         &p, end)) return 0;
    if (rpc->payload_size > 0 && rpc->payload) {
        if (!w_bytes(rpc->payload, rpc->payload_size, &p, end)) return 0;
    }
    return (uint32_t)(p - out);
}

bool jce_net_rpc_decode(const uint8_t *in, uint32_t size, JceNetRpc *out_rpc)
{
    if (!in || !out_rpc || size < 16) return false;
    const uint8_t *p = in;
    const uint8_t *end = in + size;
    uint32_t version = 0;
    if (!r_u32(&version, &p, end)) return false;
    if (version != JCE_NET_PROTOCOL_VERSION) return false;
    uint16_t channel = 0;
    if (!r_u32(&out_rpc->rpc_id, &p, end)) return false;
    if (!r_u32(&out_rpc->sender, &p, end)) return false;
    if (!r_u16(&channel, &p, end))         return false;
    out_rpc->channel = (JceNetChannel)channel;
    if (!r_u16(&out_rpc->payload_size, &p, end)) return false;
    if (out_rpc->payload_size > 0) {
        if (p + out_rpc->payload_size > end) return false;
        out_rpc->payload = p;  /* points into caller buffer */
    } else {
        out_rpc->payload = NULL;
    }
    return true;
}

bool jce_net_should_replicate(const JceNetEntitySnap *snap, uint16_t local_peer)
{
    if (!snap) return false;
    /* The server (sentinel local_peer) always replicates. */
    if (local_peer == JCE_NET_PEER_SERVER) return true;
    /* Client only writes its own entities. */
    return snap->owner_peer_id == local_peer;
}
