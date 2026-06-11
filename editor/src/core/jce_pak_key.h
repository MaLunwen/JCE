/*
 * jce_pak_key.h  Project asset-encryption key management (editor core).
 *
 * The key lives at `<project>/.jce/pak_key.hex` — 64 hex chars (32 bytes),
 * generated on first enable via the OS CSPRNG and kept OUT of version
 * control (a `.jce/.gitignore` entry is seeded alongside it, mirroring the
 * bundle packer's `.bundles/.gitignore` precedent).
 *
 * Consumers:
 *   - project open      → jce_pak_key_install_process() pushes the key into
 *                         jce_archive_set_process_key() so the editor itself
 *                         can read encrypted PAKs/bundles of the project.
 *   - bundle browser    → jce_pak_key_load() feeds JceBundlePackOptions.
 *   - build manager     → jce_pak_key_write_shares_c() emits the generated
 *                         TU (two XOR shares, regenerated per build) that
 *                         ships the key inside the game binary.
 *
 * Honesty note: this deters casual extraction only.  The key necessarily
 * ships inside the game binary, there is no MAC (not tamper-proofing), and
 * deterministic builds now depend on the key file's content.
 */

#ifndef JCE_PAK_KEY_H
#define JCE_PAK_KEY_H

#include <cstdint>
#include <string>

/* Absolute path of `<project>/.jce/pak_key.hex`. */
std::string jce_pak_key_path(const std::string &project_root);

/* True when the key file exists and parses to 32 bytes. */
bool jce_pak_key_exists(const std::string &project_root);

/* Load the key (64 hex chars -> 32 bytes).  Returns false when missing or
 * malformed. */
bool jce_pak_key_load(const std::string &project_root, uint8_t out_key[32]);

/* Generate a fresh key via the OS CSPRNG (BCryptGenRandom / /dev/urandom;
 * dev-only time+pointer fallback), write pak_key.hex and seed
 * `.jce/.gitignore`.  Refuses to overwrite an existing key unless
 * `overwrite` is set (regenerating makes previously encrypted archives
 * unreadable).  On failure returns false and fills *err when non-null. */
bool jce_pak_key_generate(const std::string &project_root, bool overwrite,
                          std::string *err);

/* Import an existing 64-hex key file (e.g. from another machine / CI
 * secret) into the project.  Validates before writing. */
bool jce_pak_key_import(const std::string &project_root,
                        const std::string &src_file, std::string *err);

/* XXH3-64 fingerprint of a key (display the first 8 hex digits in UI). */
uint64_t jce_pak_key_fingerprint(const uint8_t key[32]);

/* Convenience: fingerprint of the project's current key.  Returns false
 * when no (valid) key file exists. */
bool jce_pak_key_fingerprint_of(const std::string &project_root,
                                uint64_t *out_fp);

/* Load the project key (if any) and install it process-wide via
 * jce_archive_set_process_key(); clears the process key when the project
 * has none.  Call on project open. */
void jce_pak_key_install_process(const std::string &project_root);

/* Emit the generated C TU carrying TWO 32-byte XOR shares of the key
 * (share_a ^ share_b == key; share_a is fresh CSPRNG output per build, so
 * the byte pattern differs every build) plus the present flag:
 *   const unsigned char jce_embedded_pak_key_shares[64];
 *   const int           jce_embedded_pak_key_present;  (= 1)
 * Returns false (with *err) when the key is missing or the write fails. */
bool jce_pak_key_write_shares_c(const std::string &project_root,
                                const std::string &out_c_path,
                                std::string *err);

#endif /* JCE_PAK_KEY_H */
