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

/* XOR `len` bytes of `in` with the ChaCha20 keystream for (key, nonce,
 * initial block counter) into `out`.  `in` and `out` may alias.  This is its
 * own inverse, so the same call both encrypts and decrypts. */
void jce_archive_chacha20_xor(const uint8_t key[JCE_ARCHIVE_KEY_BYTES],
                              const uint8_t nonce[JCE_ARCHIVE_NONCE_BYTES],
                              uint32_t counter,
                              const uint8_t *in, uint8_t *out, size_t len);

/* Derive the per-entry nonce from its (archive-unique) path hash, so that no
 * two entries ever reuse a (key, nonce) pair while keeping builds
 * deterministic (spec §1.3 determinism + §9.2 stream-cipher safety). */
void jce_archive_derive_nonce(uint64_t path_hash,
                              uint8_t nonce[JCE_ARCHIVE_NONCE_BYTES]);

#endif /* JCE_ARCHIVE_CRYPTO_H */
