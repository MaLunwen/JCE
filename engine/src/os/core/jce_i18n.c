/*
 * jce_i18n.c  Internationalisation implementation.
 *
 * Loads flat {"key":"value"} JSON files from the PAK archive
 * using the engine JSON facade for correct parsing (Unicode escapes,
 * nested structures, proper error handling).
 */

#include <jce/os/core/jce_i18n.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_pak_loader.h>

#include "jce_memory.h"

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
    { "settings",      JCE_STR_SETTINGS },
    { "video",         JCE_STR_VIDEO },
    { "audio",         JCE_STR_AUDIO },
    { "fullscreen",    JCE_STR_FULLSCREEN },
    { "resolution",    JCE_STR_RESOLUTION },
    { "vsync",         JCE_STR_VSYNC },
    { "master_volume", JCE_STR_MASTER_VOLUME },
    { "music_volume",  JCE_STR_MUSIC_VOLUME },
    { "sfx_volume",    JCE_STR_SFX_VOLUME },
    { "ok",            JCE_STR_OK },
    { "cancel",        JCE_STR_CANCEL },
    { "apply",         JCE_STR_APPLY },
    { "on",            JCE_STR_ON },
    { "off",           JCE_STR_OFF },
};
#define KEY_MAP_COUNT ((int)(sizeof(s_key_map) / sizeof(s_key_map[0])))

/* ------------------------------------------------------------------ */
/* JSON parsing via jce_json                                            */
/* ------------------------------------------------------------------ */

static void parse_json(const char *json_text, JceLang lang)
{
    JceJson *root = jce_json_parse(json_text, 0);
    if (!root) {
        LOG_WARN(LOG_TAG, "JSON parse error");
        return;
    }

    for (int i = 0; i < KEY_MAP_COUNT; i++) {
        const char *val = jce_json_get_string(root, s_key_map[i].key, NULL);
        if (val) {
            SDL_strlcpy(s_strings[lang][s_key_map[i].id], val, MAX_STR);
        }
    }

    jce_json_free(root);
}

static void load_lang(const JcePakArchive *pak, JceLang lang)
{
    const JcePakAsset *asset = jce_pak_find(pak, s_lang_assets[lang]);
    if (!asset) {
        LOG_WARN(LOG_TAG, "missing translation: %s", s_lang_assets[lang]);
        return;
    }

    char *json = (char *)JCE_MALLOC((size_t)asset->original_size + 1);
    if (!json) return;

    size_t n = jce_pak_decompress(asset, json, (size_t)asset->original_size);
    if (n == 0) { JCE_FREE(json); return; }
    json[n] = '\0';

    parse_json(json, lang);
    JCE_FREE(json);

    LOG_DEBUG(LOG_TAG, "loaded %s", s_lang_assets[lang]);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void jce_i18n_init(const JcePakArchive *pak)
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

int jce_i18n_collect_codepoints(uint32_t *buf, int cap)
{
    int count = 0;
    for (int lang = 0; lang < JCE_LANG_COUNT; lang++) {
        for (int str = 0; str < JCE_STR_COUNT; str++) {
            const char *s = s_strings[lang][str];
            if (!s[0]) continue;
            const char *p = s;
            while (*p) {
                Uint32 cp = SDL_StepUTF8(&p, NULL);
                if (cp <= 127) continue;
                int dup = 0;
                for (int i = 0; i < count; i++) {
                    if (buf[i] == cp) { dup = 1; break; }
                }
                if (!dup && count < cap)
                    buf[count++] = (uint32_t)cp;
            }
        }
    }
    return count;
}
