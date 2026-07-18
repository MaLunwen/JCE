/* jce_aid_json.c -- JSON boundary for the network tiers (spec F.2/F.3/K).
 *
 * This is BOUNDARY code: it runs once at acquisition time and its output
 * is frozen into the record payload, so the solver/T3 determinism rules
 * (no libm etc.) do not bind here -- but the sanitising rules of spec K
 * do:
 *   - out-of-range numeric  -> clamp + count (jce_aid_debug_clamp_count)
 *   - missing field         -> schema default (midpoint / first enum / 0)
 *   - wrong-typed field     -> treated as missing
 *   - extra fields          -> ignored
 *   - unparsable document   -> JCE_AID_ERR_BAD_FORMAT (cascade)         */
#include "jce_aid_internal.h"

#include <cjson/cJSON.h>

static void put_le16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void put_le32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_le64(uint8_t* p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static const char* field_type_name(JceAidFieldType t)
{
    switch (t) {
    case JCE_AID_F_I32:    return "i32";
    case JCE_AID_F_F32Q:   return "f32";
    case JCE_AID_F_ENUM:   return "enum";
    case JCE_AID_F_COLOR8: return "color8";
    case JCE_AID_F_TAGSET: return "tagset";
    default:               return "?";
    }
}

/* duplicate a cJSON-printed string into module memory and free the original */
static char* own_printed(char* printed)
{
    char* out;
    if (!printed) return NULL;
    out = jce_aid_strdup(printed);
    cJSON_free(printed);
    return out;
}

static cJSON* build_request_object(const JceAidSchema* s,
                                   const JceAidContextKV* kv,
                                   uint32_t kv_count, uint32_t budget_ms)
{
    cJSON* root   = cJSON_CreateObject();
    cJSON* schema = cJSON_AddObjectToObject(root, "schema");
    cJSON* fields = cJSON_AddArrayToObject(schema, "fields");
    cJSON* ctx;
    uint16_t i;
    uint32_t k;

    cJSON_AddStringToObject(schema, "name", s->name);
    cJSON_AddNumberToObject(schema, "version", (double)s->version);
    for (i = 0; i < s->field_count; ++i) {
        const JceAidField* f  = &s->fields[i];
        cJSON*             fo = cJSON_CreateObject();
        cJSON_AddStringToObject(fo, "name", f->name);
        cJSON_AddStringToObject(fo, "type", field_type_name(f->type));
        if (f->type == JCE_AID_F_I32) {
            cJSON_AddNumberToObject(fo, "min", (double)f->min_q);
            cJSON_AddNumberToObject(fo, "max", (double)f->max_q);
        } else if (f->type == JCE_AID_F_F32Q) {
            cJSON_AddNumberToObject(fo, "min", (double)f->min_q / 65536.0);
            cJSON_AddNumberToObject(fo, "max", (double)f->max_q / 65536.0);
        } else if (f->type == JCE_AID_F_ENUM || f->type == JCE_AID_F_TAGSET) {
            cJSON*   names = cJSON_AddArrayToObject(fo, "enum");
            uint16_t e;
            for (e = 0; e < f->enum_count; ++e)
                cJSON_AddItemToArray(names,
                                     cJSON_CreateString(f->enum_names[e]));
        }
        cJSON_AddItemToArray(fields, fo);
    }

    ctx = cJSON_AddObjectToObject(root, "context");
    for (k = 0; k < kv_count; ++k)
        if (kv && kv[k].key && kv[k].value)
            cJSON_AddStringToObject(ctx, kv[k].key, kv[k].value);

    cJSON_AddNumberToObject(root, "budget_ms", (double)budget_ms);
    return root;
}

char* jce_aid_json_build_request(const JceAidSchema* s,
                                 const JceAidContextKV* kv, uint32_t kv_count,
                                 uint32_t budget_ms)
{
    cJSON* root = build_request_object(s, kv, kv_count, budget_ms);
    char*  out  = own_printed(cJSON_PrintUnformatted(root));
    cJSON_Delete(root);
    return out;
}

char* jce_aid_json_build_chat_request(const JceAidSchema* s,
                                      const JceAidContextKV* kv,
                                      uint32_t kv_count, const char* model)
{
    cJSON* req  = build_request_object(s, kv, kv_count, 0);
    char*  reqs = own_printed(cJSON_PrintUnformatted(req));
    cJSON* root = cJSON_CreateObject();
    cJSON* msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON* sys  = cJSON_CreateObject();
    cJSON* usr  = cJSON_CreateObject();
    char*  out;

    cJSON_Delete(req);
    cJSON_AddStringToObject(root, "model", model ? model : "llama3");
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content",
        "You are a scene-parameter generator.  Output EXACTLY ONE JSON "
        "object whose keys are the field names of the given schema and "
        "whose values respect the given bounds/enums.  No prose, no "
        "markdown, no additional text.");
    cJSON_AddItemToArray(msgs, sys);
    cJSON_AddStringToObject(usr, "role", "user");
    cJSON_AddStringToObject(usr, "content", reqs ? reqs : "{}");
    cJSON_AddItemToArray(msgs, usr);
    cJSON_AddBoolToObject(root, "stream", 0);

    out = own_printed(cJSON_PrintUnformatted(root));
    if (reqs) jce_aid_free(reqs);
    cJSON_Delete(root);
    return out;
}

/* ---- response parsing (spec K sanitising) ----------------------------- */

static void clamp_note(JceAidState* st) { jce_aid_stat_inc(&st->sanitize_clamps); }

static int32_t sanitize_i32(JceAidState* st, const JceAidField* f,
                            const cJSON* v)
{
    double  d;
    int64_t q;
    if (!v || !cJSON_IsNumber(v))
        return f->min_q + (int32_t)(((int64_t)f->max_q - f->min_q) / 2);
    d = v->valuedouble;
    q = (int64_t)(d >= 0 ? d + 0.5 : d - 0.5);
    if (q < f->min_q) { clamp_note(st); return f->min_q; }
    if (q > f->max_q) { clamp_note(st); return f->max_q; }
    return (int32_t)q;
}

static int32_t sanitize_f32q(JceAidState* st, const JceAidField* f,
                             const cJSON* v)
{
    double  d;
    int64_t q;
    if (!v || !cJSON_IsNumber(v))
        return f->min_q + (int32_t)(((int64_t)f->max_q - f->min_q) / 2);
    d = v->valuedouble * 65536.0;
    q = (int64_t)(d >= 0 ? d + 0.5 : d - 0.5);
    if (q < f->min_q) { clamp_note(st); return f->min_q; }
    if (q > f->max_q) { clamp_note(st); return f->max_q; }
    return (int32_t)q;
}

static uint16_t sanitize_enum(JceAidState* st, const JceAidField* f,
                              const cJSON* v)
{
    if (v && cJSON_IsString(v) && v->valuestring) {
        uint16_t e;
        for (e = 0; e < f->enum_count; ++e)
            if (strcmp(v->valuestring, f->enum_names[e]) == 0) return e;
        clamp_note(st);
        return 0;
    }
    if (v && cJSON_IsNumber(v)) {
        int64_t idx = (int64_t)v->valuedouble;
        if (idx < 0) { clamp_note(st); return 0; }
        if (idx >= f->enum_count) {
            clamp_note(st);
            return (uint16_t)(f->enum_count - 1);
        }
        return (uint16_t)idx;
    }
    return 0; /* missing/wrong type -> first enum (spec K) */
}

static uint32_t sanitize_color(JceAidState* st, const JceAidField* f,
                               const cJSON* v)
{
    (void)f;
    if (v && cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        if (d < 0)            { clamp_note(st); return 0; }
        if (d > 4294967295.0) { clamp_note(st); return 0xFFFFFFFFu; }
        return (uint32_t)d;
    }
    if (v && cJSON_IsString(v) && v->valuestring && v->valuestring[0] == '#') {
        /* "#RRGGBBAA" or "#RRGGBB" (AA=FF) */
        const char* h = v->valuestring + 1;
        uint32_t    acc = 0;
        int         n = 0;
        while (h[n] && n < 8) {
            char c = h[n];
            uint32_t d;
            if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else break;
            acc = (acc << 4) | d;
            n++;
        }
        if (n == 6) return (acc << 8) | 0xFFu;
        if (n == 8) return acc;
        clamp_note(st);
        return 0;
    }
    return 0; /* missing -> 0 */
}

static uint64_t sanitize_tagset(JceAidState* st, const JceAidField* f,
                                const cJSON* v)
{
    const uint64_t mask = (f->enum_count >= 64)
        ? ~0ull : ((1ull << f->enum_count) - 1ull);
    if (v && cJSON_IsNumber(v)) {
        uint64_t bits = (uint64_t)v->valuedouble;
        if (bits & ~mask) { clamp_note(st); bits &= mask; }
        return bits;
    }
    if (v && cJSON_IsArray(v)) {
        uint64_t bits = 0;
        cJSON*   it   = NULL;
        cJSON_ArrayForEach(it, v) {
            if (cJSON_IsString(it) && it->valuestring) {
                uint16_t e;
                int      hit = 0;
                for (e = 0; e < f->enum_count; ++e)
                    if (strcmp(it->valuestring, f->enum_names[e]) == 0) {
                        bits |= 1ull << e;
                        hit = 1;
                        break;
                    }
                if (!hit) clamp_note(st);
            }
        }
        return bits;
    }
    return 0; /* missing -> empty set */
}

static JceAidResult fields_to_payload(const JceAidSchema* s,
                                      const cJSON* fields, uint8_t* payload,
                                      uint16_t* out_len)
{
    JceAidState* st  = jce_aid_state();
    uint32_t     off = 0;
    uint16_t     i;

    for (i = 0; i < s->field_count; ++i) {
        const JceAidField* f = &s->fields[i];
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(fields, f->name);
        switch (f->type) {
        case JCE_AID_F_I32:
            put_le32(payload + off, (uint32_t)sanitize_i32(st, f, v));
            off += 4;
            break;
        case JCE_AID_F_F32Q:
            put_le32(payload + off, (uint32_t)sanitize_f32q(st, f, v));
            off += 4;
            break;
        case JCE_AID_F_ENUM:
            put_le16(payload + off, sanitize_enum(st, f, v));
            off += 2;
            break;
        case JCE_AID_F_COLOR8:
            put_le32(payload + off, sanitize_color(st, f, v));
            off += 4;
            break;
        case JCE_AID_F_TAGSET:
            put_le64(payload + off, sanitize_tagset(st, f, v));
            off += 8;
            break;
        default:
            return JCE_AID_ERR_BAD_FORMAT;
        }
    }
    *out_len = (uint16_t)off;
    return JCE_AID_OK;
}

JceAidResult jce_aid_json_parse_fields(const JceAidSchema* s,
                                       const char* json, size_t len,
                                       uint8_t* payload, uint16_t* out_len)
{
    cJSON*       root;
    const cJSON* fields;
    JceAidResult r;

    if (!json) return JCE_AID_ERR_BAD_FORMAT;
    root = cJSON_ParseWithLength(json, len);
    if (!root) return JCE_AID_ERR_BAD_FORMAT;

    fields = cJSON_GetObjectItemCaseSensitive(root, "fields");
    if (!fields || !cJSON_IsObject(fields)) {
        cJSON_Delete(root);
        return JCE_AID_ERR_BAD_FORMAT;
    }
    r = fields_to_payload(s, fields, payload, out_len);
    cJSON_Delete(root);
    return r;
}

JceAidResult jce_aid_json_parse_chat_fields(const JceAidSchema* s,
                                            const char* json, size_t len,
                                            uint8_t* payload,
                                            uint16_t* out_len)
{
    cJSON*       root;
    const cJSON* choices;
    const cJSON* first;
    const cJSON* message;
    const cJSON* content;
    cJSON*       inner;
    const cJSON* fields;
    JceAidResult r;

    if (!json) return JCE_AID_ERR_BAD_FORMAT;
    root = cJSON_ParseWithLength(json, len);
    if (!root) return JCE_AID_ERR_BAD_FORMAT;

    choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    first   = choices ? cJSON_GetArrayItem((cJSON*)choices, 0) : NULL;
    message = first ? cJSON_GetObjectItemCaseSensitive(first, "message") : NULL;
    content = message ? cJSON_GetObjectItemCaseSensitive(message, "content")
                      : NULL;
    if (!content || !cJSON_IsString(content) || !content->valuestring) {
        cJSON_Delete(root);
        return JCE_AID_ERR_BAD_FORMAT;
    }

    inner = cJSON_Parse(content->valuestring);
    cJSON_Delete(root);
    if (!inner) return JCE_AID_ERR_BAD_FORMAT;

    /* accept either a bare {field:value} object or {"fields":{...}} */
    fields = cJSON_GetObjectItemCaseSensitive(inner, "fields");
    if (!fields || !cJSON_IsObject(fields)) fields = inner;
    if (!cJSON_IsObject(fields)) {
        cJSON_Delete(inner);
        return JCE_AID_ERR_BAD_FORMAT;
    }
    r = fields_to_payload(s, fields, payload, out_len);
    cJSON_Delete(inner);
    return r;
}

uint64_t JCE_CALL jce_aid_debug_clamp_count(void)
{
    JceAidState* st = jce_aid_state();
    return st ? jce_aid_stat_get(&st->sanitize_clamps) : 0;
}
