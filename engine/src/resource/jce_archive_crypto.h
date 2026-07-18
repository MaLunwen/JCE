/*
 * jce_archive_crypto.h  Internal ChaCha20 (RFC 8439) stream cipher for the
 * JCE archive's optional payload encryption (spec §9.2).  NOT part of the
 * public API — encryption keys are configured through jce_archive.h /
 * jce_archive_writer.h.
 *
 * Client-side encryption only raises the effort to extract assets; it cannot
 * make a shipped archive truly secret because the key travels with the client.
 */
#ifndef JCE_ARCHIVE_CRYPTO_H
#define JCE_ARCHIVE_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define JCE_ARCHIVE_KEY_BYTES   32u
#define JCE_ARCHIVE_NONCE_BYTES 12u
#define JCE_ARCHIVE_AUTH_BYTES  32u

typedef struct JceArchiveSecureKeys {
    uint8_t path[JCE_ARCHIVE_KEY_BYTES];
    uint8_t enc[JCE_ARCHIVE_KEY_BYTES];
    uint8_t nonce[JCE_ARCHIVE_KEY_BYTES];
    uint8_t auth[JCE_ARCHIVE_KEY_BYTES];
} JceArchiveSecureKeys;

/* SHA-256 based message authentication used by secure archives.  Kept in the
 * resource-private header so the archive writer/reader share one audited
 * implementation without expanding the public engine API. */
void jce_archive_hmac_sha256(const uint8_t *key, size_t key_len,
                             const uint8_t *data, size_t data_len,
                             uint8_t out[JCE_ARCHIVE_AUTH_BYTES]);

/* Derive independent archive subkeys from one project master key.  salt32 is
 * the existing per-archive label salt stored in the JPAK header. */
void jce_archive_secure_keys_derive(
    const uint8_t master[JCE_ARCHIVE_KEY_BYTES], uint32_t salt32,
    JceArchiveSecureKeys *out);

/* Keyed, dictionary-resistant replacement for the plain XXH3 path index. */
uint64_t jce_archive_secure_path_hash(
    const JceArchiveSecureKeys *keys, const char *normalized_path,
    size_t path_len);

/* Deterministic content nonce.  Exact duplicate compressed payloads produce
 * the same nonce/ciphertext and may share one archive data offset; any changed
 * payload or codec metadata produces a different nonce. */
void jce_archive_secure_nonce(
    const JceArchiveSecureKeys *keys, uint8_t compression, uint16_t dict_id,
    const uint8_t *stored_plaintext, size_t stored_size,
    uint8_t nonce[JCE_ARCHIVE_NONCE_BYTES]);

/* Constant-time authentication-tag comparison and key erasure helpers. */
int  jce_archive_crypto_equal(const uint8_t *a, const uint8_t *b, size_t size);
void jce_archive_crypto_zero(void *ptr, size_t size);

/* XOR `len` bytes of `in` with the ChaCha20 keystream for (key, nonce,
 * initial block counter) into `out`.  `in` and `out` may alias.  This is its
 * own inverse, so the same call both encrypts and decrypts. */
void jce_archive_chacha20_xor(const uint8_t key[JCE_ARCHIVE_KEY_BYTES],
                              const uint8_t nonce[JCE_ARCHIVE_NONCE_BYTES],
                              uint32_t counter,
                              const uint8_t *in, uint8_t *out, size_t len);

/* Derive the per-entry nonce from its (archive-unique) path hash plus a
 * per-archive 32-bit salt, so that no two entries ever reuse a (key, nonce)
 * pair while keeping builds deterministic (spec §1.3 determinism + §9.2
 * stream-cipher safety).  The salt (header nonce_salt32, derived from the
 * archive's encrypt label — bundle id / "project_assets") additionally keeps
 * the SAME path in two DIFFERENT archives from sharing a keystream under one
 * key.  Note the residual risk: rebuilding the same label with the same key
 * still reuses (key, nonce) across versions of one archive — acceptable for
 * asset obfuscation, not for confidentiality. */
void jce_archive_derive_nonce(uint64_t path_hash, uint32_t salt32,
                              uint8_t nonce[JCE_ARCHIVE_NONCE_BYTES]);

#endif /* JCE_ARCHIVE_CRYPTO_H */
