/*
 * jce_script_vm_c.c — the "c" JceScriptVM.
 *
 * THE WHOLE BACKEND IS THIS FILE, and it is short on purpose.  "c" is a second
 * registered LANGUAGE over the native class registry that scripting/cpp
 * already owns — not a second implementation of it.  Why the boundary is drawn
 * there, and what follows from one registry serving two languages, is in
 * <jce/script_vm/jce_script_vm_c.h> under SEPARATE LANGUAGE, ONE REGISTRY.
 *
 * ── WHY THE TABLE IS DERIVED AND NOT SPELLED OUT ─────────────────────────
 *
 * Every other backend writes its JceScriptVM as a positional initialiser, and
 * that discipline is right for them: inserting or reordering a member retypes
 * every slot after the insertion point and the build stops.
 *
 * This table's slots are not its own.  Every one of them, except the language
 * and the create function, IS the cpp backend's — so writing them out would
 * create a SECOND list to update when JceScriptVM grows a twentieth slot, and
 * the failure mode of forgetting is a NULL slot: jce_script_vm_register()
 * rejects the table, jce_script_vm_c_register() returns false, and C scripts
 * are refused at runtime in a build that compiled clean.  Copying the cpp
 * table instead makes an appended slot propagate with no edit here at all.
 *
 * The two members that ARE ours are still spelled in the initialiser, because
 * they are this backend's identity and because
 * check_script_language_catalog.py finds a backend's language by
 * reading the initialiser of its JceScriptVM.  A table with no language in it
 * would leave the ".jcec" catalog row reading as an ORPHAN LANGUAGE — the
 * exact "structurally incapable of passing" failure that gate's own header
 * records happening once already, for cpp.
 *
 * ── WHY create_sized CANNOT JUST BE THE cpp ONE ──────────────────────────
 *
 * jce_script_vm_create() refuses a handle whose JceScriptVMHeader does not
 * point at the table it dispatched through — it checks
 * `hdr.vm == e->origin || hdr.vm == &e->vm`, which for the "c" registry entry
 * means &g_c_vm or the registry's copy of it, and the cpp backend's own table
 * is neither.  So this backend needs exactly one function of its own, and
 * jce_script_vm_cpp_create_for() is the primitive that makes it three lines.
 */
#include <jce/script_vm/jce_script_vm_c.h>

#include <jce/os/core/jce_log.h>

#define LOG_TAG "script"

static JceScript *c_create_sized(const JceScriptHost *host, size_t host_size);

/* NOT const: c_vm_derive() fills the rest of it on first use.  The initialiser
 * carries the members that belong to this LANGUAGE and nothing else; see WHY
 * THE TABLE IS DERIVED above for why the other seventeen are absent here
 * rather than copied by hand.
 *
 * The three entries below are, in order: struct_size (0 here; c_vm_derive()
 * takes the cpp table's), the language, and create_sized.
 *
 * NO TRAILING COMMENT BETWEEN THE FIRST COMMA AND THE LANGUAGE, and that is
 * not a style preference — it is measured.
 * check_script_language_catalog.py finds a backend's language by
 * reading the second initialiser of its JceScriptVM, and one placed there
 * made the whole backend invisible to it: the ".jcec" catalog row was
 * reported as an ORPHAN LANGUAGE ("nothing can ever run it") on a tree where
 * the language registered, claimed its extension and ran a script.  The gate
 * has since been taught to read past comments, so this is belt and braces
 * rather than the only guard — but the shape that reads cleanly to BOTH a
 * compiler and a checker costs nothing to keep. */
static JceScriptVM g_c_vm = {
    0,
    JCE_SCRIPT_VM_C_LANGUAGE,
    c_create_sized,
};

static bool g_derived;
static bool g_registered;
static bool g_ext_claimed;

static void c_vm_derive(void)
{
    const JceScriptVM *base;
    JceScriptVM        d;

    if (g_derived) return;

    base = jce_script_vm_cpp();
    if (!base) return;          /* cannot happen; a NULL here would be worse */

    /* Copy EVERY slot, then put ours back.  Written this way round rather than
     * as field-by-field assignment so that a slot added to JceScriptVM is
     * carried automatically — that is the entire reason this is a copy. */
    d = *base;
    d.language     = g_c_vm.language;
    d.create_sized = g_c_vm.create_sized;
    g_c_vm         = d;

    g_derived = true;
}

const JceScriptVM *JCE_CALL jce_script_vm_c(void)
{
    /* Derived on first use, not at registration: a host that asks for the
     * table before registering (to compare addresses, or to read the language)
     * must not get a table of NULLs. */
    c_vm_derive();
    return &g_c_vm;
}

static JceScript *c_create_sized(const JceScriptHost *host, size_t host_size)
{
    /* &g_c_vm and not jce_script_vm_c(): the handle's header must name the
     * address that was passed to jce_script_vm_register(), and by the time
     * anything can call this, that is what &g_c_vm is. */
    return jce_script_vm_cpp_create_for(&g_c_vm, host, host_size);
}

bool JCE_CALL jce_script_vm_c_register(void)
{
    /* TWO HALVES, EACH IDEMPOTENT ON ITS OWN — the same shape as
     * jce_script_vm_cpp_register(), and for the same reason: they fail for
     * unrelated reasons, and a retry must be able to complete the half that
     * did not happen.  One flag for both would let a process whose extension
     * claim was refused report "already registered" forever, with the VM
     * present and nothing routing to it. */
    if (!g_registered) {
        c_vm_derive();
        if (!g_c_vm.struct_size) {
            LOG_ERROR(LOG_TAG, "%s",
                      "script VM 'c' NOT registered: the cpp backend's table "
                      "could not be read, so there is nothing to derive from. "
                      "scripting/c is a second language over that backend's "
                      "native class registry and cannot stand alone.");
            return false;
        }
        if (!jce_script_vm_register(&g_c_vm)) return false;
        g_registered = true;
    }
    if (g_ext_claimed) return true;

    /* ".jcec" — A STRING LITERAL, not JCE_SCRIPT_VM_C_EXTENSION, and the
     * header says why: check_script_language_catalog.py reads this
     * call statically to check it against the row in
     * engine/src/resource/jce_asset_ext.c, and a claim it cannot resolve is
     * reported "could not be checked" — which leaves the row unguarded, which
     * is a packaged-build-only failure (the cooker labels the file "binary",
     * the Script picker never offers it, the publication policy may drop it,
     * and the editor keeps running it from loose files the whole time).
     *
     * NOT ".c" AND NOT ".h", refused by construction for the same reason
     * ".cpp" is: a row for either would classify every translation unit in the
     * project as an attachable script and would tell the cooker to pack the
     * project's SOURCE into the shipped game.  ".h" would additionally sweep
     * in every vendored third-party header.
     *
     * The claim FOLLOWS the registration because a claim naming an
     * unregistered language is refused, which is what keeps the
     * registered-language list a sound test for "is this backend linked".
     * *Enforced by:* tests/scripting/c/test_jce_script_vm_c.c :: "registering
     * the c backend claims .jcec and routes a scriptPath to the c VM". */
    if (!jce_script_vm_register_extension("jcec", "c")) return false;

    g_ext_claimed = true;
    return true;
}
