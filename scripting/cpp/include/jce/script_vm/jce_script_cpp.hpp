/*
 * jce_script_cpp.hpp — write a script class, get a module.
 *
 * This is the half a script author touches.  jce_script_vm_cpp.h is the C ABI
 * between the engine and a module; this header turns a C++ class into that
 * ABI, and the two jobs it does are the two that CANNOT be done anywhere else.
 *
 * ── IT IS HEADER-ONLY, AND FOR A DIFFERENT REASON THAN THE WRAPPER ───────
 *
 * scripting/cpp's call-DOWN wrapper (jce_script_api.hpp) is header-only
 * because `Api::open`'s default host_size must be evaluated in the CALLER's
 * translation unit — a compiled wrapper would bake its own sizeof into the
 * library and defeat the min(caller, engine) copy.
 *
 * That argument does NOT transfer to the VM core.  jce_script_vm_cpp.c is
 * COMPILED, deliberately: it never originates a host_size (it receives one
 * from create_sized and clamps against its own sizeof, which is exactly the
 * size of the buffer it writes into — so its own TU's sizeof is the correct
 * one, not a leak), and it owns the module registry, which must be ONE object
 * per process.  A header-only registry would give each translation unit — and
 * on Windows each module, since an `inline` variable is not shared across a
 * DLL boundary — its own copy, so a module registered by one half of a
 * program would be invisible to the other.
 *
 * The argument transfers to THIS header twice over, and both are stronger
 * than the original:
 *
 *   1. THE CATCH MUST BE COMPILED IN THE MODULE.  The thunks below are the
 *      only frames between C++ code that can throw and a C function pointer
 *      the engine calls.  Generating them here puts the try/catch in the
 *      module's TU, with the module's runtime.  Catching in the ENGINE
 *      instead would mean an exception object thrown by one CRT and caught by
 *      another, which on Windows is not a defined interaction — and unwinding
 *      through the engine's C frames to reach that catch is undefined before
 *      the catch is even considered.
 *   2. `JceCppScriptClass::struct_size` AND `JceCppModuleDesc::struct_size`
 *      must be the MODULE's sizeof, because the engine clamps to
 *      min(module, engine).  A compiled helper would publish the helper's
 *      sizeof, which is the same defect the wrapper avoids, one level down.
 *
 * ── WRITING A SCRIPT ─────────────────────────────────────────────────────
 *
 *     #include <jce/script_vm/jce_script_cpp.hpp>
 *
 *     class Spinner : public jce::script::Script {
 *     public:
 *         void on_start() override { angle_ = 0.0f; }
 *         void on_update(float dt) override {
 *             angle_ += 90.0f * dt;
 *             api().set_rotation(entity(), 0.0f, angle_, 0.0f);
 *         }
 *     private:
 *         float angle_ = 0.0f;
 *     };
 *
 *     JCE_CPP_SCRIPT_CLASS(Spinner, "Spinner")
 *
 *     JCE_CPP_MODULE_BEGIN()
 *         JCE_CPP_MODULE_CLASS(Spinner)
 *     JCE_CPP_MODULE_GLOBALS()
 *     JCE_CPP_MODULE_END("demo", demo_module)
 *
 * Linked into the game:
 *     jce_script_vm_cpp_register();
 *     jce_script_vm_cpp_add_module(demo_module());
 * Built as a shared object: JCE_CPP_MODULE_END also emits the exported entry
 * point, and the host calls jce_script_vm_cpp_load_library(absolute_path).
 * Either way the engine then reaches it through the ordinary lifecycle:
 *     jce_script_instantiate(s, "Spinner", entity);
 *
 * `api()` is jce::script::Api over scripting/c_abi — the shipped call-down
 * path.  It is the ONLY engine access this header provides, which is what
 * keeps `read_file` (and with it asset_read_text / asset_read_json's sandbox
 * policy) out of reach: that member is not in the C ABI at all.
 */
#ifndef JCE_SCRIPT_CPP_HPP
#define JCE_SCRIPT_CPP_HPP

#if defined(_MSVC_LANG)
#  if _MSVC_LANG < 201703L
#    error "jce_script_cpp.hpp requires C++17 or later (/std:c++17)"
#  endif
#elif __cplusplus < 201703L
#  error "jce_script_cpp.hpp requires C++17 or later (-std=c++17)"
#endif

#include <jce/script_vm/jce_script_vm_cpp.h>
#include <jce/script_api/jce_script_api.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

/* Exporting the module entry point.  Only the exported half of
 * JCE_CPP_MODULE_END uses it; a statically linked module never needs it. */
#if defined(_WIN32)
#  define JCE_CPP_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define JCE_CPP_MODULE_EXPORT __attribute__((visibility("default")))
#else
#  define JCE_CPP_MODULE_EXPORT
#endif

namespace jce {
namespace script {

/* The base every script class derives from.
 *
 * The virtuals default to nothing, which is how "this class does not define
 * on_collision" is spelled — but a default virtual is NOT how the engine
 * learns that, because a defaulted override is indistinguishable from an
 * implemented one at runtime.  JCE_CPP_SCRIPT_CLASS detects it at COMPILE
 * time (`&Class::on_collision == &Script::on_collision`) and leaves the ABI
 * slot NULL, so a class that declines a callback costs no dispatch and reads
 * to the engine exactly as a Lua script with no such method does.
 * *Enforced by:* tests/scripting/cpp/test_jce_script_vm_cpp_lifecycle.cpp ::
 * "a class that overrides no callback leaves every optional ABI slot NULL". */
class Script {
public:
    virtual ~Script() = default;

    virtual void on_start() {}
    virtual void on_update(float dt) { (void)dt; }
    virtual void on_destroy() {}
    virtual void on_collision(JceScriptEntity other_entity) { (void)other_entity; }
    virtual void on_message(const char *msg_name, double number_arg,
                            const char *str_arg)
    { (void)msg_name; (void)number_arg; (void)str_arg; }
    virtual void on_anim_event(std::uint32_t id, const char *name,
                               float f0, float f1, int i0)
    { (void)id; (void)name; (void)f0; (void)f1; (void)i0; }

    /* The owner entity passed to jce_script_instantiate — the Lua instance
     * table's `self.entity`, in C++. */
    JceScriptEntity entity() const noexcept { return jce_entity_; }
    const char *class_name() const noexcept { return jce_class_name_.c_str(); }

    /* The call-down surface.  One Api per instance, opened from the VM's own
     * clamped host copy — see JceCppScriptContext. */
    Api &api() noexcept { return jce_api_; }
    const Api &api() const noexcept { return jce_api_; }

    /* The host_size the VM handed this instance: min(what the host passed to
     * jce_script_create_sized, the engine's own sizeof).  Exposed because it
     * is the one number in the context with a correctness rule attached — a
     * module that opens a second Api, or forwards the host onward, must use
     * THIS and not sizeof(JceScriptHost), or it re-widens a host the engine
     * deliberately narrowed.  It is also what makes the clamp observable
     * from a test at all.
     * *Enforced by:* test_jce_script_vm_cpp_lifecycle.cpp ::
     * "a short host is not read past its end, and the clamp reaches the
     * module". */
    std::size_t host_size() const noexcept { return jce_host_size_; }

    /* Members the thunks below fill.  Named with the jce_ prefix and left
     * public because ScriptThunks<T> is a different class template in a
     * header a user may specialise; a `friend` declaration that a user's own
     * ScriptThunks shadowed would fail at the point of use, in generated
     * code, with a message about access rather than about shadowing. */
    void jce_adopt(JceScriptEntity e, const char *cls, std::size_t host_size,
                   Api api) {
        jce_entity_     = e;
        jce_class_name_ = cls ? cls : "";
        jce_host_size_  = host_size;
        jce_api_        = static_cast<Api &&>(api);
    }
    /* Where a thunk parks the text of an escaped exception.  Per-INSTANCE, so
     * two instances throwing in the same tick do not overwrite each other,
     * and it outlives the thunk's return, which JceCppStatus requires. */
    const char *jce_stash_error(const char *what) {
        jce_error_ = what ? what : "(unknown exception)";
        return jce_error_.c_str();
    }

private:
    JceScriptEntity jce_entity_ = 0;
    std::size_t     jce_host_size_ = 0;
    std::string     jce_class_name_;
    std::string     jce_error_;
    Api             jce_api_;
};

/* The context clamp, in the direction this side sees it.
 *
 * JceCppScriptContext is allocated by the ENGINE and read here, so a module
 * built against a NEWER header than its engine would read past its end.  This
 * is the min() guard jce_script_vm_cpp.h promises, in one place rather than
 * at every member. */
inline bool context_reaches(const JceCppScriptContext *ctx,
                            std::size_t end_offset) noexcept
{
    return ctx != nullptr && ctx->struct_size >= end_offset;
}

#define JCE_CPP_CTX_REACHES(ctx, member)                                      \
    ::jce::script::context_reaches(                                           \
        (ctx), offsetof(JceCppScriptContext, member) + sizeof((ctx)->member))

/* Every thunk the ABI needs for one class, as static member functions so the
 * macro below is a table of addresses and not a wall of lambdas.
 *
 * EVERY ONE OF THEM IS noexcept AND CATCHES (...).  `noexcept` is not
 * decoration: it makes an escaped exception a std::terminate at a named site
 * instead of an unwind through the engine's C frames, if the catch below is
 * ever narrowed. */
template <class T>
struct ScriptThunks {
    static void *create(const JceCppScriptContext *ctx) noexcept {
        try {
            if (!JCE_CPP_CTX_REACHES(ctx, class_name)) return nullptr;
            if (ctx->host == nullptr || ctx->host_size == 0) return nullptr;
            T *obj = new T();
            /* host_size is the VM's CLAMPED size, forwarded verbatim.
             * Passing sizeof(JceScriptHost) here would re-widen a short host
             * the engine already narrowed — the exact over-read the whole
             * chain exists to stop. */
            static_cast<Script *>(obj)->jce_adopt(
                ctx->entity, ctx->class_name, ctx->host_size,
                Api::open(*ctx->host, ctx->host_size, JCE_SCRIPT_API_VERSION));
            return obj;
        } catch (...) {
            return nullptr;
        }
    }
    static void destroy(void *self) noexcept {
        try { delete static_cast<T *>(self); } catch (...) { }
    }

/* One thunk body.  The catch order matters: std::exception first so `what()`
 * reaches the log, then (...) so a `throw 7` is still contained. */
#define JCE_CPP_THUNK_BODY(CALL)                                              \
        T *obj = static_cast<T *>(self);                                      \
        if (!obj) return nullptr;                                             \
        try { CALL; return nullptr; }                                         \
        catch (const std::exception &e) {                                     \
            return static_cast< ::jce::script::Script *>(obj)                 \
                       ->jce_stash_error(e.what());                           \
        }                                                                     \
        catch (...) {                                                         \
            return static_cast< ::jce::script::Script *>(obj)                 \
                       ->jce_stash_error("unknown C++ exception");            \
        }

    static JceCppStatus on_start(void *self) noexcept
    { JCE_CPP_THUNK_BODY(obj->on_start()) }
    static JceCppStatus on_update(void *self, float dt) noexcept
    { JCE_CPP_THUNK_BODY(obj->on_update(dt)) }
    static JceCppStatus on_destroy(void *self) noexcept
    { JCE_CPP_THUNK_BODY(obj->on_destroy()) }
    static JceCppStatus on_collision(void *self, JceScriptEntity other) noexcept
    { JCE_CPP_THUNK_BODY(obj->on_collision(other)) }
    static JceCppStatus on_message(void *self, const char *n, double num,
                                   const char *str) noexcept
    { JCE_CPP_THUNK_BODY(obj->on_message(n, num, str)) }
    static JceCppStatus on_anim_event(void *self, std::uint32_t id,
                                      const char *name, float f0, float f1,
                                      int i0) noexcept
    { JCE_CPP_THUNK_BODY(obj->on_anim_event(id, name, f0, f1, i0)) }
};

/* "Does T override this callback?"  Comparing the pointer-to-member against
 * the base's is a constant expression, so the answer is folded at compile
 * time and a declined callback costs nothing at runtime. */
template <class T> constexpr bool overrides_on_start()
{ return static_cast<void (Script::*)()>(&T::on_start) != &Script::on_start; }
template <class T> constexpr bool overrides_on_update()
{ return static_cast<void (Script::*)(float)>(&T::on_update) != &Script::on_update; }
template <class T> constexpr bool overrides_on_destroy()
{ return static_cast<void (Script::*)()>(&T::on_destroy) != &Script::on_destroy; }
template <class T> constexpr bool overrides_on_collision()
{ return static_cast<void (Script::*)(JceScriptEntity)>(&T::on_collision) != &Script::on_collision; }
template <class T> constexpr bool overrides_on_message()
{ return static_cast<void (Script::*)(const char *, double, const char *)>(&T::on_message) != &Script::on_message; }
/* ── Global handlers ─────────────────────────────────────────────────────
 *
 * jce_script_call_named / _num / _str reach a GLOBAL function, not an
 * instance method.  In Lua that is a global in the VM's own state; in C++ it
 * is a free function, and it needs the same two things every callback needs:
 * a call-down surface, and a catch before the C ABI.
 *
 * A handler is written against `Api &`, and these templates supply the rest.
 * Writing the thunk by hand would work and is exactly what must not happen:
 * a hand-written thunk with no try/catch compiles, runs, and throws through
 * the engine's C frames on the first bad input. */
using GlobalEntityFn = void (*)(Api &api, JceScriptEntity e);
using GlobalNumFn    = void (*)(Api &api, JceScriptEntity e, double value);
using GlobalStrFn    = void (*)(Api &api, JceScriptEntity e, const char *str);

/* Where a global's escaped-exception text lives.  A global has no instance to
 * park it in, so it is thread_local rather than static: two threads
 * dispatching two UI handlers would otherwise overwrite one buffer while the
 * engine still holds the pointer. */
inline std::string &global_error_slot()
{
    static thread_local std::string slot;
    return slot;
}

inline JceCppStatus global_stash(const char *what)
{
    global_error_slot() = what ? what : "(unknown exception)";
    return global_error_slot().c_str();
}

/* An Api over the context's clamped host.  Opened per dispatch: a global
 * outlives no VM and owns no state, so there is nowhere to cache it that is
 * not a lifetime bug the first time a host is destroyed. */
inline Api global_api(const JceCppScriptContext *ctx)
{
    return Api::open(*ctx->host, ctx->host_size, JCE_SCRIPT_API_VERSION);
}

#define JCE_CPP_GLOBAL_GUARD(ctx)                                                 if (!JCE_CPP_CTX_REACHES((ctx), class_name) || (ctx)->host == nullptr ||          (ctx)->host_size == 0)                                                        return nullptr;

template <GlobalEntityFn F>
JceCppStatus global_entity_thunk(const JceCppScriptContext *ctx,
                                 JceScriptEntity e) noexcept
{
    JCE_CPP_GLOBAL_GUARD(ctx)
    try { Api a = global_api(ctx); F(a, e); return nullptr; }
    catch (const std::exception &ex) { return global_stash(ex.what()); }
    catch (...) { return global_stash("unknown C++ exception"); }
}

template <GlobalNumFn F>
JceCppStatus global_num_thunk(const JceCppScriptContext *ctx,
                              JceScriptEntity e, double value) noexcept
{
    JCE_CPP_GLOBAL_GUARD(ctx)
    try { Api a = global_api(ctx); F(a, e, value); return nullptr; }
    catch (const std::exception &ex) { return global_stash(ex.what()); }
    catch (...) { return global_stash("unknown C++ exception"); }
}

template <GlobalStrFn F>
JceCppStatus global_str_thunk(const JceCppScriptContext *ctx,
                              JceScriptEntity e, const char *str) noexcept
{
    JCE_CPP_GLOBAL_GUARD(ctx)
    try { Api a = global_api(ctx); F(a, e, str); return nullptr; }
    catch (const std::exception &ex) { return global_stash(ex.what()); }
    catch (...) { return global_stash("unknown C++ exception"); }
}

template <class T> constexpr bool overrides_on_anim_event()
{ return static_cast<void (Script::*)(std::uint32_t, const char *, float, float, int)>(&T::on_anim_event) != &Script::on_anim_event; }

}  // namespace script
}  // namespace jce

/* Declare the ABI table for a class.  Place it at namespace scope, once, in
 * one translation unit of the module.
 *
 * `sizeof(JceCppScriptClass)` — never a sum of members.  The engine copies
 * min(this, its own), and a hand-computed number that drifted would hand the
 * engine a length it then trusts. */
#define JCE_CPP_SCRIPT_CLASS(CLASS, NAME)                                     \
    static const JceCppScriptClass jce_cpp_class_##CLASS = {                  \
        sizeof(JceCppScriptClass),                                            \
        NAME,                                                                 \
        &::jce::script::ScriptThunks<CLASS>::create,                          \
        &::jce::script::ScriptThunks<CLASS>::destroy,                         \
        ::jce::script::overrides_on_start<CLASS>()                            \
            ? &::jce::script::ScriptThunks<CLASS>::on_start : nullptr,        \
        ::jce::script::overrides_on_update<CLASS>()                           \
            ? &::jce::script::ScriptThunks<CLASS>::on_update : nullptr,       \
        ::jce::script::overrides_on_destroy<CLASS>()                          \
            ? &::jce::script::ScriptThunks<CLASS>::on_destroy : nullptr,      \
        ::jce::script::overrides_on_collision<CLASS>()                        \
            ? &::jce::script::ScriptThunks<CLASS>::on_collision : nullptr,    \
        ::jce::script::overrides_on_message<CLASS>()                          \
            ? &::jce::script::ScriptThunks<CLASS>::on_message : nullptr,      \
        ::jce::script::overrides_on_anim_event<CLASS>()                       \
            ? &::jce::script::ScriptThunks<CLASS>::on_anim_event : nullptr,   \
    };

/* A named global handler — the C++ spelling of a global Lua function, which
 * is what jce_script_call_named / _num / _str reach.  Pass nullptr for the
 * arities this handler does not take; see JceCppScriptGlobal on why the
 * entity-only form is the fallback.
 *
 * THE POINTERS YOU PASS HERE ARE CALLED DIRECTLY BY THE ENGINE.  Unlike a
 * class callback, nothing between them and the C ABI belongs to this header,
 * so a raw `JceCppStatus (*)(JceScriptEntity)` you write yourself is the ONE
 * place in this backend where the no-exception policy is on you.  Use
 * JCE_CPP_SCRIPT_GLOBAL_FN below and it is not: it wraps an ordinary
 * `void f(...)` in the same noexcept + try/catch shape ScriptThunks gives a
 * class, in this module's translation unit.
 * *Enforced by:* tests/scripting/cpp/test_jce_script_vm_cpp_lifecycle.cpp ::
 * "a throwing global handler is caught at the module boundary". */
#define JCE_CPP_SCRIPT_GLOBAL(IDENT, NAME, FN_ENTITY, FN_NUM, FN_STR)         \
    static const JceCppScriptGlobal jce_cpp_global_##IDENT = {                \
        sizeof(JceCppScriptGlobal), NAME, FN_ENTITY, FN_NUM, FN_STR,          \
    };

/* Wrap `FN` (an ordinary function that may throw) in a catching thunk of the
 * named arity.  ARITY is one of ENTITY / NUM / STR and picks the signature:
 *
 *   ENTITY   void FN(Api &, JceScriptEntity)
 *   NUM      void FN(Api &, JceScriptEntity, double)
 *   STR      void FN(Api &, JceScriptEntity, const char *)
 *
 * The Api is opened PER CALL, from the context the engine builds — a global
 * handler has no instance to hold one on.  That cost is a UI interaction, not
 * a frame: jce_script_call_named* fire from a click, a slider drag or a text
 * commit, never from the per-instance update pass.
 *
 * The text of an escaped exception has no instance to live on either, so it
 * goes in one thread_local buffer — thread_local and not static because a
 * JceScript belongs to the thread that created it and two of them must not
 * scribble on each other's message. */
namespace jce {
namespace script {

inline const char *stash_global_error(const char *what) noexcept
{
    static thread_local char buf[256];
    const char *m = (what != nullptr) ? what : "unknown C++ exception";
    std::size_t n = 0;
    while (m[n] != '\0' && n + 1u < sizeof(buf)) { buf[n] = m[n]; ++n; }
    buf[n] = '\0';
    return buf;
}

/* An Api over the context's host, or an EMPTY Api.  Empty is not a failure
 * mode the caller has to branch on: every method on an empty Api is still
 * callable and answers false / 0 / nullopt, which is what the C ABI does for
 * an absent host member anyway. */
inline Api api_from(const JceCppScriptContext *ctx) noexcept
{
    if (!context_reaches(ctx, offsetof(JceCppScriptContext, host_size) +
                                  sizeof(std::size_t)))
        return Api();
    if (ctx->host == nullptr || ctx->host_size == 0) return Api();
    /* host_size is the VM's CLAMPED number, forwarded verbatim.  Passing
     * sizeof(JceScriptHost) here would re-widen a host the engine narrowed. */
    return Api::open(*ctx->host, ctx->host_size, JCE_SCRIPT_API_VERSION);
}

}  // namespace script
}  // namespace jce

#define JCE_CPP_GLOBAL_THUNK_ENTITY(THUNK, FN)                                \
    static JceCppStatus THUNK(const JceCppScriptContext *ctx,                 \
                              JceScriptEntity e) noexcept                     \
    {                                                                         \
        try {                                                                 \
            ::jce::script::Api a = ::jce::script::api_from(ctx);              \
            FN(a, e);                                                         \
            return nullptr;                                                   \
        }                                                                     \
        catch (const std::exception &ex)                                      \
        { return ::jce::script::stash_global_error(ex.what()); }              \
        catch (...) { return ::jce::script::stash_global_error(nullptr); }    \
    }

#define JCE_CPP_GLOBAL_THUNK_NUM(THUNK, FN)                                   \
    static JceCppStatus THUNK(const JceCppScriptContext *ctx,                 \
                              JceScriptEntity e, double v) noexcept           \
    {                                                                         \
        try {                                                                 \
            ::jce::script::Api a = ::jce::script::api_from(ctx);              \
            FN(a, e, v);                                                      \
            return nullptr;                                                   \
        }                                                                     \
        catch (const std::exception &ex)                                      \
        { return ::jce::script::stash_global_error(ex.what()); }              \
        catch (...) { return ::jce::script::stash_global_error(nullptr); }    \
    }

#define JCE_CPP_GLOBAL_THUNK_STR(THUNK, FN)                                   \
    static JceCppStatus THUNK(const JceCppScriptContext *ctx,                 \
                              JceScriptEntity e, const char *s) noexcept      \
    {                                                                         \
        try {                                                                 \
            ::jce::script::Api a = ::jce::script::api_from(ctx);              \
            FN(a, e, s);                                                      \
            return nullptr;                                                   \
        }                                                                     \
        catch (const std::exception &ex)                                      \
        { return ::jce::script::stash_global_error(ex.what()); }              \
        catch (...) { return ::jce::script::stash_global_error(nullptr); }    \
    }

/* The whole of a safe global: thunk + table, one line at namespace scope. */
#define JCE_CPP_SCRIPT_GLOBAL_FN(IDENT, NAME, ARITY, FN)                      \
    JCE_CPP_GLOBAL_THUNK_##ARITY(jce_cpp_gthunk_##IDENT, FN)                  \
    static const JceCppScriptGlobal jce_cpp_global_##IDENT =                  \
        JCE_CPP_GLOBAL_TABLE_##ARITY(NAME, jce_cpp_gthunk_##IDENT);

#define JCE_CPP_GLOBAL_TABLE_ENTITY(NAME, THUNK)                              \
    { sizeof(JceCppScriptGlobal), NAME, THUNK, nullptr, nullptr }
#define JCE_CPP_GLOBAL_TABLE_NUM(NAME, THUNK)                                 \
    { sizeof(JceCppScriptGlobal), NAME, nullptr, THUNK, nullptr }
#define JCE_CPP_GLOBAL_TABLE_STR(NAME, THUNK)                                 \
    { sizeof(JceCppScriptGlobal), NAME, nullptr, nullptr, THUNK }

/* The module table, and (for a shared object) the one exported symbol.
 *
 * The trailing nullptr in each array is what makes an EMPTY list legal: C++
 * has no zero-length array, and a module with classes but no globals is the
 * common case.  The counts subtract it.
 *
 * JCE_CPP_MODULE_END emits BOTH the descriptor accessor — which a statically
 * linked host calls directly — and the exported `jce_cpp_script_module` entry
 * point.  Emitting both from one macro is deliberate: a module that ships as
 * a static library today and as a plugin tomorrow must not have to be edited,
 * and a plugin whose author forgot the export is a load that fails long after
 * the build succeeded. */
#define JCE_CPP_MODULE_BEGIN()                                                \
    static const JceCppScriptClass *const jce_cpp_module_classes[] = {

#define JCE_CPP_MODULE_CLASS(CLASS) &jce_cpp_class_##CLASS,

#define JCE_CPP_MODULE_GLOBALS()                                              \
        nullptr };                                                            \
    static const JceCppScriptGlobal *const jce_cpp_module_globals[] = {

#define JCE_CPP_GLOBAL_ENTITY(FN) (&::jce::script::global_entity_thunk<FN>)
#define JCE_CPP_GLOBAL_NUM(FN)    (&::jce::script::global_num_thunk<FN>)
#define JCE_CPP_GLOBAL_STR(FN)    (&::jce::script::global_str_thunk<FN>)

#define JCE_CPP_MODULE_GLOBAL(IDENT) &jce_cpp_global_##IDENT,

#define JCE_CPP_MODULE_END(MODULE_NAME, ACCESSOR)                             \
        nullptr };                                                            \
    static const JceCppModuleDesc jce_cpp_module_desc = {                     \
        sizeof(JceCppModuleDesc),                                             \
        JCE_CPP_MODULE_ABI_VERSION,                                           \
        MODULE_NAME,                                                          \
        jce_cpp_module_classes,                                               \
        (sizeof(jce_cpp_module_classes) /                                     \
         sizeof(jce_cpp_module_classes[0])) - 1u,                             \
        jce_cpp_module_globals,                                               \
        (sizeof(jce_cpp_module_globals) /                                     \
         sizeof(jce_cpp_module_globals[0])) - 1u,                             \
    };                                                                        \
    const JceCppModuleDesc *ACCESSOR(void) { return &jce_cpp_module_desc; }   \
    JCE_CPP_MODULE_ENTRY_

/* ── THE EXPORTED ENTRY IS PER SHARED OBJECT, NOT PER TRANSLATION UNIT ───
 *
 * `jce_cpp_script_module` is ONE external symbol, so an EXECUTABLE that
 * statically links TWO native script modules — two C++ ones, or a C++ one and
 * a C one, which is the same registry either way — gets two definitions and
 * does not link:
 *
 *     MSVC : error LNK2005: jce_cpp_script_module already defined in <obj>
 *     GNU  : multiple definition of `jce_cpp_script_module`
 *
 * The comment above JCE_CPP_MODULE_EXPORT already says "a statically linked
 * module never needs it".  It was right and the macro emitted it anyway.
 *
 * DEFINE JCE_SCRIPT_MODULE_NO_ENTRY on the compile that goes INTO THE BINARY;
 * leave it undefined for the shared-object build, where the entry is the only
 * door and suppressing it yields a module that loads and is then rejected as
 * "not a JCE script module" — a failure that survives shipping, unlike a link
 * error.  The default is unchanged, so no existing module needs editing, and
 * jce_script_vm_c.h honours the same macro. */
#if defined(JCE_SCRIPT_MODULE_NO_ENTRY)
#  define JCE_CPP_MODULE_ENTRY_ /* suppressed: linked INTO a binary */
#else
#  define JCE_CPP_MODULE_ENTRY_                                               \
    extern "C" JCE_CPP_MODULE_EXPORT const JceCppModuleDesc *                 \
    jce_cpp_script_module(uint32_t engine_abi_version)                        \
    {                                                                         \
        /* Refuse a host whose module ABI major we were not built for rather  \
         * than hand over a table it will misread.  The engine logs both      \
         * numbers. */                                                        \
        if (engine_abi_version != JCE_CPP_MODULE_ABI_VERSION) return nullptr; \
        return &jce_cpp_module_desc;                                          \
    }
#endif

#endif /* JCE_SCRIPT_CPP_HPP */
