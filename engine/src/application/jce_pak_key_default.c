/* jce_pak_key_default.c — zeroed default for the embedded asset key shares.
 *
 * jce_engine.c references jce_embedded_pak_key_shares / _present
 * unconditionally (see jce_embedded_assets.h).  Executables that ship an
 * encrypted PAK link an editor-generated jce_generated/jce_pak_key.c as a
 * direct object that OVERRIDES these defaults.
 *
 * The plain default-in-library pattern is NOT enough here: SDK consumers
 * link jce_engine_core with /WHOLEARCHIVE (force_load / --whole-archive),
 * which force-includes this member even when the executable provides its
 * own definition — two strong definitions = LNK2005.  Marking the defaults
 * weak (COMDAT selectany on MSVC) lets a strong executable-side definition
 * win cleanly under whole-archive on every toolchain, while plain in-tree
 * executables (editor, demos, tests) still link with zero per-target wiring.
 *
 * present == 0 means "no key shipped": the engine skips the process-key
 * install and unencrypted assets behave exactly as before.
 */

#include <jce/os/core/jce_defs.h>

/* JCE_WEAK (jce_defs.h) is the portable select-any/weak linkage (COMDAT
 * selectany on MSVC, __attribute__((weak)) elsewhere) — centralised there so
 * this TU carries no raw toolchain branch.  A strong executable-side definition
 * still overrides cleanly under whole-archive on every toolchain. */
JCE_WEAK const unsigned char jce_embedded_pak_key_shares[64] = {0};
JCE_WEAK const int           jce_embedded_pak_key_present    = 0;
