/* jce_pak_key_default.c — zeroed default for the embedded asset key shares.
 *
 * jce_engine.c references jce_embedded_pak_key_shares / _present
 * unconditionally (see jce_embedded_assets.h).  Executables that ship an
 * encrypted PAK link an editor-generated jce_generated/jce_pak_key.c (or the
 * SDK helper's stub) as a direct object, which the linker resolves FIRST;
 * this library member is only pulled in when no such object defines the
 * symbols — the classic default-in-library / override-in-executable pattern.
 * It keeps every in-tree executable (editor, demos, tests) linking without
 * per-target stub wiring.
 *
 * present == 0 means "no key shipped": the engine skips the process-key
 * install and unencrypted assets behave exactly as before.
 */

const unsigned char jce_embedded_pak_key_shares[64] = {0};
const int           jce_embedded_pak_key_present    = 0;
