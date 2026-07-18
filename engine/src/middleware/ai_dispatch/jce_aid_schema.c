/* jce_aid_schema.c -- schema registry: id = hash64("<name>:<version>"),
 * deep-copied entries, growable array. */
#include "jce_aid_internal.h"

#include <stdio.h>

uint32_t jce_aid_field_wire_size(JceAidFieldType t)
{
    switch (t) {
    case JCE_AID_F_I32:
    case JCE_AID_F_F32Q:   return 4;
    case JCE_AID_F_ENUM:   return 2;
    case JCE_AID_F_COLOR8: return 4;
    case JCE_AID_F_TAGSET: return 8;
    default:               return 0;
    }
}

uint32_t jce_aid_schema_payload_size(const JceAidSchema* s)
{
    uint32_t total = 0;
    for (uint16_t i = 0; i < s->field_count; ++i) {
        uint32_t w = jce_aid_field_wire_size(s->fields[i].type);
        if (!w) return 0;
        total += w;
    }
    return total;
}

JceAidSchemaId JCE_CALL jce_aid_schema_id(const char* name, uint16_t version)
{
    char buf[256];
    int  n;
    if (!name) return 0;
    n = snprintf(buf, sizeof(buf), "%s:%u", name, (unsigned)version);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return 0;
    return jce_aid_hash64(buf, (size_t)n);
}

static int field_valid(const JceAidField* f)
{
    if (!f->name || !f->name[0]) return 0;
    switch (f->type) {
    case JCE_AID_F_I32:
    case JCE_AID_F_F32Q:
        return f->min_q <= f->max_q;
    case JCE_AID_F_ENUM:
        return f->enum_count >= 1 && f->enum_names != NULL;
    case JCE_AID_F_TAGSET:
        return f->enum_count >= 1 && f->enum_count <= 64 && f->enum_names != NULL;
    case JCE_AID_F_COLOR8:
        return 1;
    default:
        return 0;
    }
}

static void free_schema_copy(JceAidSchema* s)
{
    if (!s) return;
    for (uint16_t f = 0; s->fields && f < s->field_count; ++f) {
        const JceAidField* fld = &s->fields[f];
        if (fld->enum_names) {
            for (uint16_t e = 0; e < fld->enum_count; ++e)
                jce_aid_free((void*)fld->enum_names[e]);
            jce_aid_free((void*)fld->enum_names);
        }
        jce_aid_free((void*)fld->name);
    }
    jce_aid_free((void*)s->fields);
    jce_aid_free((void*)s->name);
    memset(s, 0, sizeof(*s));
}

void jce_aid_schema_free_all(void)
{
    JceAidState* st = jce_aid_state();
    if (!st) return;
    for (uint32_t i = 0; i < st->slot_count; ++i)
        free_schema_copy(&st->slots[i].schema);
    jce_aid_free(st->slots);
    st->slots = NULL;
    st->slot_count = 0;
    st->slot_cap = 0;
}

static int deep_copy_schema(const JceAidSchema* src, JceAidSchema* dst)
{
    JceAidField* fields;
    memset(dst, 0, sizeof(*dst));
    dst->version     = src->version;
    dst->field_count = src->field_count;
    dst->name        = jce_aid_strdup(src->name);
    fields = (JceAidField*)jce_aid_malloc(sizeof(JceAidField) * src->field_count);
    if (!dst->name || !fields) {
        if (fields) jce_aid_free(fields);
        goto fail;
    }
    memset(fields, 0, sizeof(JceAidField) * src->field_count);
    dst->fields = fields;
    for (uint16_t i = 0; i < src->field_count; ++i) {
        const JceAidField* sf = &src->fields[i];
        fields[i].type       = sf->type;
        fields[i].min_q      = sf->min_q;
        fields[i].max_q      = sf->max_q;
        fields[i].enum_count = sf->enum_count;
        fields[i].name       = jce_aid_strdup(sf->name);
        if (!fields[i].name) goto fail;
        if (sf->enum_count) {
            const char** names =
                (const char**)jce_aid_malloc(sizeof(char*) * sf->enum_count);
            if (!names) goto fail;
            memset(names, 0, sizeof(char*) * sf->enum_count);
            fields[i].enum_names = names;
            for (uint16_t e = 0; e < sf->enum_count; ++e) {
                names[e] = jce_aid_strdup(sf->enum_names[e]);
                if (!names[e]) goto fail;
            }
        }
    }
    return 1;
fail:
    free_schema_copy(dst);
    return 0;
}

JceAidResult JCE_CALL
jce_aid_register_schema(const JceAidSchema* s, JceAidSchemaId* out_id)
{
    JceAidState*   st = jce_aid_state();
    JceAidSchemaId id;
    if (!st) return JCE_AID_ERR_NOT_INIT;
    if (!s || !s->name || !s->name[0] || !s->fields ||
        s->field_count == 0 || s->field_count > JCE_AID_MAX_FIELDS)
        return JCE_AID_ERR_INVALID_ARG;
    for (uint16_t i = 0; i < s->field_count; ++i)
        if (!field_valid(&s->fields[i])) return JCE_AID_ERR_INVALID_ARG;
    if (jce_aid_schema_payload_size(s) > JCE_AID_MAX_PAYLOAD)
        return JCE_AID_ERR_INVALID_ARG;

    id = jce_aid_schema_id(s->name, s->version);
    if (!id) return JCE_AID_ERR_INVALID_ARG;
    if (jce_aid_schema_get(id)) return JCE_AID_ERR_DUPLICATE;

    if (st->slot_count == st->slot_cap) {
        uint32_t          cap = st->slot_cap ? st->slot_cap * 2 : 16;
        JceAidSchemaSlot* ns =
            (JceAidSchemaSlot*)jce_aid_malloc(sizeof(JceAidSchemaSlot) * cap);
        if (!ns) return JCE_AID_ERR_LIMIT;
        if (st->slots) {
            memcpy(ns, st->slots, sizeof(JceAidSchemaSlot) * st->slot_count);
            jce_aid_free(st->slots);
        }
        st->slots    = ns;
        st->slot_cap = cap;
    }

    {
        JceAidSchemaSlot* slot = &st->slots[st->slot_count];
        slot->id = id;
        if (!deep_copy_schema(s, &slot->schema)) return JCE_AID_ERR_LIMIT;
        st->slot_count++;
    }
    if (out_id) *out_id = id;
    return JCE_AID_OK;
}

const JceAidSchema* JCE_CALL jce_aid_schema_get(JceAidSchemaId id)
{
    JceAidState* st = jce_aid_state();
    if (!st || !id) return NULL;
    for (uint32_t i = 0; i < st->slot_count; ++i)
        if (st->slots[i].id == id) return &st->slots[i].schema;
    return NULL;
}
