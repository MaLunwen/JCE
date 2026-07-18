/* jce_pak_key_default.c - zeroed default for the embedded asset key shares.
 *
 * jce_engine.c references jce_embedded_pak_key_shares / _present
 * unconditionally (see jce_embedded_assets.h).  Executables that ship an
 * encrypted PAK link an editor-generated jce_generated/jce_pak_key.c as a
 * direct object.
 *
 * This TU is intentionally built as a standalone static archive and linked
 * normally, never force-loaded with jce_engine_core.  Standard archive member
 * extraction therefore supplies these definitions only when a generated key
 * object has not already resolved the symbols.  This model is deterministic
 * across MSVC, ELF, and Mach-O linkers and needs no weak-symbol extensions.
 *
 * present == 0 means "no key shipped": the engine skips the process-key
 * install and unencrypted assets behave exactly as before.
 */

const unsigned char jce_embedded_pak_key_shares[64] = {0};
const int           jce_embedded_pak_key_present    = 0;
