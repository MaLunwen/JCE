/* test_jce_script_vm_cpp_modules.cpp — the module ABI, and the two things a
 * lifecycle differential structurally cannot reach.
 *
 *   1. THE CLAMP, in the direction a module allocates and the engine reads.
 *      The tables here are HAND-WRITTEN rather than produced by
 *      JCE_CPP_SCRIPT_CLASS, and that is the point: a test that used the
 *      macro could only ever exercise tables the macro can produce, and a
 *      table truncated at an old header's last member is not among them.
 *
 *   2. SYMBOL LIFETIME ACROSS A SHARED OBJECT.  The engine holds function
 *      pointers into an image it does not own.  A statically linked module
 *      cannot fail the way a plugin can — its code stays mapped whatever the
 *      registry does, so a missing refcount would be INVISIBLE.  These cases
 *      load a real .dll/.so, dispatch into it, and prove the unload refusal
 *      is a refusal rather than a log line in front of an unmap.
 *
 * No doctest main here; it lives in test_jce_script_vm_cpp_lifecycle.cpp.
 */
#include "doctest.h"

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>
#include <jce/script_vm/jce_script_vm_cpp.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

/* Defined in the lifecycle TU.  Registration is once per PROCESS. */
bool jce_cpp_test_ensure_registered();

namespace {

struct Trace {
    std::vector<std::string> events;
    std::vector<std::string> logs;
};

Trace *tr_of(void *user) { return static_cast<Trace *>(user); }

}  // namespace

extern "C" {

static void mod_log(void *user, const char *msg)
{
    tr_of(user)->logs.push_back(msg != nullptr ? msg : "(null)");
}

static void mod_ui_set_text(void *user, JceScriptEntity e, const char *txt)
{
    char head[64];
    std::snprintf(head, sizeof head, "ui_set_text|%llu|",
                  static_cast<unsigned long long>(e));
    tr_of(user)->events.push_back(std::string(head) +
                                  (txt != nullptr ? txt : "(null)"));
}

/* The slot the SHORT class table withholds.  It RECORDS rather than crashing:
 * a truncation whose only symptom is a segfault proves the process died, and
 * "a crash reports less than a red test" is the failure this campaign
 * measured four times. */
static bool g_raw_anim_called;
static int  g_raw_live;

static void *raw_create(const JceCppScriptContext *ctx)
{
    (void)ctx;
    ++g_raw_live;
    return std::malloc(4);
}
static void raw_destroy(void *self)
{
    --g_raw_live;
    std::free(self);
}
static JceCppStatus raw_on_start(void *self)
{
    (void)self;
    return NULL;
}
static JceCppStatus raw_on_anim(void *self, uint32_t id, const char *name,
                                float f0, float f1, int i0)
{
    (void)self;
    (void)id;
    (void)name;
    (void)f0;
    (void)f1;
    (void)i0;
    g_raw_anim_called = true;
    return NULL;
}

}  // extern "C"

namespace {

const JceScriptEntity kOwner = 42u;

JceScriptHost make_host(Trace *tr)
{
    JceScriptHost h;
    std::memset(&h, 0, sizeof h);
    h.user        = tr;
    h.log         = mod_log;
    h.ui_set_text = mod_ui_set_text;
    return h;
}

JceCppScriptClass raw_class(const char *name, std::size_t struct_size)
{
    JceCppScriptClass c;
    std::memset(&c, 0, sizeof c);
    c.struct_size   = struct_size;
    c.name          = name;
    c.create        = raw_create;
    c.destroy       = raw_destroy;
    c.on_start      = raw_on_start;
    c.on_anim_event = raw_on_anim;
    return c;
}

JceCppModuleDesc raw_module(const char *name,
                            const JceCppScriptClass *const *classes,
                            std::size_t                     count)
{
    JceCppModuleDesc d;
    std::memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.abi_version = JCE_CPP_MODULE_ABI_VERSION;
    d.module_name = name;
    d.classes     = classes;
    d.class_count = count;
    return d;
}

}  // namespace

/* ================================================================== *
 *  THE CLAMP
 * ================================================================== */

TEST_CASE("a short class table is not read past its end")
{
    REQUIRE(jce_cpp_test_ensure_registered());

    /* The two tables are IDENTICAL BYTES; only struct_size differs.  So the
     * only thing the comparison below can be measuring is the clamp. */
    static JceCppScriptClass full =
        raw_class("RawFull", sizeof(JceCppScriptClass));
    static JceCppScriptClass shortc =
        raw_class("RawShort", offsetof(JceCppScriptClass, on_anim_event));
    static const JceCppScriptClass *const full_arr[]  = {&full};
    static const JceCppScriptClass *const short_arr[] = {&shortc};

    JceCppModuleDesc dfull  = raw_module("raw_full", full_arr, 1);
    JceCppModuleDesc dshort = raw_module("raw_short", short_arr, 1);

    JceCppModule *mf = jce_script_vm_cpp_add_module(&dfull);
    JceCppModule *ms = jce_script_vm_cpp_add_module(&dshort);
    REQUIRE(mf != nullptr);
    REQUIRE(ms != nullptr);

    Trace         tr;
    JceScriptHost h = make_host(&tr);
    JceScript    *s = jce_script_vm_create("cpp", &h, sizeof h);
    REQUIRE(s != nullptr);

    /* VACUITY FIRST: with the full struct_size the slot IS reached, so "not
     * reached" below is evidence about the clamp and not about the slot being
     * unreachable for some unrelated reason. */
    g_raw_anim_called   = false;
    JceScriptInstance a = jce_script_instantiate(s, "RawFull", kOwner);
    REQUIRE(a != 0);
    jce_script_call_anim_event(s, a, 1u, "x", 0.f, 0.f, 0);
    CHECK_MESSAGE(g_raw_anim_called,
                  "the FULL table's on_anim_event was not called, so the "
                  "truncated case below withholds nothing and proves nothing");

    g_raw_anim_called   = false;
    JceScriptInstance b = jce_script_instantiate(s, "RawShort", kOwner);
    REQUIRE(b != 0);
    jce_script_call_anim_event(s, b, 1u, "x", 0.f, 0.f, 0);
    CHECK_MESSAGE(!g_raw_anim_called,
                  "the VM called a class slot the module's own struct_size "
                  "said was not there — a function pointer read past the end "
                  "of the module's table, and then CALLED");

    /* And the clamp cut in the right place: the slot BEFORE the truncation
     * still dispatches, so the class was narrowed rather than rejected. */
    jce_script_call_start(s, b);

    jce_script_release(s, a);
    jce_script_release(s, b);
    CHECK_MESSAGE(g_raw_live == 0, "a released instance was not destroyed");
    jce_script_destroy(s);
    CHECK(jce_script_vm_cpp_unload(mf));
    CHECK(jce_script_vm_cpp_unload(ms));
}

TEST_CASE("a class missing create or destroy is refused, and refusal leaves "
          "nothing behind")
{
    REQUIRE(jce_cpp_test_ensure_registered());
    static JceCppScriptClass bad =
        raw_class("RawBad", sizeof(JceCppScriptClass));
    bad.destroy                                 = nullptr;
    static const JceCppScriptClass *const arr[] = {&bad};
    JceCppModuleDesc d = raw_module("raw_bad", arr, 1);

    CHECK_MESSAGE(jce_script_vm_cpp_add_module(&d) == nullptr,
                  "a class with no destroy leaks every instance it makes");
    CHECK_MESSAGE(!jce_script_vm_cpp_has_class("RawBad"),
                  "a REFUSED module left its class in the registry — and a "
                  "refused module is exactly the one whose code may be about "
                  "to be unmapped");
}

TEST_CASE("a module with an unknown ABI version is refused")
{
    REQUIRE(jce_cpp_test_ensure_registered());
    static JceCppScriptClass c =
        raw_class("RawAbi", sizeof(JceCppScriptClass));
    static const JceCppScriptClass *const arr[] = {&c};
    JceCppModuleDesc d  = raw_module("raw_abi", arr, 1);
    d.abi_version      += 7u;

    CHECK(jce_script_vm_cpp_add_module(&d) == nullptr);
    CHECK(!jce_script_vm_cpp_has_class("RawAbi"));
}

TEST_CASE("a duplicate class name is refused: instantiate resolves by name")
{
    REQUIRE(jce_cpp_test_ensure_registered());
    static JceCppScriptClass c1 = raw_class("RawDup", sizeof(JceCppScriptClass));
    static JceCppScriptClass c2 = raw_class("RawDup", sizeof(JceCppScriptClass));
    static const JceCppScriptClass *const a1[] = {&c1};
    static const JceCppScriptClass *const a2[] = {&c2};
    JceCppModuleDesc d1 = raw_module("raw_dup_a", a1, 1);
    JceCppModuleDesc d2 = raw_module("raw_dup_b", a2, 1);

    JceCppModule *m1 = jce_script_vm_cpp_add_module(&d1);
    REQUIRE(m1 != nullptr);
    CHECK_MESSAGE(jce_script_vm_cpp_add_module(&d2) == nullptr,
                  "two modules published the same class name; which class "
                  "instantiate() returns would then depend on load order");
    CHECK(jce_script_vm_cpp_unload(m1));
    /* And the name is free again once the first module is gone. */
    JceCppModule *m2 = jce_script_vm_cpp_add_module(&d2);
    CHECK(m2 != nullptr);
    if (m2) CHECK(jce_script_vm_cpp_unload(m2));
}

/* ================================================================== *
 *  SYMBOL LIFETIME ACROSS A SHARED OBJECT
 * ================================================================== */

TEST_CASE("a bare module name is refused; the path must be absolute")
{
    REQUIRE(jce_cpp_test_ensure_registered());
    CHECK_MESSAGE(jce_script_vm_cpp_load_library("jce_cpp_testplugin.dll") ==
                      nullptr,
                  "jce_library_open() attaches to an already-resident module "
                  "of the same name rather than loading a second copy "
                  "(jce_library.h), so a bare name can bind to a different "
                  "file than the one on disk — and every pointer in it is one "
                  "the engine is about to call");
    CHECK(jce_script_vm_cpp_load_library("") == nullptr);
    CHECK(jce_script_vm_cpp_load_library(nullptr) == nullptr);
    CHECK_MESSAGE(!jce_script_vm_cpp_has_class("PluginProbe"),
                  "a refused load must leave nothing registered");
}

TEST_CASE("unloading a module with a live instance is refused, and the "
          "instance still dispatches")
{
    REQUIRE(jce_cpp_test_ensure_registered());

    JceCppModule *m = jce_script_vm_cpp_load_library(JCE_CPP_TESTPLUGIN_PATH);
    REQUIRE_MESSAGE(m != nullptr, "could not load the test plugin from "
                                      << std::string(JCE_CPP_TESTPLUGIN_PATH));
    CHECK(std::string(jce_script_vm_cpp_module_name(m)) ==
          "jce_cpp_testplugin");
    REQUIRE(jce_script_vm_cpp_has_class("PluginProbe"));

    Trace         tr;
    JceScriptHost h = make_host(&tr);
    JceScript    *s = jce_script_vm_create("cpp", &h, sizeof h);
    REQUIRE(s != nullptr);
    JceScriptInstance i = jce_script_instantiate(s, "PluginProbe", kOwner);
    REQUIRE(i != 0);
    CHECK(jce_script_vm_cpp_live_instances(m) == 1);

    jce_script_call_update(s, i, 0.25f);
    REQUIRE_MESSAGE(tr.events.size() == 1u,
                    "the plugin's on_update never reached the host, so the "
                    "'still dispatches' claim below has nothing to be about");

    CHECK_MESSAGE(!jce_script_vm_cpp_unload(m),
                  "unload SUCCEEDED with a live instance: the next dispatch "
                  "calls through an unmapped page, inside the engine's own "
                  "forwarder, with no script in the backtrace");
    CHECK_MESSAGE(jce_script_vm_cpp_live_instances(m) == 1,
                  "a refused unload must change nothing");

    /* THE PROOF THAT THE REFUSAL WAS A REFUSAL: the image is still mapped and
     * its code still runs.  An unload that logged and unmapped anyway would
     * take the process here. */
    jce_script_call_update(s, i, 0.25f);
    CHECK_MESSAGE(tr.events.size() == 2u,
                  "the instance stopped dispatching after a REFUSED unload");

    jce_script_release(s, i);
    CHECK(jce_script_vm_cpp_live_instances(m) == 0);
    CHECK_MESSAGE(jce_script_vm_cpp_unload(m),
                  "unload must succeed once the last instance is released");
    CHECK_MESSAGE(!jce_script_vm_cpp_has_class("PluginProbe"),
                  "an unloaded module's classes must stop resolving, or "
                  "instantiate() hands out pointers into a closed library");
    CHECK(jce_script_instantiate(s, "PluginProbe", kOwner) == 0);

    jce_script_destroy(s);
}

/* ── Reload picks up the NEW image ──────────────────────────────────────── */

TEST_CASE("unload + load_library of the same path runs the REBUILT code")
{
    /* THE CLAIM THIS PROVES is the one the editor's "Reload Native Script
     * Modules" command makes: after a project rebuilds its .jcec, unloading
     * and loading the same path runs what is on disk NOW.
     *
     * It needs proving because the plausible failure is silent. jce_library.h
     * :36-39 records that opening a library on Windows "attaches to an
     * already-resident module of the same name rather than loading a second
     * copy" -- so a reload that re-attached would succeed, log success, and go
     * on running the previous build. Nothing about that looks wrong; it looks
     * like the edit did not take.
     *
     * So: copy v1 to a scratch path, load it, observe what it does, unload,
     * copy v2 OVER THE SAME PATH, load again, and observe that what it does
     * changed. Same module name, same class name, same path -- the only
     * variable is the bytes in the file. */
    REQUIRE(jce_cpp_test_ensure_registered());

    namespace fs = std::filesystem;
    const fs::path src_v1 = JCE_CPP_TESTPLUGIN_PATH;
    const fs::path src_v2 = JCE_CPP_TESTPLUGIN_V2_PATH;
    REQUIRE_MESSAGE(fs::exists(src_v1), "v1 plugin missing: " << src_v1.string());
    REQUIRE_MESSAGE(fs::exists(src_v2), "v2 plugin missing: " << src_v2.string());

    /* Beside the binaries, so the path is absolute (load_library refuses a
     * relative one) and on the same volume as the source. */
    const fs::path live =
        src_v1.parent_path() / ("jce_cpp_reload_probe" + src_v1.extension().string());
    std::error_code ec;
    fs::remove(live, ec);
    fs::copy_file(src_v1, live, fs::copy_options::overwrite_existing, ec);
    REQUIRE_MESSAGE(!ec, "could not stage v1 at " << live.string() << ": "
                                                  << ec.message());

    std::string first, second;

    {
        JceCppModule *m = jce_script_vm_cpp_load_library(live.string().c_str());
        REQUIRE_MESSAGE(m != nullptr, "v1 did not load from " << live.string());
        Trace         tr;
        JceScriptHost h = make_host(&tr);
        JceScript    *s2 = jce_script_vm_create("cpp", &h, sizeof h);
        REQUIRE(s2 != nullptr);
        JceScriptInstance i = jce_script_instantiate(s2, "PluginProbe", kOwner);
        REQUIRE(i != 0);
        jce_script_call_update(s2, i, 0.25f);
        REQUIRE_MESSAGE(tr.events.size() == 1u,
                        "v1's on_update never reached the host, so there is "
                        "nothing for the comparison below to be about");
        first = tr.events[0];

        jce_script_release(s2, i);
        jce_script_destroy(s2);
        REQUIRE(jce_script_vm_cpp_live_instances(m) == 0);
        REQUIRE_MESSAGE(jce_script_vm_cpp_unload(m),
                        "unload refused with no live instances -- the reload "
                        "workflow has no safe point if this can happen");
    }

    /* The rebuild. Retried briefly: on Windows a just-unloaded image can stay
     * briefly unwritable, and a flaky copy here would look like a reload
     * defect. */
    ec.clear();
    for (int attempt = 0; attempt < 50; ++attempt) {
        fs::copy_file(src_v2, live, fs::copy_options::overwrite_existing, ec);
        if (!ec) break;
    }
    REQUIRE_MESSAGE(!ec, "could not overwrite " << live.string()
                                                << " with v2: " << ec.message()
                                                << " -- the image is probably "
                                                   "still mapped, which is "
                                                   "itself the defect");

    {
        JceCppModule *m = jce_script_vm_cpp_load_library(live.string().c_str());
        REQUIRE_MESSAGE(m != nullptr, "v2 did not load from " << live.string());
        Trace         tr;
        JceScriptHost h = make_host(&tr);
        JceScript    *s2 = jce_script_vm_create("cpp", &h, sizeof h);
        REQUIRE(s2 != nullptr);
        JceScriptInstance i = jce_script_instantiate(s2, "PluginProbe", kOwner);
        REQUIRE(i != 0);
        jce_script_call_update(s2, i, 0.25f);
        REQUIRE(tr.events.size() == 1u);
        second = tr.events[0];

        jce_script_release(s2, i);
        jce_script_destroy(s2);
        CHECK(jce_script_vm_cpp_unload(m));
    }

    fs::remove(live, ec);

    /* THE ASSERTION. Equal strings mean the second load re-attached to the
     * image already mapped and the rebuild never took effect -- which is
     * exactly what a reload command must not be allowed to claim it did. */
    CHECK_MESSAGE(first != second,
                  "the same path produced the same behaviour after being "
                  "overwritten with a different build: the reload re-attached "
                  "to the resident image instead of reading the file. first="
                      << first << " second=" << second);
    CHECK_MESSAGE(second.find("REBUILT") != std::string::npos,
                  "the second load did not run v2's code: " << second);
    CHECK_MESSAGE(first.find("REBUILT") == std::string::npos,
                  "v1 already says REBUILT, so the assertion above cannot "
                  "distinguish the two builds: " << first);
}

TEST_CASE("destroying a VM with live instances releases the module refcount")
{
    REQUIRE(jce_cpp_test_ensure_registered());

    JceCppModule *m = jce_script_vm_cpp_load_library(JCE_CPP_TESTPLUGIN_PATH);
    REQUIRE(m != nullptr);

    Trace         tr;
    JceScriptHost h = make_host(&tr);
    JceScript    *s = jce_script_vm_create("cpp", &h, sizeof h);
    REQUIRE(s != nullptr);
    REQUIRE(jce_script_instantiate(s, "PluginProbe", kOwner) != 0);
    REQUIRE(jce_script_instantiate(s, "PluginProbe", kOwner + 1u) != 0);
    CHECK(jce_script_vm_cpp_live_instances(m) == 2);

    /* jce_script_destroy does NOT run on_destroy — script_lua_destroy closes
     * the lua_State without calling it — but it MUST free the C++ objects and
     * give the refcount back, or a module becomes permanently un-unloadable
     * because a VM simply went away. */
    jce_script_destroy(s);
    CHECK_MESSAGE(jce_script_vm_cpp_live_instances(m) == 0,
                  "destroying the VM left the module's refcount raised; the "
                  "module can now never be unloaded");
    CHECK(jce_script_vm_cpp_unload(m));
}

TEST_CASE("a module's own name survives the image that spelled it")
{
    REQUIRE(jce_cpp_test_ensure_registered());
    /* The registry COPIES every name because the original is a string literal
     * in the module's image, and a successful unload closes that image.
     * Reading the name back afterwards is the check. */
    JceCppModule *m = jce_script_vm_cpp_load_library(JCE_CPP_TESTPLUGIN_PATH);
    REQUIRE(m != nullptr);
    const std::string before = jce_script_vm_cpp_module_name(m);
    REQUIRE(before == "jce_cpp_testplugin");
    CHECK(jce_script_vm_cpp_unload(m));
    CHECK_MESSAGE(std::string(jce_script_vm_cpp_module_name(m)) == before,
                  "the module name did not survive its own unload, so it was "
                  "borrowed from the image that was just closed");
}

/* ================================================================== *
 *  REGISTRATION
 * ================================================================== */

TEST_CASE("registering twice is not an error and does not duplicate")
{
    REQUIRE(jce_cpp_test_ensure_registered());
    const int before = jce_script_vm_count();
    CHECK(jce_script_vm_cpp_register());
    CHECK(jce_script_vm_cpp_register());
    CHECK_MESSAGE(jce_script_vm_count() == before,
                  "a second cpp registration added a second registry entry; "
                  "live handles hold a pointer into that registry");
    CHECK(jce_script_vm_find("cpp") != nullptr);
}

TEST_CASE("a second table under the name 'cpp' is refused by the core")
{
    REQUIRE(jce_cpp_test_ensure_registered());
    /* This is the structural argument for putting the plugin boundary BELOW
     * the VM rather than AT it: at most one table may own a language name, so
     * a design in which each plugin filled its own JceScriptVM would permit
     * exactly one plugin per process. */
    JceScriptVM impostor = *jce_script_vm_cpp();
    CHECK_MESSAGE(!jce_script_vm_register(&impostor),
                  "the core accepted a SECOND 'cpp' table — if it did, a "
                  "plugin-supplies-the-VM design would have been viable and "
                  "this backend's central decision is wrong");
}

TEST_CASE("a cpp handle reports its language and passed the header check")
{
    REQUIRE(jce_cpp_test_ensure_registered());
    Trace         tr;
    JceScriptHost h = make_host(&tr);
    JceScript    *s = jce_script_vm_create("cpp", &h, sizeof h);
    REQUIRE_MESSAGE(s != nullptr,
                    "NULL means jce_script_vm_create REFUSED the handle: "
                    "JceScriptVMHeader is not first, or hdr.vm does not point "
                    "at the table it dispatched through");
    CHECK(std::string(jce_script_vm_language_of(s)) == "cpp");
    jce_script_destroy(s);
}

TEST_CASE("registering cpp does not move the default language away from lua")
{
    REQUIRE(jce_cpp_test_ensure_registered());
    Trace         tr;
    JceScriptHost h = make_host(&tr);
    JceScript    *s = jce_script_create_sized(&h, sizeof h);
    REQUIRE(s != nullptr);
    CHECK_MESSAGE(std::string(jce_script_vm_language_of(s)) == "lua",
                  "jce_script_create stopped making a Lua VM — every game in "
                  "the tree would silently change interpreter");
    jce_script_destroy(s);
}

/* ================================================================== *
 *  THE SEVEN HAND-WRITTEN ENTRIES ARE NOT ON THE MODULE SURFACE
 * ================================================================== */

namespace {

/* Comments are stripped FIRST, on purpose: both headers discuss the seven by
 * name and at length, and a grep that fired on prose would be a gate nobody
 * could keep green — which is a gate that gets deleted. */
std::string strip_comments(const std::string &src)
{
    std::string out;
    out.reserve(src.size());
    std::size_t i = 0;
    while (i < src.size()) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/'))
                ++i;
            i = (i + 2 <= src.size()) ? i + 2 : src.size();
            out.push_back(' ');
        } else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
            out.push_back('\n');
        } else {
            out.push_back(src[i++]);
        }
    }
    return out;
}

std::string read_all(const std::string &path)
{
    std::ifstream      f(path.c_str(), std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

TEST_CASE("the module-facing surface names none of the hand-written entries")
{
    static const char *const kForbidden[] = {
        "asset_read_text", "asset_read_json", "play_sound",
        "start_coroutine", "wait_seconds",    "stop_coroutine",
        /* The member a convenience would actually have to reach in order to
         * bypass asset_read_*'s path validation, 1 MiB cap and depth-capped
         * JSON walk.  This is the P0-2 escape, by name. */
        "read_file",
    };
    static const char *const kHeaders[] = {
        JCE_CPP_VM_HEADER_DIR "/jce_script_vm_cpp.h",
        JCE_CPP_VM_HEADER_DIR "/jce_script_cpp.hpp",
    };

    for (const char *hdr : kHeaders) {
        const std::string raw = read_all(hdr);
        REQUIRE_MESSAGE(!raw.empty(),
                        "could not read " << std::string(hdr)
                                          << " — a path typo would make this "
                                             "gate pass vacuously");
        const std::string code = strip_comments(raw);
        REQUIRE_MESSAGE(code.find("JceCppScriptClass") != std::string::npos,
                        "stripping comments from "
                            << std::string(hdr)
                            << " left no code behind; the stripper ate the "
                               "file and the search below means nothing");
        for (const char *bad : kForbidden) {
            INFO("header: " << std::string(hdr) << "   entry: " << std::string(bad));
            CHECK_MESSAGE(code.find(bad) == std::string::npos,
                          "the module-facing surface declares '"
                              << std::string(bad)
                              << "'. Those entries carry sandbox policy that "
                                 "lives in static functions inside "
                                 "jce_script.c; reaching around them is the "
                                 "escape the manifest calls P0-2.");
        }
    }
}

/* ================================================================== *
 *  THE PATH FORM
 *
 *  A Script component stores a STRING.  For every other backend that
 *  string is a file the VM opens; for this one it NAMES a class.  Two
 *  mechanisms bridge the gap and each is pinned below:
 *
 *    the CLAIM   jce_script_vm_cpp_register() claims ".jcecpp", so
 *                jce_script_vm_language_for_path() routes such a path
 *                here at all.  Without it the runtime refuses the
 *                entity before any VM is asked, which is what a
 *                four-language scene actually measured.
 *    the RESOLVE cpp_instantiate() tries three candidates in a fixed
 *                order, whole-string first.
 *
 *  THE ORDER IS THE COMPATIBILITY GUARANTEE and it is the case worth
 *  the most here: a project that named its class after the entire
 *  stored path — the only thing that worked before these forms
 *  existed — must keep resolving to ITS class and not to a basename.
 * ================================================================== */

TEST_CASE("registering the cpp backend claims .jcecpp, and a scriptPath with "
          "that extension routes to this VM")
{
    REQUIRE(jce_cpp_test_ensure_registered());

    /* The claim is what the RUNTIME asks before it picks a VM.  A registered
     * language nothing routes to is reachable only through the whole-process
     * JCE_SCRIPT_LANGUAGE override, which a scene with a second language
     * cannot use — so "registered" alone is not evidence of anything. */
    const char *lang = jce_script_vm_language_for_path("scripts/Foo.jcecpp");
    REQUIRE_MESSAGE(lang != nullptr,
                    "no language claims '.jcecpp' — a Script component naming "
                    "a C++ class resolves to nothing and is refused before "
                    "any VM is asked");
    CHECK(std::strcmp(lang, JCE_SCRIPT_VM_CPP_LANGUAGE) == 0);

    /* Case folds, like every other claim. */
    CHECK(jce_script_vm_language_for_path("Foo.JCECPP") != nullptr);

    /* AND THE SPELLINGS THAT MUST NOT BE CLAIMED.  ".cpp" would classify
     * every translation unit in the project as an attachable script and would
     * send the project's C++ source into the shipped game. */
    CHECK_MESSAGE(jce_script_vm_language_for_path("src/turret.cpp") == nullptr,
                  "the cpp backend claimed '.cpp' — every translation unit in "
                  "the project is now an attachable script");
    CHECK(jce_script_vm_language_for_path("src/turret.h") == nullptr);
    CHECK(jce_script_vm_language_for_path("src/turret.hpp") == nullptr);
    CHECK(jce_script_vm_language_for_path("src/turret.cc") == nullptr);

    /* A second call must not double-claim (the core refuses a duplicate
     * extension and would take the whole register() down with it). */
    CHECK_MESSAGE(jce_script_vm_cpp_register(),
                  "a second jce_script_vm_cpp_register() failed — the claim "
                  "half is not idempotent, so a process linking two modules "
                  "has to remember which of them called first");
}

TEST_CASE("a class named after the whole stored path still wins over its own "
          "basename")
{
    REQUIRE(jce_cpp_test_ensure_registered());

    /* THE REGRESSION THESE CASES EXIST TO PREVENT.  Before the basename and
     * extension-stripping candidates existed, the ONLY thing that worked was
     * naming the class after the entire scriptPath — an out-of-tree project
     * shipped exactly that and is not in this repository, so nothing else
     * would catch it changing meaning.  Both classes are registered at once,
     * so this is a genuine race between candidate 1 and candidate 3 rather
     * than a lookup with one possible answer. */
    static JceCppScriptClass whole =
        raw_class("PathWhole.jcecpp", sizeof(JceCppScriptClass));
    static JceCppScriptClass stem =
        raw_class("PathWhole", sizeof(JceCppScriptClass));
    static const JceCppScriptClass *const whole_arr[] = {&whole};
    static const JceCppScriptClass *const stem_arr[]  = {&stem};

    JceCppModuleDesc dw = raw_module("path_whole", whole_arr, 1);
    JceCppModuleDesc ds = raw_module("path_stem", stem_arr, 1);
    JceCppModule    *mw = jce_script_vm_cpp_add_module(&dw);
    JceCppModule    *ms = jce_script_vm_cpp_add_module(&ds);
    REQUIRE(mw != nullptr);
    REQUIRE(ms != nullptr);

    Trace         tr;
    JceScriptHost h = make_host(&tr);
    JceScript    *s = jce_script_vm_create("cpp", &h, sizeof h);
    REQUIRE(s != nullptr);

    const int before_whole = jce_script_vm_cpp_live_instances(mw);
    const int before_stem  = jce_script_vm_cpp_live_instances(ms);

    JceScriptInstance in = jce_script_instantiate(s, "PathWhole.jcecpp", kOwner);
    REQUIRE(in != 0);

    /* WHICH class ran is read from the owning module's refcount, not from a
     * name the test itself supplied — the registry is the only witness that
     * cannot agree with a wrong answer. */
    CHECK_MESSAGE(jce_script_vm_cpp_live_instances(mw) == before_whole + 1,
                  "the whole-path class did NOT win: an existing module that "
                  "names its class after the entire scriptPath just changed "
                  "meaning, and no gate in this repository would say so");
    CHECK(jce_script_vm_cpp_live_instances(ms) == before_stem);

    jce_script_release(s, in);
    jce_script_destroy(s);
    CHECK(jce_script_vm_cpp_unload(mw));
    CHECK(jce_script_vm_cpp_unload(ms));
}

TEST_CASE("instantiate resolves a scriptPath to a class: directory, then "
          "extension")
{
    REQUIRE(jce_cpp_test_ensure_registered());

    static JceCppScriptClass c =
        raw_class("PathPlain", sizeof(JceCppScriptClass));
    static const JceCppScriptClass *const arr[] = {&c};
    JceCppModuleDesc d = raw_module("path_plain", arr, 1);
    JceCppModule    *m = jce_script_vm_cpp_add_module(&d);
    REQUIRE(m != nullptr);

    Trace         tr;
    JceScriptHost h = make_host(&tr);
    JceScript    *s = jce_script_vm_create("cpp", &h, sizeof h);
    REQUIRE(s != nullptr);

    /* Every spelling a scene could plausibly store for ONE class published
     * under the name C++ actually spells it. */
    static const char *const kAccepted[] = {
        "PathPlain",                       /* candidate 1: bare class name  */
        "PathPlain.jcecpp",                /* candidate 3: extension only   */
        "scripts/PathPlain.jcecpp",        /* candidates 2 then 3           */
        "scripts\\PathPlain.jcecpp",       /* Windows separator             */
        "a/b/c/PathPlain",                 /* candidate 2 alone             */
        "PathPlain.escpp",                 /* ANY extension, not just ours  */
    };
    for (const char *p : kAccepted) {
        INFO("scriptPath: " << std::string(p));
        JceScriptInstance in = jce_script_instantiate(s, p, kOwner);
        CHECK_MESSAGE(in != 0, "this scriptPath did not resolve to its class");
        if (in) jce_script_release(s, in);
    }

    /* The last entry above is deliberate: NO extension is validated, because
     * validating one would make resolution depend on whether the claim call
     * had already run, so the same path would resolve differently depending
     * on startup order.  These pin that stripping is still not a free-for-all
     * — a typo stays a refusal. */
    static const char *const kRefused[] = {
        "PathPlai",                        /* a real typo stays a refusal   */
        "PathPlain.jcecpp/",               /* trailing separator, no name   */
        "Other/PathOther.jcecpp",          /* neither form names our class  */
        "",
    };
    for (const char *p : kRefused) {
        INFO("scriptPath: " << std::string(p));
        CHECK(jce_script_instantiate(s, p, kOwner) == 0);
    }

    jce_script_destroy(s);
    CHECK(jce_script_vm_cpp_unload(m));
}

TEST_CASE("has_class answers for every form instantiate accepts")
{
    REQUIRE(jce_cpp_test_ensure_registered());

    static JceCppScriptClass c =
        raw_class("PathValidate", sizeof(JceCppScriptClass));
    static const JceCppScriptClass *const arr[] = {&c};
    JceCppModuleDesc d = raw_module("path_validate", arr, 1);
    JceCppModule    *m = jce_script_vm_cpp_add_module(&d);
    REQUIRE(m != nullptr);

    /* The header promises this function "resolves the way instantiate() does"
     * so a host can validate an authored scriptPath at LOAD time rather than
     * discovering a 0 at spawn time.  A narrower lookup here would refuse
     * paths instantiate would have run — a validator that is wrong in the
     * direction that costs the most. */
    CHECK(jce_script_vm_cpp_has_class("PathValidate"));
    CHECK(jce_script_vm_cpp_has_class("PathValidate.jcecpp"));
    CHECK(jce_script_vm_cpp_has_class("scripts/PathValidate.jcecpp"));
    CHECK(jce_script_vm_cpp_has_class("scripts\\PathValidate"));
    CHECK_FALSE(jce_script_vm_cpp_has_class("PathValidat"));
    CHECK_FALSE(jce_script_vm_cpp_has_class("scripts/Other.jcecpp"));
    CHECK_FALSE(jce_script_vm_cpp_has_class(nullptr));
    CHECK_FALSE(jce_script_vm_cpp_has_class(""));

    /* And it tracks the registry rather than a snapshot: after the unload the
     * same names must stop answering, or a host would validate against
     * classes that no longer exist. */
    CHECK(jce_script_vm_cpp_unload(m));
    CHECK_FALSE(jce_script_vm_cpp_has_class("PathValidate"));
    CHECK_FALSE(jce_script_vm_cpp_has_class("scripts/PathValidate.jcecpp"));
}
