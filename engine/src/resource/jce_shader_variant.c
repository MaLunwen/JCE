/*
 * jce_shader_variant.c  Shader keyword registry + name resolver.
 *
 * Single global registry — keywords are project-wide and small (<= 32).
 * Lookup is linear; with a typical project keyword count of 5–15 this
 * is faster than a hash table.
 */

#include <jce/resource/jce_shader_variant.h>

#include <stdio.h>
#include <string.h>

#define KW_NAME_MAX 32

typedef struct {
    char     name[KW_NAME_MAX];
    bool     valid;
} KwSlot;

static KwSlot s_keywords[JCE_SHADER_KEYWORD_MAX];
static uint32_t s_count = 0;

uint32_t jce_shader_keyword_register(const char *name)
{
    if (!name || !name[0]) return JCE_SHADER_KEYWORD_INVALID;
    /* Existing? */
    for (uint32_t i = 0; i < s_count; ++i) {
        if (s_keywords[i].valid &&
            strncmp(s_keywords[i].name, name, KW_NAME_MAX) == 0)
            return i;
    }
    if (s_count >= JCE_SHADER_KEYWORD_MAX) return JCE_SHADER_KEYWORD_INVALID;
    uint32_t id = s_count++;
    strncpy(s_keywords[id].name, name, KW_NAME_MAX - 1);
    s_keywords[id].name[KW_NAME_MAX - 1] = '\0';
    s_keywords[id].valid = true;
    return id;
}

JceShaderKeywordSet jce_shader_keyword_bit(const char *name)
{
    uint32_t id = jce_shader_keyword_register(name);
    return (id == JCE_SHADER_KEYWORD_INVALID) ? 0u : ((JceShaderKeywordSet)1u << id);
}

const char *jce_shader_keyword_name(uint32_t id)
{
    if (id >= s_count || !s_keywords[id].valid) return NULL;
    return s_keywords[id].name;
}

uint32_t jce_shader_keyword_count(void)
{
    return s_count;
}

JceShaderKeywordSet jce_shader_keyword_set(JceShaderKeywordSet set,
                                            const char *name, bool enabled)
{
    JceShaderKeywordSet bit = jce_shader_keyword_bit(name);
    if (enabled) return set | bit;
    return set & ~bit;
}

uint32_t jce_shader_variant_resolve_name(const char *base,
                                          JceShaderKeywordSet set,
                                          char *out, uint32_t out_size)
{
    if (!base || !out || out_size == 0) return 0;
    /* Base case: no keywords. */
    if (set == 0) {
        size_t blen = strlen(base);
        if (blen + 1 > out_size) return 0;
        memcpy(out, base, blen);
        out[blen] = '\0';
        return (uint32_t)blen;
    }
    /* Walk bits in ascending order so the variant name is canonical
     * regardless of caller toggle order. */
    uint32_t off = 0;
    int n = snprintf(out, out_size, "%s", base);
    if (n < 0 || (uint32_t)n >= out_size) return 0;
    off = (uint32_t)n;
    for (uint32_t i = 0; i < s_count; ++i) {
        if ((set & ((JceShaderKeywordSet)1u << i)) == 0) continue;
        const char *kw = s_keywords[i].name;
        if (!kw) continue;
        n = snprintf(out + off, out_size - off, "_%s", kw);
        if (n < 0 || (uint32_t)n >= out_size - off) return 0;
        off += (uint32_t)n;
    }
    return off;
}
