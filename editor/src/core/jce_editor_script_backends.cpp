/*
 * jce_editor_script_backends.cpp — the editor's own script-VM registration.
 *
 * ── THE DEFECT THIS CLOSES ───────────────────────────────────────────────
 *
 * The engine registers exactly one language for itself: the built-in Lua VM,
 * lazily (jce_script_vm.h, JCE_SCRIPT_VM_DEFAULT_LANGUAGE).  Every other
 * backend registers itself from a call the APPLICATION makes, and until this
 * file existed the editor made none — so an entity carrying `turret.py` was
 * REFUSED in editor Play, by name, in the log, while the same scene in the
 * shipped game ran it fine.
 *
 * MEASURED, on the real Elemental Serenity scene with four scripted entities
 * (2026-08-16, jce_editor.exe with no backend registration):
 *
 *     script 'scripts/es_fireflies.py': no script VM claims its extension
 *     script 'scripts/EsCampfire.java': no script VM claims its extension
 *       registered languages: lua
 *
 * One of four languages ran.  The diagnostic was excellent and the outcome
 * was still that a multi-language project cannot be AUTHORED: Play is where a
 * designer sees whether a script works, and three quarters of the scripts
 * were dead there and alive in the build.  That asymmetry — editor weaker
 * than the shipped runtime — is the exact shape of failure the packaging
 * rules in this repo exist to prevent, running in the opposite direction.
 *
 * ── C AND C++ ARE TWO LANGUAGES OVER ONE REGISTRY ────────────────────────
 *
 * Both are registered here, by two separate calls, because they are two
 * separate registered languages with two claimed extensions (".jcec",
 * ".jcecpp").  Registering one would leave the other resolving to no language
 * in Play while the shipped game ran it.
 *
 * They share the module registry and therefore the whole of
 * jce_editor_script_modules_reload() below: a compiled class has no language
 * at run time, so a project's "script_modules" entry is loaded once and its
 * classes answer to a scriptPath of either extension.  Which extension a
 * project uses states which language its AUTHOR wrote, which is what the
 * Script inspector and every refusal message need it for.
 *
 * ── THE C++ BACKEND IS HERE TOO, AND THIS SECTION USED TO SAY IT COULD NOT
 *    BE ──────────────────────────────────────────────────────────────────
 *
 * What it said was: a cpp "script" is a CLASS resolved through a module
 * registry, the module is compiled into the GAME's executable, the editor is
 * a different process and does not link the game — so registering the cpp VM
 * here would add a language with no classes in it.  Every clause of that is
 * still true and the conclusion was still wrong, because it answered a
 * question about where the CLASSES come from with a decision about where the
 * VM lives.  They are separable:
 *
 *   THE VM belongs in this process unconditionally.  It needs no toolchain
 *   and no runtime, and since it claims ".jcecpp" its absence is not neutral:
 *   without it, a Script component naming a C++ class resolves to no language
 *   in the editor and the Script inspector cannot even tell a designer which
 *   VM would run it.
 *
 *   THE CLASSES arrive at run time, from a shared object the PROJECT builds
 *   and names in its own jce_project.json ("script_modules"), loaded through
 *   jce_script_vm_cpp_load_library().  That cannot happen at init — there is
 *   no project yet — so it is a second entry point,
 *   jce_editor_script_modules_reload(), called from the project-open seam.
 *
 * The old text also warned that registering an empty VM would replace a clear
 * "no VM claims this extension" with a confusing "no cpp script class named
 * X".  That warning was right about the message and wrong about which message
 * is confusing NOW: with a module declared and loaded the second message is
 * the accurate one, and when no module is loaded at all the backend says so
 * in those words rather than blaming the spelling.
 *
 * ── THE FOUR WAYS A PROJECT MODULE FAILS TO LOAD ─────────────────────────
 *
 * A module is a DLL/.so this process dlopens, so "it did not load" covers
 * four states with four different fixes, and jce_library_open() reports the
 * same NULL for the first two.  They are separated here, before the load is
 * even attempted a second time, because a designer who reads "unknown
 * language" for a module built into the wrong configuration will go and edit
 * the scene:
 *
 *   ABSENT        the declared path holds no file.  The project has not built
 *                 its script module yet, or the manifest names the wrong
 *                 output directory.
 *   UNMAPPABLE    the file is there and the OS refused it: wrong architecture,
 *                 a dependent DLL it cannot find (jce_script_api is staged
 *                 beside jce_editor.exe, and the loader resolves a module's
 *                 dependencies from the EXECUTABLE's directory, not the
 *                 module's), or a build configuration whose C runtime this
 *                 process cannot load.
 *   NOT A MODULE  it maps but exports no jce_cpp_script_module — a plain
 *                 shared library, or one built without JCE_CPP_MODULE_END
 *                 / JCE_C_MODULE_END.  One symbol, both languages.
 *   REFUSED       it loaded and the registry rejected it: a different module
 *                 ABI (the module is stale — rebuild it), a module name
 *                 already registered, or a full registry.  That branch's own
 *                 reason is logged by the backend on the line above, and it
 *                 is the line to read: only the FIRST of those three is
 *                 fixed by rebuilding, so this branch names all three rather
 *                 than prescribing the common one.  Measured: two entries
 *                 pointing at copies of one module under different filenames
 *                 print "native script module 'X' refused: already registered" — for
 *                 which "rebuild it" is the wrong instruction.
 *
 * WHAT IS *NOT* DETECTED, stated so nobody reads more into the load line than
 * is there: nothing here can tell that a module which loads cleanly was built
 * from older sources.  The two staleness cases that ARE caught are a module
 * ABI bump (REFUSED, above) and a class added since the module was built,
 * which surfaces at instantiate as "N native script module(s) are loaded
 * and none publishes any of those names ... or rebuild the module".  The
 * load line prints the module file's timestamp so the third case is at
 * least visible.
 *
 * ── WHERE THE RUNTIME PATHS COME FROM ────────────────────────────────────
 *
 * Neither backend can discover its own runtime: CPython needs the
 * `jce_script` package on sys.path, and the JVM needs an absolute jvm library,
 * a class path, and the absolute path of the JNI shim.  Two sources, in
 * priority order, and the order is the point:
 *
 *   1. THE ENVIRONMENT, so a SHIPPED editor bundle — which has no repository
 *      to point at — can be aimed at its own copies without a rebuild.
 *   2. The configure-time paths of the tree this editor was built from,
 *      which is what makes a developer's editor work with no setup at all.
 *
 * A path that does not exist is reported and skipped rather than passed on:
 * "sys.path refused" three layers down is a worse message than this one.
 *
 * NOTHING HERE FAILS THE EDITOR.  Every refusal is logged with its reason and
 * the editor keeps running on Lua, which is precisely the state it was in
 * before this file existed.
 */

#include "jce_editor_script_backends.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_path.h>
#include <jce/middleware/script/jce_script_vm.h>

/* Generated into this target's binary dir by jce_script_enable(): the
 * registration shim the editor and every shipped game share. */
#include <jce_script_register_linked.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(JCE_SCRIPT_LINKED_CPP)
#  include <jce/application/jce_project.h>
#  include <jce/os/platform/jce_library.h>
#  include <jce/script_vm/jce_script_vm_cpp.h>
#endif
#if defined(JCE_SCRIPT_LINKED_C)
#  include <jce/script_vm/jce_script_vm_c.h>
#endif
#if defined(JCE_SCRIPT_LINKED_CSHARP)
#  include <jce/application/jce_project.h>
#  include <jce/script_vm/jce_script_vm_csharp.h>
#endif
#if defined(JCE_EDITOR_HAVE_SCRIPT_PYTHON)
#  include "jce_script_vm_python.h"
#endif
#if defined(JCE_EDITOR_HAVE_SCRIPT_JAVA)
#  include <jce/script_vm/jce_script_vm_java.h>
/* Generated by editor/CMakeLists.txt: the absolute jvm library derived from
 * the JDK the backend's jni.h came from, the class path holding
 * com.jce.script.*, and the absolute path of the JNI shim. */
#  include "jce_editor_java_paths.gen.h"
#endif

/* THE GATE BELOW NAMES A MACRO THAT IS ACTUALLY DEFINED.
 *
 * It used to read JCE_EDITOR_HAVE_SCRIPT_CPP, and nothing in the tree ever
 * defined that -- it appeared five times in this file and nowhere else, all
 * of them `#if defined(...)`.  So jce_editor_script_modules_reload() below
 * compiled to its "deliberately silent" no-op in EVERY build, and a project's
 * "script_modules" were never loaded by the editor at all.
 *
 * What made it survive is that the half above it works: the VMs are
 * registered through the shared shim, so the editor reports c and cpp as
 * available languages and the extensions resolve.  The failure is one layer
 * down and says so plainly -- "NO NATIVE SCRIPT MODULE IS LOADED IN THIS
 * PROCESS" -- which reads like a project that forgot to build its module
 * rather than an editor that cannot load one.  Measured on a seven-language
 * project: editor Play ran three languages, the shipped exe ran seven, from
 * the same scene file.
 *
 * jce_script_enable() puts JCE_SCRIPT_LINKED_<LANG>=1 on every target it
 * touches, including this one, so that is the condition that means what the
 * old name was trying to mean. */
#define LOG_TAG "editor.script"

namespace {

#if defined(JCE_EDITOR_HAVE_SCRIPT_PYTHON) || defined(JCE_EDITOR_HAVE_SCRIPT_JAVA)

/* env first, then the build-time default.  Returns an empty string when both
 * are absent, which every caller treats as "cannot configure this backend". */
std::string path_from(const char *env_name, const char *built_in)
{
    const char *v = std::getenv(env_name);
    if (v && v[0]) return std::string(v);
    return built_in ? std::string(built_in) : std::string();
}

void set_env(const char *key, const char *value)
{
#if JCE_PLATFORM_WINDOWS
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

/* jce_script_api is the shared library BOTH the Python ctypes binding and the
 * Java JNI shim call.  A dev editor sits beside its own copy; a bundle should
 * too.  Returns "" when there is none next to the executable, in which case
 * the bindings fall back to their own search and say so if they fail. */
std::string script_api_beside_exe(const char *libname)
{
    char base[1024] = {0};
    if (!jce_fs_host_get_base_path(base, (uint32_t)sizeof base) || !base[0])
        return std::string();
    std::string p = std::string(base) + libname;
    return jce_fs_host_exists_file(p.c_str()) ? p : std::string();
}

#endif  /* any backend */

#if defined(JCE_SCRIPT_LINKED_CPP)

/* One project script module this process currently holds mapped.
 *
 * THE PATH IS STORED ALONGSIDE THE HANDLE and that is not bookkeeping for its
 * own sake: reopening the SAME project must not unload and reload a module
 * whose classes may already be instantiated, and the registry refuses a second
 * module of the same name — so "is this one already loaded?" has to be
 * answerable, and the handle alone cannot answer it. */
struct LoadedModule {
    std::string   path;      /* the absolute path it was loaded from */
    JceCppModule *handle;
};

std::vector<LoadedModule> g_cpp_modules;

/* The declared list, resolved to absolute paths against `root`.
 *
 * JCE_SCRIPT_CPP_MODULES REPLACES the manifest rather than adding to it, in
 * the same shape and for the same reason as JCE_SCRIPT_JAVA_JVM above: the
 * developer aiming an editor at a different build wants THAT build, and a
 * list that quietly also loaded the manifest's would give them two modules
 * with the same module_name, the second refused. */
std::vector<std::string> declared_modules(const char *root)
{
    std::vector<std::string> out;

    auto push = [&out, root](const std::string &entry) {
        if (entry.empty()) return;
        char abs[1024];
        if (jce_path_is_absolute(entry.c_str())) {
            std::snprintf(abs, sizeof abs, "%s", entry.c_str());
        } else if (!root || !root[0]) {
            /* A relative entry with no project root cannot be resolved, and
             * jce_script_vm_cpp_load_library() REFUSES a relative path (a bare
             * name binds to whatever module of that name is already resident).
             * Saying so here is better than letting it read as "cannot open". */
            LOG_WARN(LOG_TAG,
                     "script module '%s' is a relative path and there is no "
                     "open project to resolve it against — skipped.",
                     entry.c_str());
            return;
        } else if (!jce_path_join(abs, sizeof abs, root, entry.c_str())) {
            LOG_WARN(LOG_TAG, "script module path too long: %s/%s",
                     root, entry.c_str());
            return;
        }
        jce_path_canonicalise_inplace(abs);
        out.push_back(std::string(abs));
    };

    const char *env = std::getenv("JCE_SCRIPT_CPP_MODULES");
    if (env && env[0]) {
#if JCE_PLATFORM_WINDOWS
        const char sep = ';';   /* ';' — a Windows path contains ':'         */
#else
        const char sep = ':';
#endif
        std::string cur;
        for (const char *p = env;; ++p) {
            if (*p == sep || *p == '\0') {
                push(cur);
                cur.clear();
                if (*p == '\0') break;
            } else {
                cur.push_back(*p);
            }
        }
        LOG_INFO(LOG_TAG,
                 "JCE_SCRIPT_CPP_MODULES is set: it REPLACES the project's "
                 "\"script_modules\" for this run (%d entry/entries).",
                 (int)out.size());
        return out;
    }

    if (!root || !root[0]) return out;
    const JceProject *proj = jce_project_load(root);
    if (!proj) return out;          /* no manifest is not an error */
    for (int i = 0; i < proj->script_modules_count; ++i)
        if (proj->script_modules[i]) push(std::string(proj->script_modules[i]));
    jce_project_free(const_cast<JceProject *>(proj));
    return out;
}

/* WHY EVERY MESSAGE IN THIS FILE SAYS "native script module" AND NOT "C++".
 *
 * Five of them said C++ until a project loaded a C module and read
 * "C++ script module loaded: 'elemental_serenity_c'".  A compiled class has
 * no language at run time — one registry, one loader, one entry symbol — so
 * the loader genuinely CANNOT know, and a message that cannot know must not
 * guess: this is the same defect 028cc503 fixed for the runtime's refusal and
 * 1a7323cc for the cpp backend's diagnostics.  The failure branch below was
 * already right ("native script module ABSENT"); the success branch was not,
 * which is the worse way round — a working C author was told their module was
 * something else, every time it worked.
 *
 * The one place the language IS knowable is the scriptPath, so the summary
 * names BOTH extensions rather than the language of the module.
 */

/* Say WHICH of the four states a failed load is in.  See the banner.
 *
 * EACH STATE IS TWO LOG CALLS AND THE SPLIT IS LOAD-BEARING, not styling.
 * jce_log_ring.h's queued record is `char message[512]`, and jce_log_write
 * vsnprintf()s into it — so a message longer than 511 chars is TRUNCATED
 * SILENTLY, tail first, and the tail of every message here is the part that
 * says what to do about it.  Measured before this split, with the path 126
 * chars: UNMAPPABLE emitted 610 chars and arrived cut at "...whose C runtime
 * is not installed. R", losing the whole remedy sentence — the one state the
 * designer cannot guess their way out of.  ABSENT emitted 505 and survived by
 * six characters, which is not a margin, it is luck with that path.
 *
 * So the PATH goes on the first line alone and the EXPLANATION on the second
 * with no format arguments at all, which makes every explanation a fixed
 * length no path can push over the cap.  Keep it that way: folding one of
 * these pairs back into a single call reintroduces a defect whose only
 * symptom is a sentence that stops mid-word.
 *
 * Nothing enforces this — there is no test that formats these strings and
 * measures them, and adding a gate for one function's log lines is not worth
 * what it would cost.  The guard is this comment plus the arithmetic above. */
void explain_load_failure(const char *abs)
{
    if (!jce_fs_host_exists_file(abs)) {
        LOG_WARN(LOG_TAG, "native script module ABSENT: nothing at '%s'", abs);
        LOG_WARN(LOG_TAG, "%s",
                 "  The project declares it in jce_project.json "
                 "(\"script_modules\"); build that target, or point the entry "
                 "at the output directory this build actually writes. A "
                 "relative entry is resolved against the project root. Until "
                 "then C and C++ scripts in this project are refused in Play — this "
                 "is NOT 'unknown language', and editing the scene will not "
                 "help.");
        return;
    }
    if (!jce_library_exists(abs)) {
        LOG_WARN(LOG_TAG,
                 "native script module UNMAPPABLE: '%s' exists and the OS "
                 "refused to load it", abs);
        LOG_WARN(LOG_TAG, "%s",
                 "  The usual three: it is built for a different ARCHITECTURE "
                 "than this editor; it needs a dependent library the loader "
                 "cannot find (dependencies resolve from THIS executable's "
                 "directory, not the module's — jce_script_api is staged "
                 "there); or it is built for a build CONFIGURATION whose C "
                 "runtime is not installed. Rebuild the module for the "
                 "configuration this editor is, or set "
                 "JCE_SCRIPT_CPP_MODULES to one that is.");
        return;
    }
    if (!jce_library_has_symbol(abs, JCE_CPP_MODULE_ENTRY_SYMBOL)) {
        LOG_WARN(LOG_TAG,
                 "'%s' loads but is NOT A JCE SCRIPT MODULE: it exports no %s",
                 abs, JCE_CPP_MODULE_ENTRY_SYMBOL);
        LOG_WARN(LOG_TAG, "%s",
                 "  A module must link jce_script_cpp_module (C++) or "
                 "jce_script_c_module (C) and emit the entry with "
                 "JCE_CPP_MODULE_END / JCE_C_MODULE_END(name, accessor); "
                 "a module "
                 "compiled only INTO the game executable has no entry symbol "
                 "and cannot be loaded by anything else.");
        return;
    }
    LOG_WARN(LOG_TAG,
             "native script module '%s' loaded and was REFUSED by the "
             "registry",
             abs);
    LOG_WARN(LOG_TAG, "%s",
             "  The reason is logged immediately above this line and is the "
             "one to read: the registry refuses a stale module ABI, a module "
             "name already registered, and a full registry, and only the "
             "first is fixed by rebuilding.");
}

#endif  /* JCE_SCRIPT_LINKED_CPP */

}  // namespace

void jce_editor_register_script_backends(void)
{
    /* ── ONE MECHANISM, SHARED WITH EVERY SHIPPED GAME ──────────────
     *
     * This was four hand-written blocks — one per language — doing the same
     * four steps the generated shim does, and they sat on the editor-versus-
     * shipped parity boundary: a language added to scripting/ worked in a
     * shipped executable and was silently refused in Play, visible only by
     * pressing Play.  Two implementations of one contract is how that stays
     * possible; there is now one, generated by jce_script_enable() from the
     * backend rosters, and the editor and a game run the SAME CODE.
     *
     * WHAT THIS FILE USED TO DO BETTER IS NOT GONE, IT MOVED.  The env
     * override (JCE_SCRIPT_PYTHON_PACKAGE_DIR, JAVA_HOME, …) and the
     * existence check before registering — with wording that says the backend
     * IS linked and the path is wrong, rather than "this language does not
     * work" — are in the shim now, so every consumer gets them.
     *
     * NULL: the editor publishes no native module at INIT.  A cpp/.jcec class
     * lives in a module the PROJECT builds, loaded later from that project's
     * jce_project.json "script_modules" — see jce_editor_script_modules_reload
     * below.  Nothing about the project is known here and nothing pretends to
     * be. */
    (void)jce_script_register_linked_languages(nullptr);

    /* Always say what Play can actually run, whether or not anything above
     * was compiled in.  A designer whose .py does nothing needs this line
     * before they need anything else, and "lua" alone is a complete answer
     * to the question they are about to ask. */
    {
        std::string langs;
        const int n = jce_script_vm_count();
        for (int i = 0; i < n; ++i) {
            const char *name = jce_script_vm_language_at(i);
            if (!name) continue;
            if (!langs.empty()) langs += ", ";
            langs += name;
        }
        LOG_INFO(LOG_TAG, "editor Play can run: %s", langs.c_str());
    }
}

bool jce_editor_script_modules_reload_native(const char *project_root)
{
#if !defined(JCE_SCRIPT_LINKED_CPP)
    (void)project_root;
    LOG_WARN(LOG_TAG,
             "native script module reload: this editor was built without the "
             "cpp script VM, so there is no module to reload.");
    return false;
#else
    if (!project_root || !project_root[0]) {
        LOG_WARN(LOG_TAG, "native script module reload: no project is open.");
        return false;
    }

    /* THE SAFE POINT, ASKED RATHER THAN ASSUMED.  See the header for why
     * "Play is stopped" is the answer and why this still counts. */
    int live = 0;
    const char *live_in = nullptr;
    for (const LoadedModule &m : g_cpp_modules) {
        const int n = jce_script_vm_cpp_live_instances(m.handle);
        if (n > 0) {
            live += n;
            if (!live_in) live_in = jce_script_vm_cpp_module_name(m.handle);
        }
    }
    if (live > 0) {
        LOG_WARN(LOG_TAG,
                 "native script module reload REFUSED: %d live script "
                 "instance(s) (first in module '%s'). Unloading now would "
                 "unmap code those instances dispatch into. Stop Play and "
                 "try again -- nothing was changed.",
                 live, live_in ? live_in : "?");
        return false;
    }

    /* Unload EVERYTHING before loading, because the load pass below skips a
     * path it already holds -- which is exactly the short-circuit that makes
     * the ordinary reload a no-op on the same project. */
    const int had = (int)g_cpp_modules.size();
    for (const LoadedModule &m : g_cpp_modules) {
        if (jce_script_vm_cpp_unload(m.handle)) continue;
        /* Cannot happen with live == 0, and handled anyway: a partially
         * unloaded list would leave the editor holding handles to closed
         * libraries.  Report and keep what is left rather than guess. */
        LOG_ERROR(LOG_TAG,
                  "native script module reload: '%s' refused to unload even "
                  "with no live instances. The module set is now partial; "
                  "close and reopen the project.",
                  m.path.c_str());
        g_cpp_modules.clear();
        jce_editor_script_modules_reload(project_root);
        return false;
    }
    g_cpp_modules.clear();

    /* REUSE, not a second loader.  With the list empty this takes the ordinary
     * load path, including the failure explanations and the build-timestamp
     * line that is the only cheap evidence a module which loaded cleanly is
     * nonetheless yesterday's build. */
    jce_editor_script_modules_reload(project_root);

    LOG_INFO(LOG_TAG,
             "native script modules reloaded from disk (%d were loaded, %d "
             "now).", had, (int)g_cpp_modules.size());
    return true;
#endif
}

#if defined(JCE_SCRIPT_LINKED_CSHARP)
/* A log sink for the throwaway VM below.
 *
 * A zeroed JceScriptHost has none, and the managed side reports a failed load
 * THROUGH it -- so with {0} the load returns false and says nothing anywhere.
 * An instrument that cannot report is indistinguishable from one that found
 * nothing. */
static void ed_cs_log(void *user, const char *msg)
{
    (void)user;
    if (msg && msg[0]) LOG_INFO(LOG_TAG, "csharp: %s", msg);
}

/* Load the project's managed assemblies so a .cs script resolves in Play.
 *
 * WHY A VM IS OPENED AND IMMEDIATELY CLOSED.  The assembly list on the
 * managed side is PROCESS-WIDE, not per-VM -- it has to be, because the
 * runtime creates its own Vm when Play starts, after any load a tool did.
 * So loading through a temporary VM here is enough, and keeping one alive
 * would only add a second handle nothing dispatches into.
 *
 * WHY THIS IS SEPARATE FROM THE NATIVE MODULE LOADER.  Neither loader can
 * tell the two kinds of .dll apart by looking: a native module is dlopened
 * and publishes classes through an entry symbol, a managed assembly is
 * handed to the .NET host.  Feeding one list to both would make every
 * project with C# scripts report a native-module failure and every project
 * with a native module report a managed one. */
static void ed_load_project_assemblies(const char *project_root)
{
    if (!project_root || !project_root[0]) return;
    const JceProject *proj = jce_project_load(project_root);
    if (!proj) return;                    /* no manifest is not an error */
    if (proj->script_assemblies_count <= 0) {
        jce_project_free(const_cast<JceProject *>(proj));
        return;                           /* no C# scripts is the common case */
    }

    const JceScriptVM *vm = jce_script_vm_find("csharp");
    if (!vm || !vm->create_sized) {
        LOG_WARN(LOG_TAG,
                 "this project declares %d managed assembly/assemblies and "
                 "this editor has no csharp backend linked, so every .cs "
                 "script in it will be refused by name in Play.",
                 proj->script_assemblies_count);
        jce_project_free(const_cast<JceProject *>(proj));
        return;
    }

    JceScriptHost host;
    std::memset(&host, 0, sizeof host);
    host.log = ed_cs_log;
    JceScript *sc = vm->create_sized(&host, sizeof host);
    if (!sc) {
        LOG_WARN(LOG_TAG, "could not open a csharp VM to load this "
                          "project's assemblies");
        jce_project_free(const_cast<JceProject *>(proj));
        return;
    }

    for (int i = 0; i < proj->script_assemblies_count; ++i) {
        const char *rel = proj->script_assemblies[i];
        if (!rel || !rel[0]) continue;
        std::string abs = rel;
        const bool rooted = (rel[0] == '/' || rel[0] == '\\' ||
                             (rel[0] && rel[1] == ':'));
        if (!rooted) abs = std::string(project_root) + "/" + rel;
        if (!jce_fs_host_exists_file(abs.c_str())) {
            LOG_WARN(LOG_TAG,
                     "managed assembly declared but not built: %s "
                     "(\"script_assemblies\") -- build it, or correct the "
                     "path; every .cs script will be refused until then.",
                     abs.c_str());
            continue;
        }
        if (jce_script_vm_csharp_load_assembly(sc, abs.c_str()))
            LOG_INFO(LOG_TAG, "managed script assembly loaded: %s",
                     abs.c_str());
        else
            LOG_WARN(LOG_TAG, "managed script assembly REFUSED: %s",
                     abs.c_str());
    }
    if (vm->destroy) vm->destroy(sc);
    jce_project_free(const_cast<JceProject *>(proj));
}
#endif  /* JCE_SCRIPT_LINKED_CSHARP */

void jce_editor_script_modules_reload(const char *project_root)
{
#if defined(JCE_SCRIPT_LINKED_CSHARP)
    /* Managed first and unconditionally: it does not share the native
     * loader's gate, so a build with C# but no C++ still gets its scripts. */
    ed_load_project_assemblies(project_root);
#endif
#if !defined(JCE_SCRIPT_LINKED_CPP)
    (void)project_root;
    /* Deliberately silent.  With no cpp VM in this build there is nothing a
     * project could declare that this editor could load, and a line on every
     * project open would be noise about a feature that is switched off. */
#else
    /* A CLOSED PROJECT LOADS NOTHING, env override or not.  The override
     * aims the editor at a different build OF THE OPEN PROJECT; honouring it
     * with no project open would map a module the editor has no scene for and
     * then hold its name against the next project that wants it. */
    std::vector<std::string> want;
    if (project_root && project_root[0])
        want = declared_modules(project_root);

    /* Unload what the new project does not ask for.  Keeping a module that IS
     * asked for is not an optimisation: reloading it would unmap code that
     * live instances dispatch into, which the backend refuses anyway — this
     * just means reopening the same project is a no-op instead of a refusal. */
    {
        std::vector<LoadedModule> kept;
        for (const LoadedModule &m : g_cpp_modules) {
            bool still_wanted = false;
            for (const std::string &w : want)
                if (w == m.path) { still_wanted = true; break; }
            if (still_wanted) { kept.push_back(m); continue; }
            if (jce_script_vm_cpp_unload(m.handle)) {
                LOG_INFO(LOG_TAG, "native script module unloaded: %s",
                         m.path.c_str());
                continue;
            }
            kept.push_back(m);
            LOG_WARN(LOG_TAG,
                     "native script module '%s' could not be unloaded (reason "
                     "above; almost always live script instances). It stays "
                     "loaded and keeps its module name reserved.",
                     m.path.c_str());
        }
        g_cpp_modules.swap(kept);
    }

    int loaded = 0;
    for (const std::string &abs : want) {
        bool already = false;
        for (const LoadedModule &m : g_cpp_modules)
            if (m.path == abs) { already = true; break; }
        if (already) { ++loaded; continue; }

        /* THE LOAD IS TRIED FIRST AND EXPLAINED SECOND.  The probes in
         * explain_load_failure() each map and unmap the library, so paying for
         * them on the happy path would map every module twice for nothing. */
        JceCppModule *m = jce_script_vm_cpp_load_library(abs.c_str());
        if (!m) { explain_load_failure(abs.c_str()); continue; }

        g_cpp_modules.push_back(LoadedModule{abs, m});
        ++loaded;

        /* The timestamp is the only cheap evidence that a module which loaded
         * cleanly is nonetheless yesterday's build.  Nothing here can prove
         * it is current; printing it is what lets a reader notice. */
        int64_t mtime = 0;
        if (jce_fs_host_get_mtime(abs.c_str(), &mtime)) {
            LOG_INFO(LOG_TAG,
                     "native script module loaded: '%s' from %s (built %lld epoch-s)",
                     jce_script_vm_cpp_module_name(m), abs.c_str(),
                     (long long)mtime);
        } else {
            LOG_INFO(LOG_TAG, "native script module loaded: '%s' from %s",
                     jce_script_vm_cpp_module_name(m), abs.c_str());
        }
    }

    if (!want.empty() || !g_cpp_modules.empty()) {
        LOG_INFO(LOG_TAG,
                 "native script modules for this project: %d of %d loaded. "
                 "Play resolves a .jcecpp OR .jcec scriptPath to a class "
                 "inside them.",
                 loaded, (int)want.size());
    }
#endif
}
