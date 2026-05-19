/*
 * jce_net_vars.c  Declarative per-field network sync.
 *
 * Wire format is little-endian; tied to host endianness for now
 * (every JCE target is LE).  Caller is responsible for transport.
 */

#include <jce/middleware/net/jce_net_vars.h>

#include <string.h>

static JceNetVarSlot s_slots[JCE_NETVAR_REGISTRY_MAX];
static uint32_t      s_count;

void jce_netvar_clear(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_count = 0;
}

static JceNetVarSlot *find_by_id(uint64_t id)
{
    for (uint32_t i = 0; i < s_count; ++i)
        if (s_slots[i].active && s_slots[i].net_id == id)
            return &s_slots[i];
    return NULL;
}

static size_t type_size(JceNetVarType t)
{
    switch (t) {
    case JCE_NETVAR_BOOL:   return 1;
    case JCE_NETVAR_INT32:
    case JCE_NETVAR_UINT32:
    case JCE_NETVAR_FLOAT:  return 4;
    case JCE_NETVAR_VEC3:   return 12;
    case JCE_NETVAR_QUAT:   return 16;
    case JCE_NETVAR_STRING: return JCE_NETVAR_STRING_LEN;
    case JCE_NETVAR_ENTITY: return 8;
    }
    return 0;
}

uint32_t jce_netvar_register(uint64_t net_id, const char *name,
                              JceNetVarType type,
                              JceNetVarReliability rel, void *value_ptr)
{
    if (!value_ptr) return UINT32_MAX;
    if (find_by_id(net_id)) return UINT32_MAX;
    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0; i < s_count; ++i)
        if (!s_slots[i].active) { slot = i; break; }
    if (slot == UINT32_MAX) {
        if (s_count >= JCE_NETVAR_REGISTRY_MAX) return UINT32_MAX;
        slot = s_count++;
    }
    JceNetVarSlot *s = &s_slots[slot];
    memset(s, 0, sizeof(*s));
    s->net_id      = net_id;
    if (name) strncpy(s->name, name, JCE_NETVAR_NAME_LEN - 1);
    s->type        = type;
    s->reliability = rel;
    s->value_ptr   = value_ptr;
    /* Seed cache so the first collect produces no diff. */
    memcpy(s->cached_value, value_ptr, type_size(type));
    s->dirty       = false;
    s->active      = true;
    return slot;
}

bool jce_netvar_unregister(uint64_t net_id)
{
    JceNetVarSlot *s = find_by_id(net_id);
    if (!s) return false;
    memset(s, 0, sizeof(*s));
    return true;
}

void jce_netvar_mark_dirty(uint64_t net_id)
{
    JceNetVarSlot *s = find_by_id(net_id);
    if (s) s->dirty = true;
}

void jce_netvar_mark_all_dirty(void)
{
    for (uint32_t i = 0; i < s_count; ++i)
        if (s_slots[i].active) s_slots[i].dirty = true;
}

uint32_t jce_netvar_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s_count; ++i) if (s_slots[i].active) n++;
    return n;
}

const JceNetVarSlot *jce_netvar_at(uint32_t idx)
{
    uint32_t seen = 0;
    for (uint32_t i = 0; i < s_count; ++i) {
        if (!s_slots[i].active) continue;
        if (seen == idx) return &s_slots[i];
        seen++;
    }
    return NULL;
}

/* ── Encoding helpers ───────────────────────────────────────── */

static bool write_bytes(uint8_t *out, uint32_t cap, uint32_t *pos,
                          const void *src, uint32_t n)
{
    if (*pos + n > cap) return false;
    memcpy(out + *pos, src, n);
    *pos += n;
    return true;
}

static bool read_bytes(const uint8_t *in, uint32_t len, uint32_t *pos,
                         void *dst, uint32_t n)
{
    if (*pos + n > len) return false;
    memcpy(dst, in + *pos, n);
    *pos += n;
    return true;
}

uint32_t jce_netvar_collect_diffs(uint8_t *out_buf, uint32_t cap,
                                    int reliability_filter)
{
    if (!out_buf || cap < 2) return 0;
    uint32_t pos = 2;          /* reserve 2 bytes for count */
    uint16_t count = 0;
    for (uint32_t i = 0; i < s_count; ++i) {
        JceNetVarSlot *s = &s_slots[i];
        if (!s->active || !s->dirty) continue;
        if (reliability_filter >= 0 &&
            (int)s->reliability != reliability_filter) continue;
        size_t sz = type_size(s->type);
        /* Compare against cache; skip when bit-identical. */
        if (memcmp(s->value_ptr, s->cached_value, sz) == 0) {
            s->dirty = false;
            continue;
        }
        uint8_t tbyte = (uint8_t)s->type;
        if (!write_bytes(out_buf, cap, &pos, &s->net_id, 8)) break;
        if (!write_bytes(out_buf, cap, &pos, &tbyte,  1))    break;
        if (!write_bytes(out_buf, cap, &pos, s->value_ptr, (uint32_t)sz)) break;
        memcpy(s->cached_value, s->value_ptr, sz);
        s->dirty = false;
        count++;
    }
    memcpy(out_buf, &count, 2);
    return pos;
}

bool jce_netvar_apply_incoming(const uint8_t *buf, uint32_t len)
{
    if (!buf || len < 2) return false;
    uint16_t count = 0;
    memcpy(&count, buf, 2);
    uint32_t pos = 2;
    for (uint16_t i = 0; i < count; ++i) {
        uint64_t id;
        uint8_t  tbyte;
        if (!read_bytes(buf, len, &pos, &id,    8)) return false;
        if (!read_bytes(buf, len, &pos, &tbyte, 1)) return false;
        JceNetVarSlot *s = find_by_id(id);
        if (!s) {
            /* Unknown var — skip its bytes. */
            size_t sz = type_size((JceNetVarType)tbyte);
            pos += (uint32_t)sz;
            continue;
        }
        size_t sz = type_size((JceNetVarType)tbyte);
        if (!read_bytes(buf, len, &pos, s->value_ptr, (uint32_t)sz))
            return false;
        memcpy(s->cached_value, s->value_ptr, sz);
    }
    return true;
}
