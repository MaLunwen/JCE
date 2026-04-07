/*
 * jce_i18n.c  Internationalisation implementation.
 *
 * Loads flat {"key":"value"} JSON files from the PAK archive.
 * A minimal JSON parser handles string extraction (no full parser needed).
 */

#include <jce/core/jce_i18n.h>
#include <jce/core/jce_log.h>
#include <jce/resource/pak_loader.h>

#include <SDL3/SDL.h>
#include <string.h>

#define LOG_TAG    "jce_i18n"
#define MAX_STR    512

/* ------------------------------------------------------------------ */
/* Storage                                                              */
/* ------------------------------------------------------------------ */

static JceLang s_lang = JCE_LANG_EN;
static char    s_strings[JCE_LANG_COUNT][JCE_STR_COUNT][MAX_STR];

static const char *s_lang_names[JCE_LANG_COUNT] = { "EN", "ZH" };

static const char *s_lang_assets[JCE_LANG_COUNT] = {
    "i18n/en.json",
    "i18n/zh_cn.json",
};

/* Key name -> JceStringId mapping. */
static const struct { const char *key; JceStringId id; } s_key_map[] = {
    { "paused",        JCE_STR_PAUSED },
    { "continue",      JCE_STR_CONTINUE },
    { "quit",          JCE_STR_QUIT },
    { "controls_hint", JCE_STR_CONTROLS_HINT },
    { "text_demo",     JCE_STR_TEXT_DEMO },
};
#define KEY_MAP_COUNT ((int)(sizeof(s_key_map) / sizeof(s_key_map[0])))

/* ------------------------------------------------------------------ */
/* Minimal JSON string parser                                           */
/* ------------------------------------------------------------------ */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Parse a JSON string starting at the opening '"'.
   Writes content (UTF-8, un-escaped) into buf.
   Returns pointer past the closing '"'. */
static const char *json_str(const char *p, char *buf, int cap)
{
    if (*p != '"') { buf[0] = '\0'; return p; }
    p++;
    int i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p;
            if (c == 'n')       c = '\n';
            else if (c == 't')  c = '\t';
            /* \" and \\ pass through as-is */
            if (i < cap - 1) buf[i++] = c;
        } else {
            if (i < cap - 1) buf[i++] = *p;
        }
        p++;
    }
    buf[i] = '\0';
    if (*p == '"') p++;
    return p;
}

static void parse_json(const char *json, JceLang lang)
{
    const char *p = skip_ws(json);
    if (*p != '{') return;
    p++;

    char key[64], val[MAX_STR];
    while (*p) {
        p = skip_ws(p);
        if (*p == '}' || *p == '\0') break;
        if (*p == ',') { p++; continue; }

        p = json_str(p, key, (int)sizeof(key));
        p = skip_ws(p);
        if (*p == ':') p++;
        p = skip_ws(p);
        p = json_str(p, val, (int)sizeof(val));

        for (int i = 0; i < KEY_MAP_COUNT; i++) {
            if (strcmp(key, s_key_map[i].key) == 0) {
                SDL_strlcpy(s_strings[lang][s_key_map[i].id], val, MAX_STR);
                break;
            }
        }
    }
}

static void load_lang(const PakArchive *pak, JceLang lang)
{
    const PakAsset *asset = pak_find(pak, s_lang_assets[lang]);
    if (!asset) {
        LOG_WARN(LOG_TAG, "missing translation: %s", s_lang_assets[lang]);
        return;
    }

    char *json = (char *)SDL_malloc((size_t)asset->original_size + 1);
    if (!json) return;

    size_t n = pak_decompress(asset, json, (size_t)asset->original_size);
    if (n == 0) { SDL_free(json); return; }
    json[n] = '\0';

    parse_json(json, lang);
    SDL_free(json);

    LOG_DEBUG(LOG_TAG, "loaded %s", s_lang_assets[lang]);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void jce_i18n_init(const PakArchive *pak)
{
    if (!pak) return;
    memset(s_strings, 0, sizeof(s_strings));
    for (int i = 0; i < JCE_LANG_COUNT; i++)
        load_lang(pak, (JceLang)i);
}

void jce_i18n_set_lang(JceLang lang)
{
    if (lang < JCE_LANG_COUNT) s_lang = lang;
}

JceLang jce_i18n_get_lang(void)
{
    return s_lang;
}

const char *jce_i18n_get(JceStringId id)
{
    if (id >= JCE_STR_COUNT) return "";
    if (s_strings[s_lang][id][0])
        return s_strings[s_lang][id];
    /* Fallback to English. */
    if (s_strings[JCE_LANG_EN][id][0])
        return s_strings[JCE_LANG_EN][id];
    return "";
}

const char *jce_i18n_lang_name(void)
{
    return s_lang_names[s_lang];
}

/* ------------------------------------------------------------------ */
/* Codepoint collector                                                  */
/* ------------------------------------------------------------------ */

static uint32_t utf8_next(const char **pp)
{
    const unsigned char *s = (const unsigned char *)*pp;
    uint32_t cp;
    if (s[0] < 0x80) {
        cp = s[0]; *pp += 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *pp += 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12)
           | ((uint32_t)(s[1] & 0x3F) << 6)
           | (s[2] & 0x3F);
        *pp += 3;
    } else if ((s[0] & 0xF8) == 0xF0) {
        cp = ((uint32_t)(s[0] & 0x07) << 18)
           | ((uint32_t)(s[1] & 0x3F) << 12)
           | ((uint32_t)(s[2] & 0x3F) << 6)
           | (s[3] & 0x3F);
        *pp += 4;
    } else {
        cp = '?'; *pp += 1;
    }
    return cp;
}

int jce_i18n_collect_codepoints(uint32_t *buf, int cap)
{
    int count = 0;
    for (int lang = 0; lang < JCE_LANG_COUNT; lang++) {
        for (int str = 0; str < JCE_STR_COUNT; str++) {
            const char *s = s_strings[lang][str];
            if (!s[0]) continue;
            const char *p = s;
            while (*p) {
                uint32_t cp = utf8_next(&p);
                if (cp <= 127) continue;
                int dup = 0;
                for (int i = 0; i < count; i++) {
                    if (buf[i] == cp) { dup = 1; break; }
                }
                if (!dup && count < cap)
                    buf[count++] = cp;
            }
        }
    }
    return count;
}
