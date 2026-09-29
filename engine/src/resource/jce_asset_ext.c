/*
 * jce_asset_ext.c  Source-extension -> asset-kind classification.
 *
 * The single authority for "what kind of asset is this file?".  See the
 * contract note on jce_asset_type_from_ext() in <jce/resource/jce_asset_format.h>.
 *
 * The table below is the UNION of the five per-consumer lists that existed
 * before this file (cooker dispatch, runtime texture whitelist, editor asset
 * database, editor texture cache, asset-browser thumbnailer).  Where they
 * disagreed, the broader classification wins: a .webp really is a texture and
 * a .dae really is a model, regardless of whether a given build step happens
 * to support it.  Capability ("can I encode this?") is a separate question
 * each consumer answers for itself.
 */

#include <jce/resource/jce_asset_format.h>

#include <stddef.h>

/* ASCII-only case-insensitive compare.  Deliberately self-contained: this TU
 * is linked into the minimal-source-list host cooker as well as the engine, so
 * it must not pull in jce_str/SDL.  Extensions are ASCII by definition, and an
 * explicit A-Z fold avoids tolower()'s locale dependence. */
static int ext_icmp(const char *a, const char *b)
{
    for (;; ++a, ++b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == '\0') return 0;
    }
}

typedef struct {
    const char *ext;   /* lowercase, no leading dot */
    int         type;  /* JCEASSET_TYPE_* */
} ExtRow;

/* Kept sorted by kind for readability, not for lookup — the list is short
 * enough that a linear scan is cheaper than any index. */
static const ExtRow k_ext_table[] = {
    /* Textures.  dds/ktx/ktx2 are GPU containers; hdr is radiance HDR;
     * psd/gif/webp come from the editor-side tables. */
    { "png",  JCEASSET_TYPE_TEXTURE },
    { "jpg",  JCEASSET_TYPE_TEXTURE },
    { "jpeg", JCEASSET_TYPE_TEXTURE },
    { "bmp",  JCEASSET_TYPE_TEXTURE },
    { "tga",  JCEASSET_TYPE_TEXTURE },
    { "dds",  JCEASSET_TYPE_TEXTURE },
    { "ktx",  JCEASSET_TYPE_TEXTURE },
    { "ktx2", JCEASSET_TYPE_TEXTURE },
    { "hdr",  JCEASSET_TYPE_TEXTURE },
    { "webp", JCEASSET_TYPE_TEXTURE },
    { "psd",  JCEASSET_TYPE_TEXTURE },
    { "gif",  JCEASSET_TYPE_TEXTURE },

    /* Models.  obj/fbx/gltf/glb have first-party importers; the rest are
     * recognised so the editor and cooker agree on what they ARE. */
    { "obj",  JCEASSET_TYPE_MODEL },
    { "fbx",  JCEASSET_TYPE_MODEL },
    { "gltf", JCEASSET_TYPE_MODEL },
    { "glb",  JCEASSET_TYPE_MODEL },
    { "dae",  JCEASSET_TYPE_MODEL },
    { "stl",  JCEASSET_TYPE_MODEL },
    { "ply",  JCEASSET_TYPE_MODEL },
    { "usd",  JCEASSET_TYPE_MODEL },
    { "usdc", JCEASSET_TYPE_MODEL },
    { "usdz", JCEASSET_TYPE_MODEL },

    /* Audio.  opus is the royalty-free preference; m4a/aac are legacy. */
    { "wav",  JCEASSET_TYPE_SOUND },
    { "ogg",  JCEASSET_TYPE_SOUND },
    { "opus", JCEASSET_TYPE_SOUND },
    { "flac", JCEASSET_TYPE_SOUND },
    { "mp3",  JCEASSET_TYPE_SOUND },
    { "m4a",  JCEASSET_TYPE_SOUND },
    { "aac",  JCEASSET_TYPE_SOUND },

    /* Fonts. */
    { "ttf",  JCEASSET_TYPE_FONT },
    { "otf",  JCEASSET_TYPE_FONT },

    /* Shaders.  '.bin' is a compiled bgfx blob; '.sc' is bgfx source. */
    { "sc",   JCEASSET_TYPE_SHADER },
    { "sh",   JCEASSET_TYPE_SHADER },
    { "sb",   JCEASSET_TYPE_SHADER },
    { "bin",  JCEASSET_TYPE_SHADER },
};

/* Return the extension (no dot) of `path`, or NULL when it has none.
 * A bare extension ("png" or ".png") is accepted as-is. */
static const char *ext_of(const char *path)
{
    if (!path || path[0] == '\0') return NULL;

    const char *dot = NULL;
    for (const char *p = path; *p; ++p) {
        if (*p == '.')                       dot = p;
        else if (*p == '/' || *p == '\\')    dot = NULL; /* dot was in a dir */
    }
    if (dot) return (dot[1] != '\0') ? dot + 1 : NULL;

    /* No dot at all: treat the whole string as a bare extension, but only
     * when it looks like one (no separators) — otherwise it is a plain
     * file name with no extension. */
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') return NULL;
    return path;
}

int jce_asset_type_from_ext(const char *path)
{
    const char *ext = ext_of(path);
    if (!ext) return JCEASSET_TYPE_RAW;

    for (size_t i = 0; i < sizeof(k_ext_table) / sizeof(k_ext_table[0]); ++i) {
        if (ext_icmp(ext, k_ext_table[i].ext) == 0)
            return k_ext_table[i].type;
    }
    return JCEASSET_TYPE_RAW;
}

bool jce_asset_ext_is_texture(const char *path)
{
    return jce_asset_type_from_ext(path) == JCEASSET_TYPE_TEXTURE;
}

/* ================================================================== */
/* Script languages                                                    */
/* ================================================================== */

/* A script extension is NOT a JCEASSET_TYPE_*: there is no cooked
 * .jceasset container for a script (it ships as its own bytes), and adding
 * a tenth JCEASSET_TYPE_ tag would change an on-disk enum every existing
 * bundle already encodes.  So scripts get their own table, keyed the same
 * way and answering a different question — see the contract note on
 * jce_asset_script_language_from_ext() in <jce/resource/jce_asset_format.h>.
 *
 * `.class` is java's compiled form and is listed because a shipped Java game
 * carries bytecode, not sources; it differs from the rest ONLY in `form`,
 * which is the single field anything downstream branches on.
 *
 * WHY C++'s ROW IS `.jcecpp` AND NOT `.cpp`.  A C++ script is a CLASS in a
 * compiled native module, so its `scriptPath` names code rather than
 * containing it — but the runtime still has to ROUTE that path to the cpp VM,
 * and jce_script_vm_language_for_path() answers only by extension.  So the
 * backend claims one, and the claim has to be an extension no C++ toolchain
 * uses:
 *
 *   `.cpp` / `.cc` / `.h` / `.hpp` are REFUSED BY CONSTRUCTION.  A row for
 *   any of them would classify every translation unit in the project as an
 *   attachable script — re-opening the exact defect eedff3ea closed, where
 *   the Script picker offered every .cpp and .h in the project — and would
 *   additionally hand the cooker and the publication policy a mandate to pack
 *   the project's C++ SOURCE into the shipped game.
 *
 * `.jcecpp` is engine-namespaced, so it cannot collide with a real build
 * input, and it is the SAME string the backend claims at runtime
 * (scripting/cpp/src/jce_script_vm_cpp.c, jce_script_vm_cpp_register).
 * *Enforced by* (wired into tools/audit/run_architecture_audit.py):
 * check_script_language_catalog.py — a backend
 * that claims an extension this table does not carry, or carries for another
 * language, fails it.
 *
 * Its `form` is REFERENCE, not SOURCE: nothing compiles these bytes.  A
 * project need not create a file at all — the path can be typed into the
 * Script component — but a project that DOES drop a stub beside its other
 * scripts gets the editor's asset browser, the Script picker and the
 * publication policy for free, because all three read this table.
 *
 * WHY `.jcec` IS ITS OWN ROW AND NOT A SECOND SPELLING OF `.jcecpp`.  C is a
 * DRIVER LANGUAGE here, registered as "c" by scripting/c — not a dialect of
 * "cpp" and not an alias of it.  The two share one native class registry
 * (a compiled class has no language at run time; the module ABI is a plain C
 * ABI), and they are still two languages because everything a USER meets is
 * different: the header they include, the toolchain that compiles them, the
 * name the editor prints beside their Script component, and the language
 * every refusal message names.  A row saying `.jcec` -> "cpp" would make the
 * editor tell a C author their script is C++, which is the same
 * name-the-wrong-thing defect `.jcecpp` itself was introduced to end.
 *
 * `.c` and `.h` are REFUSED BY CONSTRUCTION for exactly the reasons `.cpp`
 * and `.hpp` are, one paragraph up, and more sharply: `.h` is also every
 * engine and third-party header a project vendors.  `.jcec` is
 * engine-namespaced, is a distinct string from `.jcecpp` (matching is on the
 * whole text after the last dot, so neither is a prefix of the other for this
 * table), and collides with no build input. */
typedef struct {
    const char *ext;             /* lowercase, no leading dot            */
    const char *language;        /* the jce_script_vm_register() key     */
    const char *representation;  /* JCE_BUNDLE_KEY_REPRESENTATION token  */
    int         form;            /* JCEASSET_SCRIPT_FORM_*               */
} ScriptExtRow;

static const ScriptExtRow k_script_ext_table[] = {
    { "lua",    "lua",    "lua.source",    JCEASSET_SCRIPT_FORM_SOURCE    },
    { "py",     "python", "python.source", JCEASSET_SCRIPT_FORM_SOURCE    },
    { "java",   "java",   "java.source",   JCEASSET_SCRIPT_FORM_SOURCE    },
    { "class",  "java",   "java.class",    JCEASSET_SCRIPT_FORM_BYTECODE  },
    { "jcecpp", "cpp",    "cpp.class-ref", JCEASSET_SCRIPT_FORM_REFERENCE },
    { "jcec",   "c",      "c.class-ref",   JCEASSET_SCRIPT_FORM_REFERENCE },
    /* .jcejs and NOT .js: editor/src/core/jce_assetdb.cpp already classifies
       .js and .ts as project-side WEB TOOLING the engine does not execute, and
       claiming .js would offer every build script in a project as an
       attachable gameplay script and ship it as readable source.  Same reason
       .jcecpp and .jcec exist.  SOURCE, because a .jcejs file IS its text --
       it is evaluated, not referenced the way a compiled native class is. */
    { "jcejs",  "js",     "js.source",     JCEASSET_SCRIPT_FORM_SOURCE    },
    /* .cs, the real extension, and REFERENCE rather than SOURCE.  Nothing
       else claims .cs, and a .cs file in a JCE project IS a gameplay script —
       so unlike .js there is no collision to dodge.  But the engine never
       READS it: `dotnet build` compiled it before the process started and the
       path names the TYPE (Unity's convention, the class matching the file
       name).  SOURCE would put the text in the cooker's shared TEXT
       dictionary "because a VM will compile it", and no VM will. */
    { "cs",     "csharp", "csharp.class-ref",
                                           JCEASSET_SCRIPT_FORM_REFERENCE },
};

static const ScriptExtRow *script_row(const char *path)
{
    const char *ext = ext_of(path);
    if (!ext) return NULL;

    for (size_t i = 0;
         i < sizeof(k_script_ext_table) / sizeof(k_script_ext_table[0]); ++i) {
        if (ext_icmp(ext, k_script_ext_table[i].ext) == 0)
            return &k_script_ext_table[i];
    }
    return NULL;
}

const char *jce_asset_script_language_from_ext(const char *path)
{
    const ScriptExtRow *row = script_row(path);
    return row ? row->language : NULL;
}

const char *jce_asset_script_representation_from_ext(const char *path)
{
    const ScriptExtRow *row = script_row(path);
    return row ? row->representation : NULL;
}

int jce_asset_script_form_from_ext(const char *path)
{
    const ScriptExtRow *row = script_row(path);
    return row ? row->form : JCEASSET_SCRIPT_FORM_NONE;
}
