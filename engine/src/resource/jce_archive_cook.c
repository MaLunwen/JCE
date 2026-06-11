/* jce_archive_cook.c
 *
 * Shared build-time "cook" orchestration: classify resources (spec §6.2),
 * train one shared dictionary per text/shader class (spec §7), and emit a
 * JPAK v1 archive through the engine archive writer.  See the header for
 * the rationale; this is the one place every producer routes through so
 * the compression policy stays identical and deterministic.
 */

#include <jce/resource/jce_archive_cook.h>
#include <jce/resource/jce_archive.h>
#include <jce/resource/jce_archive_writer.h>

#include "os/core/jce_memory.h"

#include <string.h>

/* ================================================================== */
/* Resource classification (spec §6.2)                                 */
/* ================================================================== */

typedef enum {
    CLS_STORE = 0,  /* already entropy-coded media -> rely on keep-if-helps  */
    CLS_JSON,       /* structured text sharing the JSON dictionary           */
    CLS_TEXT,       /* free text sharing the TEXT dictionary                 */
    CLS_SHADER,     /* compiled shader binaries sharing the SHDR dictionary  */
    CLS_DEFAULT,    /* general binary / fallback                             */
    CLS_COUNT
} ResClass;

/* Lowercased extension (without dot) of `path`, or "" when none. */
static void path_ext(const char *path, char *out, size_t out_cap) {
    out[0] = '\0';
    const char *dot = NULL;
    for (const char *p = path; *p; ++p) {
        if (*p == '/') dot = NULL;
        else if (*p == '.') dot = p;
    }
    if (!dot || !dot[1]) return;
    size_t j = 0;
    for (const char *p = dot + 1; *p && j + 1 < out_cap; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[j++] = c;
    }
    out[j] = '\0';
}

static int ext_in(const char *ext, const char *const *set) {
    for (size_t i = 0; set[i]; ++i)
        if (strcmp(ext, set[i]) == 0) return 1;
    return 0;
}

static ResClass classify(const char *path) {
    char ext[32];
    path_ext(path, ext, sizeof(ext));
    if (ext[0] == '\0') return CLS_DEFAULT;

    static const char *const store[] = {
        "png", "jpg", "jpeg", "webp", "ogg", "opus", "mp3",
        "mp4", "webm", "mkv", NULL
    };
    static const char *const json[]   = { "json", "scene", "mat", "gltf", NULL };
    static const char *const text[]   = { "txt", "csv", "xml", "rml", "rcss", "lua", NULL };
    static const char *const shader[] = { "bin", NULL };

    if (ext_in(ext, store))  return CLS_STORE;
    if (ext_in(ext, json))   return CLS_JSON;
    if (ext_in(ext, text))   return CLS_TEXT;
    if (ext_in(ext, shader)) return CLS_SHADER;
    return CLS_DEFAULT;
}

/* ================================================================== */
/* Dictionary training per class (spec §7)                             */
/* ================================================================== */

/* Minimum samples before attempting to train; ZDICT enforces its own
 * floor too, and a class whose training fails is skipped gracefully. */
#define MIN_DICT_SAMPLES 12

static const uint32_t k_class_tag[CLS_COUNT] = {
    0,
    /* JSON   */ JCE_ARCHIVE_TAG('J', 'S', 'O', 'N'),
    /* TEXT   */ JCE_ARCHIVE_TAG('T', 'E', 'X', 'T'),
    /* SHADER */ JCE_ARCHIVE_TAG('S', 'H', 'D', 'R'),
    0
};

/* Train + register a dictionary for `cls`, returning its writer dict_id,
 * or -1 if not applicable / too few samples / training failed. */
static int train_class_dict(JceArchiveWriter *w, const JceCookInput *in,
                            const ResClass *cls_of, size_t count, ResClass cls) {
    if (k_class_tag[cls] == 0) return -1;

    size_t n = 0, total = 0;
    for (size_t i = 0; i < count; ++i)
        if (cls_of[i] == cls && in[i].size > 0) n++;
    if (n < MIN_DICT_SAMPLES) return -1;

    const void **samples = (const void **)JCE_MALLOC(n * sizeof(void *));
    size_t      *sizes   = (size_t *)JCE_MALLOC(n * sizeof(size_t));
    if (!samples || !sizes) { JCE_FREE((void *)samples); JCE_FREE(sizes); return -1; }

    size_t j = 0;
    for (size_t i = 0; i < count; ++i) {
        if (cls_of[i] == cls && in[i].size > 0) {
            samples[j] = in[i].data;
            sizes[j]   = in[i].size;
            total     += in[i].size;
            j++;
        }
    }

    /* Target dict ~1/16 of corpus, clamped to the spec's 16..112 KiB band. */
    size_t target = total / 16;
    if (target < 16 * 1024)  target = 16 * 1024;
    if (target > 112 * 1024) target = 112 * 1024;

    void *dict_buf = JCE_MALLOC(target);
    if (!dict_buf) { JCE_FREE((void *)samples); JCE_FREE(sizes); return -1; }

    size_t dict_size = jce_archive_train_dictionary(samples, sizes, n,
                                                    target, dict_buf, target);
    int dict_id = -1;
    if (dict_size > 0)
        dict_id = jce_archive_writer_add_dictionary(w, k_class_tag[cls],
                                                    dict_buf, dict_size);

    JCE_FREE(dict_buf);
    JCE_FREE((void *)samples);
    JCE_FREE(sizes);
    return dict_id;
}

/* ================================================================== */
/* jce_archive_cook                                                    */
/* ================================================================== */

bool jce_archive_cook(const JceCookInput *inputs, size_t count,
                      const JceCookConfig *cfg,
                      void **out_buf, size_t *out_size,
                      uint16_t *out_dict_count) {
    if (!inputs || !out_buf || !out_size) return false;
    if (out_dict_count) *out_dict_count = 0;

    JceCookConfig def = {0};
    if (cfg) def = *cfg;
    if (def.zstd_level == 0)     def.zstd_level = 19;
    if (def.alignment_log2 == 0) def.alignment_log2 = 4;

    const bool encrypt = def.encrypt && def.encryption_key != NULL;

    JceArchiveWriterConfig wc = {0};
    wc.zstd_level     = def.zstd_level;
    wc.alignment_log2 = def.alignment_log2;
    wc.compress_index = def.compress_index;
    wc.emit_debug_paths = def.emit_debug_paths;
    wc.mmap_friendly  = def.mmap_friendly;
    wc.dedup_content  = def.dedup_content;
    if (encrypt)
        wc.encryption_salt = jce_archive_salt_from_label(def.encrypt_label);

    JceArchiveWriter *w = jce_archive_writer_create(&wc);
    if (!w) return false;
    if (encrypt)
        jce_archive_writer_set_encryption_key(w, def.encryption_key);

    ResClass *cls_of = NULL;
    if (count > 0) {
        cls_of = (ResClass *)JCE_MALLOC(count * sizeof(ResClass));
        if (!cls_of) { jce_archive_writer_destroy(w); return false; }
        for (size_t i = 0; i < count; ++i)
            cls_of[i] = classify(inputs[i].vpath);
    }

    /* Train one dictionary per populated text/shader class (spec §7). */
    int class_dict[CLS_COUNT];
    for (int c = 0; c < CLS_COUNT; ++c) class_dict[c] = -1;
    uint16_t trained = 0;
    if (def.use_dict && count > 0) {
        class_dict[CLS_JSON]   = train_class_dict(w, inputs, cls_of, count, CLS_JSON);
        class_dict[CLS_TEXT]   = train_class_dict(w, inputs, cls_of, count, CLS_TEXT);
        class_dict[CLS_SHADER] = train_class_dict(w, inputs, cls_of, count, CLS_SHADER);
        for (int c = 0; c < CLS_COUNT; ++c)
            if (class_dict[c] >= 0) trained++;
    }

    bool ok = true;
    for (size_t i = 0; i < count && ok; ++i) {
        int dict_id = cls_of ? class_dict[cls_of[i]] : -1;
        /* Encrypt EVERYTHING when requested (incl. manifests): the
         * encrypted add path composes with dictionary compression —
         * compress (optionally with dict) first, then encrypt (§9.2). */
        ok = encrypt
            ? jce_archive_writer_add_encrypted(w, inputs[i].vpath,
                                               inputs[i].data, inputs[i].size, dict_id)
            : ((dict_id >= 0)
                   ? jce_archive_writer_add_with_dict(w, inputs[i].vpath,
                                                      inputs[i].data, inputs[i].size, dict_id)
                   : jce_archive_writer_add(w, inputs[i].vpath,
                                            inputs[i].data, inputs[i].size));
    }

    JCE_FREE(cls_of);

    if (ok) ok = jce_archive_writer_finish(w, out_buf, out_size);
    jce_archive_writer_destroy(w);

    if (ok && out_dict_count) *out_dict_count = trained;
    return ok;
}
