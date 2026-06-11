/* jce_archive_cook.h
 *
 * Build-time helper that "cooks" a set of in-memory resources into a JCE
 * Archive (JPAK v1) blob, applying the spec's differentiated compression
 * strategy in one place: each input is classified by extension (spec
 * §6.2), one shared zstd dictionary is trained per populated text/shader
 * class (spec §7), and every resource is added through the archive writer
 * with its class dictionary (the universal keep-if-helps guard, §6.3,
 * still decides whether compression is kept).
 *
 * This is the single orchestration shared by every producer — the host
 * packer (tools/jce_pak.c), the in-process scene-bundle packer
 * (jce_bundle_pack.c) and the v2->v1 converter — so classification,
 * dictionary policy and determinism stay identical across all of them.
 *
 * Layer: Resource (Layer 3).  Pure C, FFI-safe.  Build-time use; the
 * runtime never calls it.
 */
#ifndef JCE_ARCHIVE_COOK_H
#define JCE_ARCHIVE_COOK_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* One resource to place in the archive.  `vpath` is normalized by the
 * writer; `data`/`size` are the original bytes and are borrowed (copied
 * into the archive during the call). */
typedef struct JceCookInput {
    const char *vpath;
    const void *data;
    size_t      size;
} JceCookInput;

/* Cooking parameters.  Zero-initialize then override; jce_archive_cook()
 * applies sensible defaults for any field left at its zero value. */
typedef struct JceCookConfig {
    int     zstd_level;       /* zstd level (default 19 when 0)             */
    uint8_t alignment_log2;   /* data alignment power-of-two (default 4)    */
    bool    mmap_friendly;    /* page-align uncompressed entries (spec §8)  */
    bool    emit_debug_paths; /* embed the debug path table (names)         */
    bool    compress_index;   /* zstd-compress the index region            */
    bool    use_dict;         /* train + use JSON/TEXT/SHADER dictionaries  */
    bool    dedup_content;    /* coalesce byte-identical payloads (one copy *
                               * on disk; entries share data_offset)        */

    /* ── Optional payload encryption (spec §9.2) ─────────────────────────
     * When `encrypt` is true and `encryption_key` is non-NULL, EVERY input
     * (including any manifest entry) is compressed-then-ChaCha20-encrypted.
     * `encrypt_label` (bundle id / "project_assets") seeds the per-archive
     * nonce salt so the same path in two archives never shares a keystream;
     * NULL/empty selects the legacy zero salt.  Encryption is keyed
     * obfuscation, NOT tamper-proofing: there is no MAC, and the key ships
     * inside the game binary.  Note: dedup_content is effectively disabled
     * for encrypted entries (their per-path nonce makes ciphertext unique). */
    bool           encrypt;
    const uint8_t *encryption_key;  /* 32 bytes, borrowed                  */
    const char    *encrypt_label;   /* nonce-salt label, borrowed          */
} JceCookConfig;

/* Build a JPAK v1 archive from `inputs` into a freshly jce_malloc'd buffer
 * handed back via *out_buf (free with jce_free) and *out_size.  Returns
 * false on error (allocation failure, a hash collision between two
 * distinct paths per spec §12.3, or a writer failure).  When
 * `out_dict_count` is non-NULL it receives the number of dictionaries the
 * cook trained and embedded.  Identical inputs and config yield a
 * byte-identical archive (spec §10.5). */
JCE_API bool jce_archive_cook(const JceCookInput *inputs, size_t count,
                              const JceCookConfig *cfg,
                              void **out_buf, size_t *out_size,
                              uint16_t *out_dict_count);

JCE_EXTERN_C_END

#endif /* JCE_ARCHIVE_COOK_H */
