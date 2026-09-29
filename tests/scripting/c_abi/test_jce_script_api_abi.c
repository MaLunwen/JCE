/*
 * test_jce_script_api_abi.c — the script-facing shared library, tested the way
 * a binding will actually use it: dlopen/LoadLibrary by absolute path, then
 * resolve every entry point BY NAME.
 *
 * The load-bearing test in this file is not "the scripting functions are
 * exported".  It is test_the_dll_exports_nothing_else.  Owner decision 7 rests
 * a security property on the export table —
 *
 *     "a script cannot reach past the scripting surface because the surface
 *      is all the shared object exports"
 *
 * — and a test that checks only what IS exported tests the wrong half of it.
 * So this file reads the real export directory out of the built object and
 * asserts SET EQUALITY against the generated manifest list, in both
 * directions, naming the offender either way.
 *
 * Both oracles are here on purpose and they are independent:
 *   * the file parser reads the PE export directory off disk;
 *   * GetProcAddress / dlsym asks the LOADER, which is what a binding does.
 * A leak that one is blind to would have to be invisible to the other too.
 */

#include "unity.h"

#include <jce/middleware/script/jce_script.h>
#include <jce/script_api/jce_script_api.h>

#include "jce_script_api_exports.gen.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#ifndef JCE_SCRIPT_API_DLL_PATH
#error "JCE_SCRIPT_API_DLL_PATH must name the built shared library"
#endif

/* ------------------------------------------------------------------ *
 *  Loading the library exactly as a ctypes / JNA binding would.
 * ------------------------------------------------------------------ */

typedef void (*api_fn)(void);

static void *g_lib;

static void *load_library_once(void)
{
    if (g_lib)
        return g_lib;
#if defined(_WIN32)
    g_lib = (void *)LoadLibraryA(JCE_SCRIPT_API_DLL_PATH);
#else
    g_lib = dlopen(JCE_SCRIPT_API_DLL_PATH, RTLD_NOW | RTLD_LOCAL);
#endif
    TEST_ASSERT_NOT_NULL_MESSAGE(g_lib,
        "could not load " JCE_SCRIPT_API_DLL_PATH);
    return g_lib;
}

static api_fn sym(const char *name)
{
    void *lib = load_library_once();
#if defined(_WIN32)
    return (api_fn)GetProcAddress((HMODULE)lib, name);
#else
    api_fn f;
    *(void **)&f = dlsym(lib, name);
    return f;
#endif
}

/* Resolve-then-assert, for every symbol this file is about to CALL.
 *
 * Found by mutation M2 (one name deleted from the generated .def): with a bare
 * sym(), the missing export made the first call jump through NULL and the
 * process died with no output at all — not even the named failures the earlier
 * tests had already produced, because stdout was still buffered.  A gate whose
 * failure mode is a silent segfault is a gate that says nothing about WHY. */
static api_fn must_sym(const char *name)
{
    api_fn f = sym(name);
    if (!f) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "%s does not resolve in the DLL — it is not exported", name);
        TEST_FAIL_MESSAGE(msg);
    }
    return f;
}

typedef uint32_t (*fn_version)(void);
typedef JceScriptApi *(*fn_open)(const JceScriptHost *, size_t, uint32_t);
typedef void (*fn_close)(JceScriptApi *);
typedef bool (*fn_get_position)(JceScriptApi *, JceScriptEntity, float *);
typedef void (*fn_set_position)(JceScriptApi *, JceScriptEntity,
                                float, float, float);
typedef void (*fn_move_axis)(JceScriptApi *, float *);
typedef JceScriptEntity (*fn_find_with_tag)(JceScriptApi *, const char *);
typedef int (*fn_raycast_all)(JceScriptApi *, const float *, const float *,
                              float, uint32_t, bool, JceScriptEntity *, int);
typedef bool (*fn_curve_eval)(JceScriptApi *, const char *,
                              const char *, double, double *);
/* The manifest's TAIL entry, whose prototype the short-host test must call
 * exactly.  Retyped when the tail moves; the assert in that test says so. */
typedef void (*fn_ui_set_progress)(JceScriptApi *, JceScriptEntity, float);
typedef void (*fn_ui_set_scroll)(JceScriptApi *, JceScriptEntity, float,
                                 float);
typedef float (*fn_world_get_wind_speed)(JceScriptApi *);
typedef bool  (*fn_is_transitioning)(JceScriptApi *);
typedef bool  (*fn_audio_is_playing)(JceScriptApi *, uint64_t);
typedef bool  (*fn_load_game)(JceScriptApi *, const char *);
typedef int   (*fn_vcam_activate)(JceScriptApi *, const char *);
typedef const char *(*fn_get_param_text)(JceScriptApi *, JceScriptEntity,
                                         const char *);
typedef int   (*fn_overlap_box)(JceScriptApi *, float, float, float,
                                float, float, float, uint32_t,
                                uint64_t *, int);
typedef bool (*fn_button)(JceScriptApi *);
typedef const char *(*fn_tr)(JceScriptApi *, const char *);
typedef float (*fn_music_transition)(JceScriptApi *, int);
typedef int (*fn_touch_count)(JceScriptApi *);
typedef int (*fn_comp_get)(JceScriptApi *, JceScriptEntity, const char *,
                           char *, int);
typedef void (*fn_set_locale)(JceScriptApi *, const char *);

/* ------------------------------------------------------------------ *
 *  Reading the export table out of the built object.
 * ------------------------------------------------------------------ */

#if defined(_WIN32)

static unsigned char *slurp(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* PE export names, read from the FILE — not from the loaded module, so that a
 * loader that resolved a name some other way cannot make this look clean. */
static char **read_exported_names(size_t *out_count, const char **err)
{
    size_t size = 0, i;
    unsigned char *buf = slurp(JCE_SCRIPT_API_DLL_PATH, &size);
    const unsigned char *pe, *opt, *sections, *dir;
    uint32_t e_lfanew, exp_rva, dd_off, names_rva, count, off, names_off;
    uint16_t nsec, opt_size, magic, s;
    char **names;

    *out_count = 0;
    if (!buf) { *err = "could not read the DLL"; return NULL; }
    if (size < 0x40u || buf[0] != 'M' || buf[1] != 'Z') {
        free(buf); *err = "not a PE image"; return NULL;
    }
    e_lfanew = rd32(buf + 0x3C);
    if ((size_t)e_lfanew + 0x18u > size) {
        free(buf); *err = "truncated PE header"; return NULL;
    }
    pe = buf + e_lfanew;
    if (memcmp(pe, "PE\0\0", 4) != 0) {
        free(buf); *err = "no PE signature"; return NULL;
    }
    nsec = rd16(pe + 6);
    opt_size = rd16(pe + 20);
    opt = pe + 24;
    magic = rd16(opt);
    dd_off = (magic == 0x20Bu) ? 112u : 96u;      /* PE32+ vs PE32 */
    exp_rva = rd32(opt + dd_off);
    sections = opt + opt_size;

    if (exp_rva == 0u) {
        /* A DLL with no export directory at all.  Reported, never silently
         * treated as "zero exports, nothing to compare". */
        free(buf);
        *err = "the DLL has NO export directory — the .def never reached the "
               "linker, and every export assertion here would be vacuous";
        return NULL;
    }

    /* RVA -> file offset, through the section table. */
    off = 0u;
    for (s = 0; s < nsec; ++s) {
        const unsigned char *sh = sections + (size_t)s * 40u;
        uint32_t va = rd32(sh + 12), vsz = rd32(sh + 8);
        uint32_t raw = rd32(sh + 20), rsz = rd32(sh + 16);
        uint32_t span = (vsz > rsz) ? vsz : rsz;
        if (exp_rva >= va && exp_rva < va + span) {
            off = raw + (exp_rva - va);
            break;
        }
    }
    if (off == 0u || (size_t)off + 0x28u > size) {
        free(buf); *err = "export directory outside the image"; return NULL;
    }
    dir = buf + off;
    count = rd32(dir + 0x18);                      /* NumberOfNames  */
    names_rva = rd32(dir + 0x20);                  /* AddressOfNames */

    names_off = 0u;
    for (s = 0; s < nsec; ++s) {
        const unsigned char *sh = sections + (size_t)s * 40u;
        uint32_t va = rd32(sh + 12), vsz = rd32(sh + 8);
        uint32_t raw = rd32(sh + 20), rsz = rd32(sh + 16);
        uint32_t span = (vsz > rsz) ? vsz : rsz;
        if (names_rva >= va && names_rva < va + span) {
            names_off = raw + (names_rva - va);
            break;
        }
    }
    if (names_off == 0u) {
        free(buf); *err = "export name table outside the image"; return NULL;
    }

    names = (char **)calloc(count ? count : 1u, sizeof(char *));
    for (i = 0; i < count; ++i) {
        uint32_t rva = rd32(buf + names_off + i * 4u);
        uint32_t noff = 0u;
        for (s = 0; s < nsec; ++s) {
            const unsigned char *sh = sections + (size_t)s * 40u;
            uint32_t va = rd32(sh + 12), vsz = rd32(sh + 8);
            uint32_t raw = rd32(sh + 20), rsz = rd32(sh + 16);
            uint32_t span = (vsz > rsz) ? vsz : rsz;
            if (rva >= va && rva < va + span) {
                noff = raw + (rva - va);
                break;
            }
        }
        names[i] = noff ? _strdup((const char *)(buf + noff)) : _strdup("?");
    }
    free(buf);
    *out_count = count;
    *err = NULL;
    return names;
}

#define JCE_HAVE_EXPORT_READER 1

#else   /* !_WIN32 */

/* ELF/Mach-O export enumeration is not implemented, and is IGNORED rather than
 * silently passed: an unimplemented reader that returned "no extra exports"
 * would be a green light nobody measured.  The GetProcAddress/dlsym half of
 * this file still runs everywhere. */
static char **read_exported_names(size_t *out_count, const char **err)
{
    *out_count = 0;
    *err = NULL;
    return NULL;
}

#endif

static void free_names(char **names, size_t count)
{
    size_t i;
    if (!names)
        return;
    for (i = 0; i < count; ++i)
        free(names[i]);
    free(names);
}

static bool in_manifest(const char *name)
{
    size_t i;
    for (i = 0; i < JCE_SCRIPT_API_EXPORT_COUNT; ++i)
        if (strcmp(kJceScriptApiExports[i], name) == 0)
            return true;
    return false;
}

/* ------------------------------------------------------------------ *
 *  A mock host that records what the forwarders did.
 * ------------------------------------------------------------------ */

typedef struct MockHost {
    int   set_position_calls;
    float last_xyz[3];
    int   last_button;
    int   button_calls;
    int   touch_count_value;
    int   json_free_calls;
    char *last_json;
} MockHost;

static MockHost g_mock;
static bool     g_poison_called;

static bool m_get_position(void *user, JceScriptEntity e, float out[3])
{
    (void)user;
    if (e != 42u)
        return false;
    out[0] = 1.0f; out[1] = 2.0f; out[2] = 3.0f;
    return true;
}

static void m_set_position(void *user, JceScriptEntity e,
                           float x, float y, float z)
{
    MockHost *m = (MockHost *)user;
    (void)e;
    ++m->set_position_calls;
    m->last_xyz[0] = x; m->last_xyz[1] = y; m->last_xyz[2] = z;
}

static void m_move_axis(void *user, float out_xz[2])
{
    (void)user;
    out_xz[0] = 0.25f;
    out_xz[1] = -0.5f;
}

static JceScriptEntity m_find_with_tag(void *user, const char *tag)
{
    (void)user;
    return (tag && strcmp(tag, "player") == 0) ? 7u : 0u;
}

static bool m_input_button(void *user, int button)
{
    MockHost *m = (MockHost *)user;
    ++m->button_calls;
    m->last_button = button;
    return true;
}

static int m_touch_count(void *user)
{
    return ((MockHost *)user)->touch_count_value;
}

static char *m_comp_get_json(void *user, JceScriptEntity e, const char *type)
{
    MockHost *m = (MockHost *)user;
    (void)e; (void)type;
    m->last_json = (char *)malloc(16);
    memcpy(m->last_json, "{\"a\":1}", 8);
    return m->last_json;
}

static void m_json_free(void *user, char *s)
{
    MockHost *m = (MockHost *)user;
    ++m->json_free_calls;
    free(s);
}

static const char *m_loc_translate(void *user, const char *key)
{
    (void)user; (void)key;
    return "translated";
}

static void m_poison(void)
{
    g_poison_called = true;
}

static void fill_host(JceScriptHost *h)
{
    memset(h, 0, sizeof *h);
    h->user = &g_mock;
    h->get_position = m_get_position;
    h->set_position = m_set_position;
    h->move_axis = m_move_axis;
    h->find_with_tag = m_find_with_tag;
    h->input_button = m_input_button;
    h->touch_count = m_touch_count;
    h->comp_get_json = m_comp_get_json;
    h->json_free = m_json_free;
    h->loc_translate = m_loc_translate;
}

static JceScriptApi *open_with(const JceScriptHost *h, size_t size)
{
    fn_open open_fn = (fn_open)must_sym("jce_script_api_open");
    TEST_ASSERT_NOT_NULL_MESSAGE(open_fn, "jce_script_api_open is not exported");
    return open_fn(h, size, JCE_SCRIPT_API_VERSION);
}

static void close_api(JceScriptApi *api)
{
    ((fn_close)must_sym("jce_script_api_close"))(api);
}

void setUp(void)
{
    memset(&g_mock, 0, sizeof g_mock);
    g_poison_called = false;
}

void tearDown(void) {}

/* ================================================================== *
 *  1. The export table IS the scripting surface.
 * ================================================================== */

static void test_the_dll_exports_every_manifest_entry(void)
{
    const char *err = NULL;
    size_t count = 0, i, j;
    char **names = read_exported_names(&count, &err);

#if !defined(JCE_HAVE_EXPORT_READER)
    (void)names; (void)i; (void)j;
    TEST_IGNORE_MESSAGE("export-table reader is implemented for PE only");
#else
    TEST_ASSERT_NULL_MESSAGE(err, err);
    TEST_ASSERT_NOT_NULL(names);
    for (i = 0; i < JCE_SCRIPT_API_EXPORT_COUNT; ++i) {
        bool found = false;
        for (j = 0; j < count && !found; ++j)
            found = strcmp(names[j], kJceScriptApiExports[i]) == 0;
        if (!found) {
            char msg[192];
            snprintf(msg, sizeof msg,
                     "the manifest entry '%s' is NOT exported by the DLL",
                     kJceScriptApiExports[i]);
            free_names(names, count);
            TEST_FAIL_MESSAGE(msg);
        }
    }
    free_names(names, count);
#endif
}

/* The half that carries the sandbox claim.  It forbids EVERY name that is not
 * a manifest entry: an engine symbol, a CRT symbol, a debug helper somebody
 * added a __declspec(dllexport) to, an asset reader.  If this is green, the
 * shared object's exports and the scripting surface are the same set. */
static void test_the_dll_exports_nothing_else(void)
{
    const char *err = NULL;
    size_t count = 0, j;
    char **names = read_exported_names(&count, &err);

#if !defined(JCE_HAVE_EXPORT_READER)
    (void)names; (void)j;
    TEST_IGNORE_MESSAGE("export-table reader is implemented for PE only");
#else
    TEST_ASSERT_NULL_MESSAGE(err, err);
    TEST_ASSERT_NOT_NULL(names);
    for (j = 0; j < count; ++j) {
        if (!in_manifest(names[j])) {
            char msg[224];
            snprintf(msg, sizeof msg,
                     "the DLL exports '%s', which is not on the scripting "
                     "surface — the export set must be exactly the manifest",
                     names[j]);
            free_names(names, count);
            TEST_FAIL_MESSAGE(msg);
        }
    }
    if (count != JCE_SCRIPT_API_EXPORT_COUNT) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "the DLL exports %u names; the manifest describes %u",
                 (unsigned)count, (unsigned)JCE_SCRIPT_API_EXPORT_COUNT);
        free_names(names, count);
        TEST_FAIL_MESSAGE(msg);
    }
    free_names(names, count);
#endif
}

/* The loader's own answer, independent of the file parser above. */
static void test_every_manifest_entry_resolves_by_name(void)
{
    size_t i;
    for (i = 0; i < JCE_SCRIPT_API_EXPORT_COUNT; ++i) {
        if (sym(kJceScriptApiExports[i]) == NULL) {
            char msg[160];
            snprintf(msg, sizeof msg, "%s does not resolve in the loaded DLL",
                     kJceScriptApiExports[i]);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

/* Named individually because these are the ones that would matter if the
 * boundary ever leaked: engine lifecycle, the engine ABI handshake, the
 * allocator, and the unvalidated asset primitive the manifest calls P0-2. */
static void test_no_engine_symbol_is_reachable_by_name(void)
{
    static const char *const forbidden[] = {
        "jce_script_create",
        "jce_script_create_sized",
        "jce_script_instantiate",
        "jce_script_call_update",
        "jce_api_version",
        "jce_log",
        "jce_alloc",
        "jce_script_api_asset_read_text",
        "jce_script_api_asset_read_json",
        "jce_script_api_read_file",
    };
    size_t i;
    for (i = 0; i < sizeof forbidden / sizeof forbidden[0]; ++i) {
        if (sym(forbidden[i]) != NULL) {
            char msg[176];
            snprintf(msg, sizeof msg,
                     "'%s' resolves in the script API DLL — the boundary leaks",
                     forbidden[i]);
            TEST_FAIL_MESSAGE(msg);
        }
    }
}

/* ================================================================== *
 *  2. Versioning.
 * ================================================================== */

static void test_the_version_entry_point_reports_the_manifest_version(void)
{
    fn_version v = (fn_version)must_sym("jce_script_api_version");
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(JCE_SCRIPT_API_VERSION, v(),
        "the library reports a different script_api_version than the header "
        "both were generated from");
}

static void test_open_refuses_a_binding_newer_than_the_library(void)
{
    JceScriptHost host;
    fn_open open_fn = (fn_open)must_sym("jce_script_api_open");
    JceScriptApi *api;
    fill_host(&host);
    api = open_fn(&host, sizeof host, JCE_SCRIPT_API_VERSION + 1u);
    TEST_ASSERT_NULL_MESSAGE(api,
        "a binding generated against a NEWER script_api_version was accepted; "
        "it would call entry points this library does not have");
}

static void test_open_accepts_a_binding_at_or_below_the_library_version(void)
{
    JceScriptHost host;
    fn_open open_fn = (fn_open)must_sym("jce_script_api_open");
    JceScriptApi *api;
    fill_host(&host);
    api = open_fn(&host, sizeof host, JCE_SCRIPT_API_VERSION);
    TEST_ASSERT_NOT_NULL_MESSAGE(api, "a matching binding was refused");
    close_api(api);
}

static void test_open_rejects_a_missing_host(void)
{
    fn_open open_fn = (fn_open)must_sym("jce_script_api_open");
    TEST_ASSERT_NULL(open_fn(NULL, sizeof(JceScriptHost),
                             JCE_SCRIPT_API_VERSION));
}

/* ================================================================== *
 *  3. The forwarders.
 * ================================================================== */

static void test_forwarders_reach_the_host(void)
{
    JceScriptHost host;
    JceScriptApi *api;
    float xyz[3] = { 0 };
    float axis[2] = { 0 };

    fill_host(&host);
    api = open_with(&host, sizeof host);
    TEST_ASSERT_NOT_NULL(api);

    TEST_ASSERT_TRUE(((fn_get_position)must_sym("jce_script_api_get_position"))(
        api, 42u, xyz));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, xyz[0]);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, xyz[2]);

    ((fn_set_position)must_sym("jce_script_api_set_position"))(api, 1u,
                                                          4.0f, 5.0f, 6.0f);
    TEST_ASSERT_EQUAL_INT(1, g_mock.set_position_calls);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, g_mock.last_xyz[1]);

    ((fn_move_axis)must_sym("jce_script_api_move_axis"))(api, axis);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, axis[0]);

    TEST_ASSERT_EQUAL_UINT64(7u,
        ((fn_find_with_tag)must_sym("jce_script_api_find_with_tag"))(api, "player"));

    close_api(api);
}

/* jump_pressed / sprint / attack_pressed are three entries over ONE host
 * member, separated only by the button the binding supplies.  If the ABI
 * passed the button through instead, the three would be the same function and
 * a caller could ask for a button the scripting surface cannot name. */
static void test_bound_arguments_are_supplied_by_the_library(void)
{
    JceScriptHost host;
    JceScriptApi *api;
    fill_host(&host);
    api = open_with(&host, sizeof host);

    TEST_ASSERT_TRUE(((fn_button)must_sym("jce_script_api_jump_pressed"))(api));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_mock.last_button,
        "jump_pressed must call input_button(0)");
    TEST_ASSERT_TRUE(((fn_button)must_sym("jce_script_api_sprint"))(api));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_mock.last_button,
        "sprint must call input_button(1)");
    TEST_ASSERT_TRUE(((fn_button)must_sym("jce_script_api_attack_pressed"))(api));
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_mock.last_button,
        "attack_pressed must call input_button(2)");

    close_api(api);
}

static void test_absent_members_are_absent_and_never_a_crash(void)
{
    JceScriptHost host;
    JceScriptApi *api;
    float xyz[3] = { 9.0f, 9.0f, 9.0f };
    float axis[2] = { 9.0f, 9.0f };
    char buf[8];

    memset(&host, 0, sizeof host);          /* every member NULL */
    api = open_with(&host, sizeof host);
    TEST_ASSERT_NOT_NULL(api);

    TEST_ASSERT_FALSE(((fn_get_position)must_sym("jce_script_api_get_position"))(
        api, 42u, xyz));
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, xyz[0],
        "an absent fallible_out must zero its out parameter, not leave the "
        "caller's buffer untouched");

    ((fn_move_axis)must_sym("jce_script_api_move_axis"))(api, axis);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, axis[1]);

    TEST_ASSERT_EQUAL_UINT64(0u,
        ((fn_find_with_tag)must_sym("jce_script_api_find_with_tag"))(api, "player"));

    TEST_ASSERT_EQUAL_INT_MESSAGE(-1,
        ((fn_comp_get)must_sym("jce_script_api_comp_get"))(api, 1u, "Water",
                                                      buf, (int)sizeof buf),
        "an absent owned_string_release entry must report -1");

    close_api(api);
}

/* Two entries whose absent value is NOT the zero of their type.  The manifest
 * says so, and a C ABI that returned 0 / NULL there would be a different
 * surface from the Lua one. */
static void test_absent_values_that_are_not_zero(void)
{
    JceScriptHost host;
    JceScriptApi *api;
    const char *tr;

    memset(&host, 0, sizeof host);
    api = open_with(&host, sizeof host);

    tr = ((fn_tr)must_sym("jce_script_api_tr"))(api, "ui.play");
    TEST_ASSERT_NOT_NULL_MESSAGE(tr,
        "tr must degrade to KEY PASSTHROUGH when the host has no translator");
    TEST_ASSERT_EQUAL_STRING("ui.play", tr);

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-1.0f,
        ((fn_music_transition)must_sym("jce_script_api_music_request_transition"))(
            api, 2),
        "music_request_transition reports a NEGATIVE playhead on a miss; 0 "
        "would read as a valid time");

    close_api(api);
}

static void test_a_negative_touch_count_is_clamped(void)
{
    JceScriptHost host;
    JceScriptApi *api;
    fill_host(&host);
    g_mock.touch_count_value = -3;
    api = open_with(&host, sizeof host);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0,
        ((fn_touch_count)must_sym("jce_script_api_get_touch_count"))(api),
        "a host returning a negative count must be clamped to 0");
    close_api(api);
}

static void test_owned_strings_are_copied_and_released(void)
{
    JceScriptHost host;
    JceScriptApi *api;
    char buf[32];
    int n;

    fill_host(&host);
    api = open_with(&host, sizeof host);
    n = ((fn_comp_get)must_sym("jce_script_api_comp_get"))(api, 3u, "Water",
                                                      buf, (int)sizeof buf);
    TEST_ASSERT_EQUAL_INT(7, n);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", buf);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_mock.json_free_calls,
        "the host's heap string must be released before returning — no "
        "ownership may cross this ABI");
    close_api(api);
}

/* ================================================================== *
 *  4. A short host is never read past its end.
 *
 *  The same hazard test_jce_script_host_abi.c exists for, on this side of the
 *  boundary: JceScriptHost is allocated by the CALLER and grows over releases,
 *  so a copy at the library's own sizeof reads whatever followed a shorter
 *  caller's object and files it under the newest member — a FUNCTION POINTER
 *  the next call jumps through.
 *
 *  The poison is a pointer to a real function rather than 0xFF, so the failure
 *  is a NAMED assertion instead of a segfault — and, more importantly, so that
 *  a zero-filled tail cannot make "absent" true for the wrong reason.
 * ================================================================== */

static void test_a_short_host_is_never_read_past_its_end(void)
{
    JceScriptHost full;
    JceScriptApi *api;
    unsigned char *shorter;
    void (*poison)(void) = m_poison;
    size_t trunc = offsetof(JceScriptHost, JCE_SCRIPT_API_TAIL_MEMBER);
    size_t tail_slots = 8u, i;
    float out[3] = { 0.0f, 0.0f, 0.0f };

    /* The tail entry MOVES whenever JceScriptHost is appended to, which the
     * ABI rules explicitly permit -- so this assert is not a constant anybody
     * chose, it is a tripwire that says which call below to re-type.  Last
     * moved by the raycast_filtered / raycast_all append (2026-09-22): giving
     * the script raycast the layer mask and multi-hit the overlap queries
     * beside it already had.  Before that it was curve_eval, before that
     * get_param_text, and before that vcam_activate.
     *
     * RE-TYPING THE CALL IS NOT OPTIONAL WORK.  Leaving the old call in place
     * would keep this test GREEN and stop it testing anything: the previous
     * tail member is no longer truncated away, so it would answer normally
     * and the poison would sit past a boundary nothing reaches. */
    TEST_ASSERT_EQUAL_STRING_MESSAGE("jce_script_api_raycast_all",
        JCE_SCRIPT_API_TAIL_ENTRY,
        "the manifest's tail entry moved; this test still calls "
        "raycast_all, so update the call below to the new entry's "
        "prototype");
    TEST_ASSERT_TRUE_MESSAGE(sizeof(JceScriptHost) > trunc,
        "the truncation point withholds nothing — this test has gone vacuous");

    fill_host(&full);
    shorter = (unsigned char *)calloc(1u, trunc + tail_slots * sizeof poison);
    TEST_ASSERT_NOT_NULL(shorter);
    memcpy(shorter, &full, trunc);
    for (i = 0; i < tail_slots; ++i)
        memcpy(shorter + trunc + i * sizeof poison, &poison, sizeof poison);

    api = open_with((const JceScriptHost *)shorter, trunc);
    TEST_ASSERT_NOT_NULL(api);

    (void)out;
    /* The short host stops BEFORE this member, so the forwarder must take its
     * absent branch.  THREE things are asserted: false back, the out slot left
     * in a DEFINED state, and the poison never entered.
     *
     * DEFINED, not preserved -- and the distinction is worth the sentence,
     * because the two contracts on either side of this boundary differ on
     * purpose.  The HOST callback leaves *out_value untouched on false, so a
     * caller's default survives; the generated forwarder MEMSETS every out
     * slot to zero on its absent branch, uniformly, for every fallible_out in
     * the manifest.  Across a C ABI that is the better rule: leaving a
     * foreign runtime's buffer unwritten is worse than writing a defined
     * value, and the zero is unobservable through all seven bindings, which
     * discard it whenever the call returns false.
     *
     * The sentinel starts at a value neither side can produce, so "the
     * forwarder wrote zero" and "the forwarder wrote nothing" are still two
     * different readings here. */
    /* The tail is now an entity_table, so the shape of the answer changes
     * with it: a COUNT rather than a bool, and an entity array rather than a
     * double.  The sentinel moves into that array for the same reason it
     * existed before -- "the forwarder wrote zero" and "the forwarder wrote
     * nothing" must stay two different readings. */
    const float t_origin[3] = { 0.0f, 0.0f, 0.0f };
    const float t_dir[3]    = { 0.0f, 0.0f, 1.0f };
    JceScriptEntity tail_hits[4];
    for (i = 0; i < 4u; ++i) tail_hits[i] = (JceScriptEntity)0xABCDEF01u;

    TEST_ASSERT_EQUAL_INT_MESSAGE(0,
        ((fn_raycast_all)must_sym(JCE_SCRIPT_API_TAIL_ENTRY))(
            api, t_origin, t_dir, 100.0f, 0u, false, tail_hits, 4),
        "a member the caller's host does not carry answered with something");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(0u, (uint64_t)tail_hits[0],
        "the absent branch left the caller's out slot unwritten; every other "
        "forwarder zeroes its out slots, and a C ABI that hands back an "
        "untouched buffer has handed back whatever was there");
    TEST_ASSERT_FALSE_MESSAGE(g_poison_called,
        "the library read past the caller's shorter host: the tail entry "
        "called what followed the caller's object");

    /* And the members the short host DOES carry still work. */
    TEST_ASSERT_EQUAL_UINT64(7u,
        ((fn_find_with_tag)must_sym("jce_script_api_find_with_tag"))(api, "player"));

    close_api(api);
    free(shorter);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_dll_exports_every_manifest_entry);
    RUN_TEST(test_the_dll_exports_nothing_else);
    RUN_TEST(test_every_manifest_entry_resolves_by_name);
    RUN_TEST(test_no_engine_symbol_is_reachable_by_name);
    RUN_TEST(test_the_version_entry_point_reports_the_manifest_version);
    RUN_TEST(test_open_refuses_a_binding_newer_than_the_library);
    RUN_TEST(test_open_accepts_a_binding_at_or_below_the_library_version);
    RUN_TEST(test_open_rejects_a_missing_host);
    RUN_TEST(test_forwarders_reach_the_host);
    RUN_TEST(test_bound_arguments_are_supplied_by_the_library);
    RUN_TEST(test_absent_members_are_absent_and_never_a_crash);
    RUN_TEST(test_absent_values_that_are_not_zero);
    RUN_TEST(test_a_negative_touch_count_is_clamped);
    RUN_TEST(test_owned_strings_are_copied_and_released);
    RUN_TEST(test_a_short_host_is_never_read_past_its_end);
    return UNITY_END();
}
