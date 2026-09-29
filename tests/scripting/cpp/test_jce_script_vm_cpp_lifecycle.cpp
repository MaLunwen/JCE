/* test_jce_script_vm_cpp_lifecycle.cpp — the cpp JceScriptVM, against Lua.
 *
 * ── WHAT THE ORACLE IS ───────────────────────────────────────────────────
 *
 * Lua is the reference LIFECYCLE.  It is a shipped, tested implementation of
 * the same eighteen slots, reached through the same public entry points, and
 * the engine drives it every frame.  So the acceptance for this backend is:
 * THE SAME SCRIPT SEMANTICS, WRITTEN TWICE — once in Lua and once as a C++
 * class — driven through jce_script_call_* and compared on
 *
 *   * the SEQUENCE OF HOST CALLS.  Both scripts report every callback they
 *     receive by calling one manifest entry (`ui_set_text`) on ONE recording
 *     mock host.  A backend that produced the right final state by firing the
 *     wrong callbacks is invisible to a state comparison and visible here.
 *   * TYPE AS WELL AS VALUE.  Every recorded field is "<type>|<value>", and
 *     the Lua side gets its type from `type(x)` — the reference's own answer,
 *     not mine.  `nil`, `0` and `false` are three different strings, so none
 *     can pass for another, and a dt that arrived as nil says so.
 *   * the RETURN of every call that has one (call_named / _num / _str,
 *     instance_count), recorded into the same sequence.
 *
 * ── THE LIVENESS GATE, WHICH RUNS BEFORE ANY COMPARISON ──────────────────
 *
 * A differential shipped in this worktree was 150/151 dead because the
 * reference side returned nothing, and NOTHING COMPARES EQUAL TO NOTHING.
 * So `require_lively()` runs on EACH SIDE INDEPENDENTLY, before the two are
 * ever compared, and it is not "the vector is non-empty": it demands that
 * every lifecycle KIND the drive sequence exercises appears at least once —
 * start, update, collision, message, anim, destroy, and the three global
 * arities.  A side that only managed on_start fails its own gate by name and
 * the comparison never runs.
 *
 * ── WHAT IS PINNED RATHER THAN COMPARED ──────────────────────────────────
 *
 * Three things genuinely differ and each is asserted by name, with the
 * expected value on BOTH sides, instead of being left out of the sweep:
 * instantiate_source, compile_module, and the text (not the presence) of the
 * error a failing callback logs.  An unpinned known divergence is an immune
 * mutation with a silent reason.
 */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>
#include <jce/script_vm/jce_script_cpp.hpp>
#include <jce/script_vm/jce_script_vm_cpp.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

/* ------------------------------------------------------------------ *
 *  1. The one recording mock host.
 *
 *  Both VMs are created over the SAME host table and the same `user`, so a
 *  difference in the trace is a difference in what the two backends did, not
 *  in what they were given.
 * ------------------------------------------------------------------ */

namespace {

struct Trace {
    std::vector<std::string> events;   /* compared between the two sides */
    std::vector<std::string> logs;     /* host->log; PRESENCE compared, text pinned */
};

Trace *tr_of(void *user) { return static_cast<Trace *>(user); }

}  // namespace

extern "C" {

static void mock_log(void *user, const char *msg)
{
    tr_of(user)->logs.push_back(msg != nullptr ? msg : "(null)");
}

static void mock_ui_set_text(void *user, JceScriptEntity e, const char *txt)
{
    char head[64];
    std::snprintf(head, sizeof head, "ui_set_text|%llu|",
                  static_cast<unsigned long long>(e));
    tr_of(user)->events.push_back(std::string(head) +
                                  (txt != nullptr ? txt : "(null)"));
}

/* The member the short-host test WITHHOLDS.  A poison that records rather
 * than crashes: a truncation whose only symptom is a segfault proves the
 * process died, and "a crash reports less than a red test" has been measured
 * four times in this campaign. */
static bool g_poison_locale_called;
static void mock_poison_set_locale(void *user, const char *locale)
{
    (void)user;
    (void)locale;
    g_poison_locale_called = true;
}

}  // extern "C"

namespace {

JceScriptHost make_host(Trace *tr)
{
    JceScriptHost h;
    std::memset(&h, 0, sizeof h);
    h.user           = tr;
    h.log            = mock_log;
    h.ui_set_text    = mock_ui_set_text;
    h.loc_set_locale = mock_poison_set_locale;
    return h;
}

/* ------------------------------------------------------------------ *
 *  2. The C++ side of the differential — one module, in this TU.
 * ------------------------------------------------------------------ */

std::string vnum(double x)
{
    char b[64];
    std::snprintf(b, sizeof b, "number|%.3f", x);
    return b;
}

std::string vstr(const char *s)
{
    return (s != nullptr) ? (std::string("string|") + s) : std::string("nil|");
}

}  // namespace

/* The C++ half of "the same script, written twice".  Every method records
 * exactly what the Lua half records, through the same manifest entry. */
class Probe : public jce::script::Script {
public:
    void on_start() override { rec("start"); }
    void on_update(float dt) override { rec("update|" + vnum(dt)); }
    void on_destroy() override { rec("destroy"); }
    void on_collision(JceScriptEntity other) override
    {
        rec("collision|" + vnum(static_cast<double>(other)));
    }
    void on_message(const char *name, double num, const char *str) override
    {
        rec(std::string("message|") + (name ? name : "?") + "|" + vnum(num) +
            "|" + vstr(str));
    }
    void on_anim_event(std::uint32_t id, const char *name, float f0, float f1,
                       int i0) override
    {
        rec("anim|" + vnum(id) + "|" + vstr(name) + "|" + vnum(f0) + "|" +
            vnum(f1) + "|" + vnum(i0));
    }

private:
    void rec(const std::string &s) { api().ui_set_text(entity(), s); }
};

/* Overrides nothing.  Its ABI table must leave every optional slot NULL, so
 * that "this class declines that callback" reads to the engine exactly as a
 * Lua module without the method does. */
class Silent : public jce::script::Script {};

/* Throws from its SECOND on_start onward.  jce_script_call_start has no
 * at-most-once contract, and this is the only class in the suite that can see
 * cpp_call_start passing CB_NONE instead of CB_START — every other case
 * dispatches on_start once, where disabling it and not disabling it look
 * exactly the same. */
class StartBoom : public jce::script::Script {
public:
    void on_start() override
    {
        ++n_;
        api().ui_set_text(entity(), "startboom|" + std::to_string(n_));
        if (n_ >= 2) throw std::runtime_error("start boom");
    }

private:
    int n_ = 0;
};

/* Throws on the SECOND update, after it has already made a host call.  The
 * host call before the throw is what proves the exception did not eat the
 * work that preceded it. */
class Boom : public jce::script::Script {
public:
    void on_update(float dt) override
    {
        (void)dt;
        ++n_;
        api().ui_set_text(entity(), "boom|update|" + std::to_string(n_));
        if (n_ == 2) throw std::runtime_error("boom");
    }

    /* A SECOND hook, throwing on ITS second call.  Two jobs:
     *
     *   - while on_update is disabled and this is not, it separates "on_update
     *     was disabled" from "the instance was dropped".  Without a second
     *     live callback on the same instance those two are one observation.
     *   - throwing later gives the rule a second hook to disable, which is
     *     what stops a backend from binding the WRONG bit (reporting
     *     on_collision but disabling on_update) or from gating only the one
     *     hook a test happens to drive. */
    void on_collision(JceScriptEntity other) override
    {
        ++c_;
        api().ui_set_text(entity(), "boom|collision|" + std::to_string(c_)
                                        + "|" + std::to_string(other));
        if (c_ == 2) throw std::runtime_error("crunch");
    }

    /* A THIRD hook, which never throws and which the rule does not cover.
     * jce_script_call_message does not participate in THE FAILING-CALLBACK
     * RULE in any backend, and this backend is the reason why: it routes every
     * message NAME through this one thunk, so "disable the offending callback"
     * would mean disabling every message the instance receives.  Driving it
     * once while on_update is disabled is what catches a backend that gated
     * messages on some other hook's bit. */
    void on_message(const char *name, double num, const char *str) override
    {
        (void)num;
        (void)str;
        api().ui_set_text(entity(),
                          std::string("boom|message|") + (name ? name : "?"));
    }

private:
    int n_ = 0;
    int c_ = 0;
};

/* Reports the host_size the VM handed it, and calls the entry point whose
 * host member the short-host test withholds. */
class ClampProbe : public jce::script::Script {
public:
    void on_start() override
    {
        api().set_locale("xx");
        api().ui_set_text(entity(),
                          "clamp|host_size|" + std::to_string(host_size()));
    }
};

namespace {

/* A global handler has no instance and therefore no member Api; the engine
 * builds it a context and the generated thunk opens one per call.  That is
 * the whole reason these take an Api by reference: they reach the host
 * through the SAME call-down surface an instance method uses, so the record
 * lands in the same sequence and the comparison stays one comparison. */
void g_click_fn(jce::script::Api &api, JceScriptEntity e)
{
    api.ui_set_text(e, "g_click|" + vnum(static_cast<double>(e)));
}

void g_slider_fn(jce::script::Api &api, JceScriptEntity e, double v)
{
    api.ui_set_text(e,
                    "g_slider|" + vnum(static_cast<double>(e)) + "|" + vnum(v));
}

void g_text_fn(jce::script::Api &api, JceScriptEntity e, const char *s)
{
    api.ui_set_text(e,
                    "g_text|" + vnum(static_cast<double>(e)) + "|" + vstr(s));
}

void g_throw_fn(jce::script::Api &api, JceScriptEntity e)
{
    (void)api;
    (void)e;
    throw std::runtime_error("global boom");
}

}  // namespace

JCE_CPP_SCRIPT_CLASS(Probe, "Probe")
JCE_CPP_SCRIPT_CLASS(Silent, "Silent")
JCE_CPP_SCRIPT_CLASS(Boom, "Boom")
JCE_CPP_SCRIPT_CLASS(StartBoom, "StartBoom")
JCE_CPP_SCRIPT_CLASS(ClampProbe, "ClampProbe")

JCE_CPP_SCRIPT_GLOBAL_FN(g_click, "g_click", ENTITY, g_click_fn)
JCE_CPP_SCRIPT_GLOBAL_FN(g_slider, "g_slider", NUM, g_slider_fn)
JCE_CPP_SCRIPT_GLOBAL_FN(g_text, "g_text", STR, g_text_fn)
JCE_CPP_SCRIPT_GLOBAL_FN(g_throw, "g_throw", ENTITY, g_throw_fn)

JCE_CPP_MODULE_BEGIN()
    JCE_CPP_MODULE_CLASS(Probe)
    JCE_CPP_MODULE_CLASS(Silent)
    JCE_CPP_MODULE_CLASS(Boom)
    JCE_CPP_MODULE_CLASS(StartBoom)
    JCE_CPP_MODULE_CLASS(ClampProbe)
JCE_CPP_MODULE_GLOBALS()
    JCE_CPP_MODULE_GLOBAL(g_click)
    JCE_CPP_MODULE_GLOBAL(g_slider)
    JCE_CPP_MODULE_GLOBAL(g_text)
    JCE_CPP_MODULE_GLOBAL(g_throw)
JCE_CPP_MODULE_END("probe", jce_cpp_probe_module)

/* ------------------------------------------------------------------ *
 *  3. The Lua side — the SAME script, in the reference language.
 * ------------------------------------------------------------------ */

namespace {

const char *const kLuaProbe = R"LUA(
local S = {}

-- The type comes from Lua's own `type()`, not from the test's belief about
-- what should have arrived.  That is what makes "nil vs 0 vs false" a real
-- comparison rather than a formatting convention.
local function v(x)
  local t = type(x)
  if t == "nil"     then return "nil|" end
  if t == "number"  then return "number|" .. string.format("%.3f", x) end
  if t == "string"  then return "string|" .. x end
  if t == "boolean" then return "boolean|" .. tostring(x) end
  return t .. "|?"
end

local function rec(self, s) jce.ui_set_text(self.entity, s) end

function S:on_start()          rec(self, "start") end
function S:on_update(dt)       rec(self, "update|" .. v(dt)) end
function S:on_destroy()        rec(self, "destroy") end
function S:on_collision(other) rec(self, "collision|" .. v(other)) end
function S:ping(num, str)
  rec(self, "message|ping|" .. v(num) .. "|" .. v(str))
end
function S:on_anim_event(id, name, f0, f1, i0)
  rec(self, "anim|" .. v(id) .. "|" .. v(name) .. "|" ..
            v(f0) .. "|" .. v(f1) .. "|" .. v(i0))
end

function g_click(e)      jce.ui_set_text(e, "g_click|" .. v(e)) end
function g_slider(e, x)  jce.ui_set_text(e, "g_slider|" .. v(e) .. "|" .. v(x)) end
function g_text(e, s)    jce.ui_set_text(e, "g_text|" .. v(e) .. "|" .. v(s)) end
function g_throw(e)      error("global boom") end

return S
)LUA";

const char *const kLuaBoom = R"LUA(
local S = {}
local n = 0
function S:on_update(dt)
  n = n + 1
  jce.ui_set_text(self.entity, "boom|update|" .. tostring(n))
  if n == 2 then error("boom") end
end
local c = 0
function S:on_collision(other)
  c = c + 1
  jce.ui_set_text(self.entity,
                  "boom|collision|" .. tostring(c) .. "|" .. tostring(other))
  if c == 2 then error("crunch") end
end
function S:poke(n, s)
  jce.ui_set_text(self.entity, "boom|message|poke")
end
return S
)LUA";

/* The Lua spelling of `class StartBoom`. */
const char *const kLuaStartBoom = R"LUA(
local S = {}
local n = 0
function S:on_start()
  n = n + 1
  jce.ui_set_text(self.entity, "startboom|" .. tostring(n))
  if n >= 2 then error("start boom") end
end
return S
)LUA";

/* A module with no methods at all — the Lua spelling of `class Silent`. */
const char *const kLuaSilent = "return {}";

/* ------------------------------------------------------------------ *
 *  4. One drive sequence, run against either backend.
 * ------------------------------------------------------------------ */

const JceScriptEntity kOwner = 42u;

void drive(JceScript *s, JceScriptInstance inst, Trace &tr)
{
    jce_script_call_start(s, inst);
    jce_script_call_update(s, inst, 0.25f);
    jce_script_call_update(s, inst, 0.5f);
    jce_script_call_collision(s, inst, 77u);
    jce_script_call_message(s, inst, "ping", 2.5, "hello");
    jce_script_call_message(s, inst, "ping", -1.0, nullptr);
    jce_script_call_anim_event(s, inst, 9u, "footstep", 1.5f, -0.25f, 7);
    jce_script_call_anim_event(s, inst, 10u, nullptr, 0.0f, 0.0f, 0);

    tr.events.push_back(std::string("ret|call_named|g_click|") +
                        (jce_script_call_named(s, "g_click", 5u) ? "true"
                                                                 : "false"));
    tr.events.push_back(
        std::string("ret|call_named_num|g_slider|") +
        (jce_script_call_named_num(s, "g_slider", 6u, 0.75) ? "true" : "false"));
    tr.events.push_back(
        std::string("ret|call_named_str|g_text|") +
        (jce_script_call_named_str(s, "g_text", 7u, "typed") ? "true"
                                                             : "false"));
    tr.events.push_back(
        std::string("ret|call_named_str|g_text_nil|") +
        (jce_script_call_named_str(s, "g_text", 7u, nullptr) ? "true"
                                                             : "false"));
    /* No such global: false on both sides, and the two silent slots are the
     * ones whose absence is invisible, so this case is not optional. */
    tr.events.push_back(
        std::string("ret|call_named|missing|") +
        (jce_script_call_named(s, "no_such_global", 8u) ? "true" : "false"));
    /* ARITY FALLBACK.  Lua invokes `function g_click(e)` for a _num call and
     * returns true, dropping the extra argument; the cpp VM must agree, or a
     * UISlider bound to a one-parameter handler goes dead with no error. */
    tr.events.push_back(
        std::string("ret|call_named_num|g_click_arity|") +
        (jce_script_call_named_num(s, "g_click", 9u, 3.5) ? "true" : "false"));

    tr.events.push_back("count|" +
                        std::to_string(jce_script_instance_count(s)));
    jce_script_update_coroutines(s, 0.016f);
    jce_script_release(s, inst);
    tr.events.push_back("count_after_release|" +
                        std::to_string(jce_script_instance_count(s)));
}

/* ------------------------------------------------------------------ *
 *  5. The liveness gate.  Runs on EACH SIDE, before any comparison.
 * ------------------------------------------------------------------ */

bool contains_kind(const std::vector<std::string> &v, const char *needle)
{
    for (const std::string &s : v)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

/* ONLY the host calls the SCRIPT made.
 *
 * `drive()` pushes its own bookkeeping into the same vector — "ret|call_named_
 * str|g_text|false", "count|1" — and those lines contain the very substrings
 * this gate searches for.  Measured, not feared: with cpp_call_named_str
 * stubbed to return false unconditionally, the cpp side produced no g_text
 * event at all and the "|g_text|" needle STILL matched, because the driver's
 * own "ret|call_named_str|g_text|false" record contains it.  Three of the nine
 * script kinds — the two silent slots and call_named, i.e. exactly the ones
 * whose absence is invisible — were therefore being proved live by the thing
 * that cannot fail.  Every event the script causes arrives through the mock's
 * ui_set_text, so scoping the search to that prefix is what makes the gate
 * about the SCRIPT rather than about the driver. */
std::vector<std::string> script_events(const std::vector<std::string> &v)
{
    std::vector<std::string> out;
    for (const std::string &s : v)
        if (s.rfind("ui_set_text|", 0) == 0) out.push_back(s);
    return out;
}

void require_lively(const char *side, const Trace &tr)
{
    /* Nine kinds the SCRIPT must have produced.  "count|" is not among them:
     * it is a driver record by construction and could never be evidence that
     * the other side ran. */
    static const char *const kKinds[] = {
        "|start",   "|update|", "|collision|", "|message|ping|",
        "|anim|",   "|destroy", "|g_click|",   "|g_slider|",
        "|g_text|",
    };
    const std::vector<std::string> ev = script_events(tr.events);
    INFO("side = " << std::string(side));
    REQUIRE_MESSAGE(!ev.empty(),
                    "the " << std::string(side)
                           << " side produced NO host call from the script at "
                              "all; nothing compares equal to nothing, so the "
                              "comparison below would pass vacuously");
    for (const char *k : kKinds) {
        INFO("missing kind: " << std::string(k));
        REQUIRE_MESSAGE(contains_kind(ev, k),
                        "the " << std::string(side) << " side never produced a '"
                               << std::string(k)
                               << "' event FROM THE SCRIPT — this side is not "
                                  "live enough to be a differential operand");
    }
    /* The driver's own record, checked separately and named as such. */
    REQUIRE_MESSAGE(contains_kind(tr.events, "count|"),
                    "the " << std::string(side)
                           << " side's driver never recorded instance_count");
}

void dump(const char *side, const std::vector<std::string> &v)
{
    for (std::size_t i = 0; i < v.size(); ++i)
        MESSAGE(std::string(side) << "[" << i << "] " << v[i]);
}

/* ------------------------------------------------------------------ *
 *  6. Registration, done once for the whole binary.
 * ------------------------------------------------------------------ */

}  // namespace

/* Registration happens ONCE per process, not once per TEST_CASE: the registry
 * refuses a duplicate language and a duplicate module name, so a per-case
 * registration would report "already registered" and read as a defect.
 * External linkage because the module-ABI half of this suite lives in a
 * second translation unit and must not register a second copy. */
bool jce_cpp_test_ensure_registered()
{
    static bool ok = [] {
        const bool r = jce_script_vm_cpp_register();
        const bool m =
            jce_script_vm_cpp_add_module(jce_cpp_probe_module()) != nullptr;
        return r && m;
    }();
    return ok;
}

namespace {
bool ensure_registered() { return jce_cpp_test_ensure_registered(); }
}  // namespace

/* ================================================================== *
 *  THE DIFFERENTIAL
 * ================================================================== */

TEST_CASE("the cpp VM and Lua agree on the whole lifecycle")
{
    REQUIRE(ensure_registered());

    Trace lua_tr;
    Trace cpp_tr;

    {
        JceScriptHost h = make_host(&lua_tr);
        JceScript *s    = jce_script_create_sized(&h, sizeof h);
        REQUIRE(s != nullptr);
        JceScriptInstance i =
            jce_script_instantiate_source(s, "@probe", kLuaProbe, kOwner);
        REQUIRE_MESSAGE(i != 0,
                        "the Lua reference chunk did not produce an instance; "
                        "every comparison below would be against an empty "
                        "trace");
        drive(s, i, lua_tr);
        jce_script_destroy(s);
    }

    {
        JceScriptHost h = make_host(&cpp_tr);
        JceScript *s    = jce_script_vm_create("cpp", &h, sizeof h);
        REQUIRE_MESSAGE(s != nullptr,
                        "jce_script_vm_create(\"cpp\") returned NULL — either "
                        "the language is not registered or the handle failed "
                        "jce_script_vm_create's JceScriptVMHeader check");
        /* THE ENTRY POINT THAT MEANS "MAKE ME AN INSTANCE OF THIS SCRIPT".
         * For Lua that is instantiate_source (no file in a unit test); for a
         * compiled class it is instantiate, whose `path` is a class name.
         * Both are the public lifecycle. */
        JceScriptInstance i = jce_script_instantiate(s, "Probe", kOwner);
        REQUIRE_MESSAGE(i != 0,
                        "jce_script_instantiate(s, \"Probe\") returned 0 — the "
                        "class registry did not resolve the name");
        drive(s, i, cpp_tr);
        jce_script_destroy(s);
    }

    /* BOTH SIDES PROVE THEY RAN, INDEPENDENTLY, BEFORE ANY COMPARISON. */
    require_lively("lua", lua_tr);
    require_lively("cpp", cpp_tr);

    if (lua_tr.events != cpp_tr.events) {
        dump("lua", lua_tr.events);
        dump("cpp", cpp_tr.events);
    }
    CHECK_MESSAGE(lua_tr.events.size() == cpp_tr.events.size(),
                  "the two sides produced a different NUMBER of host calls");
    const std::size_t n = lua_tr.events.size() < cpp_tr.events.size()
                              ? lua_tr.events.size()
                              : cpp_tr.events.size();
    for (std::size_t k = 0; k < n; ++k) {
        INFO("event index " << k);
        CHECK_MESSAGE(lua_tr.events[k] == cpp_tr.events[k],
                      "lua: " << lua_tr.events[k]
                              << "  cpp: " << cpp_tr.events[k]);
    }

    /* One divergence that IS expected and is therefore stated: g_throw is
     * driven by the error test, not here, so neither side may have logged. */
    CHECK_MESSAGE(lua_tr.logs.empty(),
                  "the Lua side logged during a run with no errors in it");
    CHECK_MESSAGE(cpp_tr.logs.empty(),
                  "the cpp side logged during a run with no errors in it");
}

TEST_CASE("a script that defines no callbacks is a clean no-op on both sides")
{
    REQUIRE(ensure_registered());

    Trace lua_tr;
    Trace cpp_tr;

    {
        JceScriptHost h = make_host(&lua_tr);
        JceScript    *s = jce_script_create_sized(&h, sizeof h);
        REQUIRE(s != nullptr);
        JceScriptInstance i =
            jce_script_instantiate_source(s, "@silent", kLuaSilent, kOwner);
        REQUIRE(i != 0);
        jce_script_call_start(s, i);
        jce_script_call_update(s, i, 0.25f);
        jce_script_call_collision(s, i, 1u);
        jce_script_call_anim_event(s, i, 1u, "x", 0.f, 0.f, 0);
        jce_script_release(s, i);
        jce_script_destroy(s);
    }
    {
        JceScriptHost h = make_host(&cpp_tr);
        JceScript    *s = jce_script_vm_create("cpp", &h, sizeof h);
        REQUIRE(s != nullptr);
        JceScriptInstance i = jce_script_instantiate(s, "Silent", kOwner);
        REQUIRE(i != 0);
        jce_script_call_start(s, i);
        jce_script_call_update(s, i, 0.25f);
        jce_script_call_collision(s, i, 1u);
        jce_script_call_anim_event(s, i, 1u, "x", 0.f, 0.f, 0);
        jce_script_release(s, i);
        jce_script_destroy(s);
    }

    CHECK_MESSAGE(lua_tr.events.empty(),
                  "the Lua no-method module made a host call it should not "
                  "have — the premise of this comparison is wrong");
    CHECK_MESSAGE(cpp_tr.events.empty(),
                  "a cpp class that overrides nothing made a host call");
    CHECK_MESSAGE(lua_tr.logs.empty(), "Lua logged for an absent method");
    CHECK_MESSAGE(cpp_tr.logs.empty(),
                  "the cpp VM logged for a callback the class declines; Lua's "
                  "lua_isfunction check returns silently and this must too");
}

TEST_CASE("every class publishes every optional ABI slot, whatever it overrides")
{
    /* This test asserted the OPPOSITE until 2026-09-22: that a class which
     * overrides nothing leaves its slots NULL, via the overrides_on_X<>
     * compile-time detection.  That detection is not portable and the table
     * no longer uses it.
     *
     * Why: comparing &T::f against &Script::f answers "does T override f?"
     * only where a pointer-to-virtual-member encodes an ADDRESS.  MSVC
     * encodes a thunk address, so it answered correctly here and this test
     * was green.  On the Itanium ABI (GCC/MinGW — which is what builds the
     * user-project script modules) such a pointer is a vtable INDEX, and an
     * override occupies its base's index, so every comparison answered "not
     * overridden" and the emitted table was ALL NULL.  The engine then loaded
     * the module, resolved the class, constructed the instance, logged
     * "script: loaded ... (cpp)" — and never called it.  The defect read as
     * "C++ scripts run in the shipped game and do nothing in the editor",
     * and this test could not see it because it only ever ran under MSVC.
     *
     * So the contract is now: publish every thunk unconditionally, and let
     * the empty base implementation be the no-op.  The failure this test
     * exists to catch is therefore a NULL slot, not a non-NULL one. */
    CHECK(jce_cpp_class_Silent.create != nullptr);
    CHECK(jce_cpp_class_Silent.destroy != nullptr);
    CHECK_MESSAGE(jce_cpp_class_Silent.on_start != nullptr,
                  "Silent overrides no on_start, but the slot must still be "
                  "published — a NULL here is the all-NULL-table defect");
    CHECK(jce_cpp_class_Silent.on_update != nullptr);
    CHECK(jce_cpp_class_Silent.on_destroy != nullptr);
    CHECK(jce_cpp_class_Silent.on_collision != nullptr);
    CHECK(jce_cpp_class_Silent.on_message != nullptr);
    CHECK(jce_cpp_class_Silent.on_anim_event != nullptr);

    CHECK(jce_cpp_class_Probe.on_update != nullptr);
    CHECK(jce_cpp_class_Probe.on_anim_event != nullptr);
    CHECK(jce_cpp_class_Boom.on_collision != nullptr);
    CHECK(jce_cpp_class_Boom.on_message != nullptr);
    CHECK(jce_cpp_class_Boom.on_anim_event != nullptr);

    /* Vacuity guard.  "Every slot is non-NULL" is satisfied just as well by a
     * macro that wired ONE stub into all seven, which would dispatch every
     * callback to the wrong body.  The seven differ in signature and in the
     * member they call, so no COMDAT folding can collapse them: distinct
     * addresses prove the slots are seven distinct thunks.
     *
     * Deliberately compared WITHIN one class.  Across classes the thunks for
     * a hook nobody overrides can be genuinely identical code and the linker
     * is free to fold them, which would make that form of the check flaky. */
    const void *slots[] = {
        (const void *)jce_cpp_class_Silent.on_start,
        (const void *)jce_cpp_class_Silent.on_update,
        (const void *)jce_cpp_class_Silent.on_destroy,
        (const void *)jce_cpp_class_Silent.on_collision,
        (const void *)jce_cpp_class_Silent.on_message,
        (const void *)jce_cpp_class_Silent.on_anim_event,
        (const void *)jce_cpp_class_Silent.on_fixed_update,
    };
    const int n = (int)(sizeof slots / sizeof slots[0]);
    for (int a = 0; a < n; ++a)
        for (int b = a + 1; b < n; ++b)
            CHECK_MESSAGE(slots[a] != slots[b],
                          "two of Silent's seven callback slots hold the same "
                          "address — the table wired one stub into several "
                          "hooks, so a callback reaches the wrong body");

    /* And the behavioural half, which is what the NULL slots used to buy:
     * a class that overrides nothing must still do nothing observable.
     * That is asserted by the preceding test case ("a cpp class that
     * overrides nothing made a host call" / "logged for an absent method"),
     * which now covers it for published-but-empty thunks instead of for
     * absent ones. */
}

/* ================================================================== *
 *  EXCEPTIONS DO NOT CROSS THE C ABI
 * ================================================================== */

TEST_CASE("an erroring callback is logged on both sides, both sides then stop "
          "calling it, and neither side stops calling anything else")
{
    REQUIRE(ensure_registered());

    Trace lua_tr;
    Trace cpp_tr;

    /* `Boom` / `kLuaBoom` make a host call on every update and every
     * collision, and each throws on ITS OWN second call.  Driving four
     * updates, a message and then three collisions asks five questions with
     * one sequence:
     *
     *   - did the work BEFORE a throw survive it?  (updates 1 and 2 must both
     *     appear; an exception that ate the frame loses update 2)
     *   - did the throw DISABLE on_update?  (updates 3 and 4 must appear
     *     nowhere; this is THE FAILING-CALLBACK RULE, jce_script.h)
     *   - did it disable ONLY on_update?  (collisions 1 and 2 must still
     *     arrive AFTER on_update went quiet; a VM that dropped the instance,
     *     or that keeps one flag for the whole instance, fails here)
     *   - is the bit the rule sets the one the hook that threw owns?  (the
     *     third collision must be silent; a backend that reported
     *     "on_collision error" while setting on_update's bit passes every
     *     question above and fails this one)
     *   - and does a dispatcher OUTSIDE the rule stay outside it?  (the
     *     message sent while on_update is disabled must arrive; a backend that
     *     gated messages on any hook's bit fails only here)
     *
     * on_start is asked the same question by its own case below, because it
     * needs a SECOND dispatch that this sequence does not make.
     */
    {
        JceScriptHost h = make_host(&lua_tr);
        JceScript    *s = jce_script_create_sized(&h, sizeof h);
        REQUIRE(s != nullptr);
        JceScriptInstance i =
            jce_script_instantiate_source(s, "@boom", kLuaBoom, kOwner);
        REQUIRE(i != 0);
        for (int k = 0; k < 4; ++k) jce_script_call_update(s, i, 0.25f);
        jce_script_call_message(s, i, "poke", 1.0, nullptr);
        for (int k = 0; k < 3; ++k) jce_script_call_collision(s, i, 7u);
        jce_script_release(s, i);
        jce_script_destroy(s);
    }
    {
        JceScriptHost h = make_host(&cpp_tr);
        JceScript    *s = jce_script_vm_create("cpp", &h, sizeof h);
        REQUIRE(s != nullptr);
        JceScriptInstance i = jce_script_instantiate(s, "Boom", kOwner);
        REQUIRE(i != 0);
        /* If the thunk did not catch, these loops do not return. */
        for (int k = 0; k < 4; ++k) jce_script_call_update(s, i, 0.25f);
        jce_script_call_message(s, i, "poke", 1.0, nullptr);
        for (int k = 0; k < 3; ++k) jce_script_call_collision(s, i, 7u);
        jce_script_release(s, i);
        jce_script_destroy(s);
    }

    /* The recorder prefixes every event with `ui_set_text|<entity>|`; the
     * expectation is BUILT from kOwner rather than spelled, so a change to the
     * owner is not a failure in this list. */
    const std::string pfx = "ui_set_text|" + std::to_string(kOwner) + "|";
    const std::vector<std::string> expected = {
        pfx + "boom|update|1", pfx + "boom|update|2", pfx + "boom|message|poke",
        pfx + "boom|collision|1|7", pfx + "boom|collision|2|7",
    };
    REQUIRE_MESSAGE(lua_tr.events == expected,
                    "the Lua reference did not produce updates 1-2, the "
                    "message and collisions 1-2 and nothing else; the "
                    "comparison has no operand. Got "
                        << lua_tr.events.size() << " event(s)");
    REQUIRE_MESSAGE(cpp_tr.events == expected,
                    "the cpp side did not produce updates 1-2, the message and "
                    "collisions 1-2 and nothing else. More events mean a "
                    "disable did not take; a missing collision or message "
                    "means the instance was dropped instead of the callback, "
                    "or one hook's throw disabled another; zero would mean the "
                    "exception escaped. Got "
                        << cpp_tr.events.size() << " event(s)");
    CHECK_MESSAGE(lua_tr.events == cpp_tr.events,
                  "the host calls made BEFORE the throw and AFTER the disable "
                  "must be identical on both sides");

    /* FOUR LINES ON EACH SIDE: for each of the two throws, the error and then
     * the notice.  A backend that logged the error and kept dispatching — the
     * behaviour the reference itself had before this rule was implemented —
     * produces two, and goes on producing more every frame. */
    REQUIRE_MESSAGE(lua_tr.logs.size() == 4u,
                    "Lua logged " << lua_tr.logs.size()
                                  << " line(s) for two errors; expected an "
                                     "error and a disable notice for each");
    REQUIRE_MESSAGE(cpp_tr.logs.size() == 4u,
                    "the cpp VM logged " << cpp_tr.logs.size()
                                         << " line(s) for two throws; expected "
                                            "an error and a disable notice for "
                                            "each");

    /* PINNED DIVERGENCE: the error line's PREFIX is identical and its TEXT is
     * not.  Lua's error carries "chunk:line:", a C++ what() does not, and
     * neither is the other's defect.  Comparing the prefix is the comparison
     * that means something; comparing the whole string would be a test of
     * Lua's error formatting. */
    const std::string prefix = "on_update error: ";
    CHECK_MESSAGE(lua_tr.logs[0].rfind(prefix, 0) == 0,
                  "lua log was: " << lua_tr.logs[0]);
    CHECK_MESSAGE(cpp_tr.logs[0].rfind(prefix, 0) == 0,
                  "cpp log was: " << cpp_tr.logs[0]);
    CHECK_MESSAGE(cpp_tr.logs[0] == prefix + "boom",
                  "the cpp message must carry what() verbatim after the "
                  "prefix; got: "
                      << cpp_tr.logs[0]);
    CHECK_MESSAGE(lua_tr.logs[0] != cpp_tr.logs[0],
                  "PINNED: the two error TEXTS are expected to differ. If they "
                  "ever match, this pin is stale and the comparison above "
                  "should be tightened to the whole string.");

    /* The notice lines are the opposite: they carry no language-specific
     * detail, so they are compared WHOLE and against the wording jce_script.h
     * publishes.  That is what makes them the lines a cross-language
     * differential can use.  Each names ITS OWN hook, which is how a backend
     * that disabled the wrong one is caught in the log as well as in the
     * events. */
    char update_notice[512];
    char collision_notice[512];
    std::snprintf(update_notice, sizeof update_notice,
                  JCE_SCRIPT_DISABLED_NOTICE_FMT, "on_update");
    std::snprintf(collision_notice, sizeof collision_notice,
                  JCE_SCRIPT_DISABLED_NOTICE_FMT, "on_collision");

    CHECK_MESSAGE(lua_tr.logs[1] == std::string(update_notice),
                  "the Lua reference did not announce the on_update disable in "
                  "the words its own header publishes; got: "
                      << lua_tr.logs[1]);
    CHECK_MESSAGE(cpp_tr.logs[1] == std::string(update_notice),
                  "the cpp VM did not announce the on_update disable in the "
                  "words jce_script.h publishes; got: " << cpp_tr.logs[1]);

    CHECK(lua_tr.logs[2].rfind("on_collision error: ", 0) == 0);
    CHECK(cpp_tr.logs[2] == std::string("on_collision error: crunch"));
    CHECK_MESSAGE(lua_tr.logs[3] == std::string(collision_notice),
                  "got: " << lua_tr.logs[3]);
    CHECK_MESSAGE(cpp_tr.logs[3] == std::string(collision_notice),
                  "got: " << cpp_tr.logs[3]);
}

TEST_CASE("on_start participates in the rule, on both sides")
{
    REQUIRE(ensure_registered());

    Trace lua_tr;
    Trace cpp_tr;

    {
        JceScriptHost h = make_host(&lua_tr);
        JceScript    *s = jce_script_create_sized(&h, sizeof h);
        REQUIRE(s != nullptr);
        JceScriptInstance i =
            jce_script_instantiate_source(s, "@sb", kLuaStartBoom, kOwner);
        REQUIRE(i != 0);
        for (int k = 0; k < 5; ++k) jce_script_call_start(s, i);
        jce_script_release(s, i);
        jce_script_destroy(s);
    }
    {
        JceScriptHost h = make_host(&cpp_tr);
        JceScript    *s = jce_script_vm_create("cpp", &h, sizeof h);
        REQUIRE(s != nullptr);
        JceScriptInstance i = jce_script_instantiate(s, "StartBoom", kOwner);
        REQUIRE(i != 0);
        for (int k = 0; k < 5; ++k) jce_script_call_start(s, i);
        jce_script_release(s, i);
        jce_script_destroy(s);
    }

    const std::string pfx = "ui_set_text|" + std::to_string(kOwner) + "|";
    const std::vector<std::string> expected = {
        pfx + "startboom|1", pfx + "startboom|2",
    };
    char notice[512];
    std::snprintf(notice, sizeof notice, JCE_SCRIPT_DISABLED_NOTICE_FMT,
                  "on_start");

    REQUIRE_MESSAGE(lua_tr.events == expected,
                    "the Lua reference did not dispatch on_start exactly twice "
                    "before the rule silenced it; got "
                        << lua_tr.events.size() << " event(s)");
    REQUIRE_MESSAGE(cpp_tr.events == expected,
                    "the cpp VM did not dispatch on_start exactly twice before "
                    "the rule silenced it. Five events mean cpp_call_start "
                    "passes CB_NONE where it should pass CB_START; got "
                        << cpp_tr.events.size() << " event(s)");
    REQUIRE(lua_tr.logs.size() == 2u);
    REQUIRE(cpp_tr.logs.size() == 2u);
    CHECK(lua_tr.logs[0].rfind("on_start error: ", 0) == 0);
    CHECK(cpp_tr.logs[0] == std::string("on_start error: start boom"));
    CHECK(lua_tr.logs[1] == std::string(notice));
    CHECK(cpp_tr.logs[1] == std::string(notice));
}

TEST_CASE("PINNED: a rebind cannot re-enable a disabled callback for compiled "
          "code, and re-instantiating does")
{
    REQUIRE(ensure_registered());

    /* jce_script.h says a rebind clears THE FAILING-CALLBACK RULE's disables,
     * and the Lua reference does exactly that.  This backend cannot: a C++
     * class is not recompiled in-process, `compile_module` refuses and returns
     * 0, and `rebind_instance` is a documented no-op.  Re-enabling on a rebind
     * here would mean re-enabling a hook whose implementation is byte for byte
     * the one that just failed.
     *
     * So the divergence is STATED as the expected result, and the escape hatch
     * is asserted next to it: a NEW instance of the same class starts clean.
     * An unpinned known divergence is an immune mutation with no reason. */
    Trace         tr;
    JceScriptHost h = make_host(&tr);
    JceScript    *s = jce_script_vm_create("cpp", &h, sizeof h);
    REQUIRE(s != nullptr);

    JceScriptInstance i = jce_script_instantiate(s, "Boom", kOwner);
    REQUIRE(i != 0);
    for (int k = 0; k < 3; ++k) jce_script_call_update(s, i, 0.25f);
    REQUIRE(tr.events.size() == 2u);          /* disabled after the throw */

    jce_script_rebind_instance(s, i, 0);
    jce_script_call_update(s, i, 0.25f);
    CHECK_MESSAGE(tr.events.size() == 2u,
                  "PINNED: a rebind re-enabled a disabled callback in the cpp "
                  "VM. It must not — rebind is a no-op here, so the code "
                  "behind the hook is unchanged and the next frame would fail "
                  "and log all over again.");

    /* The supported re-enable: a fresh instance. */
    tr.events.clear();
    JceScriptInstance j = jce_script_instantiate(s, "Boom", kOwner);
    REQUIRE(j != 0);
    jce_script_call_update(s, j, 0.25f);
    CHECK_MESSAGE(tr.events.size() == 1u,
                  "a NEW instance inherited the old one's disables. The mask "
                  "is per instance, and the slot it lands in is recycled.");

    jce_script_release(s, i);
    jce_script_release(s, j);
    jce_script_destroy(s);
}

TEST_CASE("a throwing global handler is caught at the module boundary")
{
    REQUIRE(ensure_registered());
    Trace         tr;
    JceScriptHost h = make_host(&tr);
    JceScript *s    = jce_script_vm_create("cpp", &h, sizeof h);
    REQUIRE(s != nullptr);

    /* A global has no instance, so its thunk is the ONLY frame between the
     * throw and the engine's C forwarder.  Returning true is the contract:
     * "a global of that name existed and was invoked", regardless of whether
     * the call itself errored — which is what jce_script.h says for Lua. */
    const bool invoked = jce_script_call_named(s, "g_throw", 3u);
    CHECK_MESSAGE(invoked,
                  "a handler that threw was still INVOKED; false would mean "
                  "'no such handler', which is a different fact");
    REQUIRE_MESSAGE(tr.logs.size() == 1u,
                    "the escaped global exception was not reported");
    CHECK(tr.logs[0] == std::string("g_throw error: global boom"));

    jce_script_destroy(s);
}

/* ================================================================== *
 *  THE SHORT-HOST CLAMP  (the header names this test as the enforcement)
 * ================================================================== */

TEST_CASE("a short host is not read past its end, and the clamp reaches the "
          "module")
{
    REQUIRE(ensure_registered());

    const std::size_t full  = sizeof(JceScriptHost);
    const std::size_t short_ = offsetof(JceScriptHost, loc_set_locale);

    /* VACUITY CHECK FIRST.  With the FULL size the withheld member IS called,
     * so "not called" below is evidence about the clamp and not about
     * set_locale being unreachable for some other reason. */
    {
        Trace tr;
        g_poison_locale_called = false;
        JceScriptHost h        = make_host(&tr);
        JceScript    *s        = jce_script_vm_create("cpp", &h, full);
        REQUIRE(s != nullptr);
        JceScriptInstance i = jce_script_instantiate(s, "ClampProbe", kOwner);
        REQUIRE(i != 0);
        jce_script_call_start(s, i);
        CHECK_MESSAGE(g_poison_locale_called,
                      "with the full host_size, loc_set_locale must reach the "
                      "host — otherwise the truncated case below withholds "
                      "nothing and proves nothing");
        REQUIRE(tr.events.size() == 1u);
        CHECK(tr.events[0] ==
              "ui_set_text|42|clamp|host_size|" + std::to_string(full));
        jce_script_destroy(s);
    }

    /* THE TRUNCATION.  The bytes for loc_set_locale are present in `h` and
     * are a real function pointer; only the SIZE says they are not ours. */
    {
        Trace tr;
        g_poison_locale_called = false;
        JceScriptHost h        = make_host(&tr);
        JceScript    *s        = jce_script_vm_create("cpp", &h, short_);
        REQUIRE(s != nullptr);
        JceScriptInstance i = jce_script_instantiate(s, "ClampProbe", kOwner);
        REQUIRE(i != 0);
        jce_script_call_start(s, i);
        CHECK_MESSAGE(!g_poison_locale_called,
                      "the VM called a host member the caller's host_size "
                      "said it did not have — this is the over-read the "
                      "min(caller, engine) copy exists to prevent");
        REQUIRE(tr.events.size() == 1u);
        CHECK_MESSAGE(tr.events[0] == "ui_set_text|42|clamp|host_size|" +
                                          std::to_string(short_),
                      "the module was handed a host_size that is not the "
                      "clamped one; re-widening it one layer down undoes the "
                      "clamp entirely");
        jce_script_destroy(s);
    }
}

/* ================================================================== *
 *  PINNED DIVERGENCES — stated with the value on BOTH sides
 * ================================================================== */

TEST_CASE("PINNED: instantiate_source is Lua's entry point and not the cpp VM's")
{
    REQUIRE(ensure_registered());
    Trace         tr;
    JceScriptHost h = make_host(&tr);

    JceScript *lua = jce_script_create_sized(&h, sizeof h);
    REQUIRE(lua != nullptr);
    CHECK_MESSAGE(jce_script_instantiate_source(lua, "@x", kLuaSilent,
                                                kOwner) != 0,
                  "the reference accepts source; if it ever stops, the "
                  "divergence below is no longer a divergence");
    jce_script_destroy(lua);

    JceScript *cpp = jce_script_vm_create("cpp", &h, sizeof h);
    REQUIRE(cpp != nullptr);
    CHECK_MESSAGE(
        jce_script_instantiate_source(cpp, "Probe", "class X {};", kOwner) == 0,
        "a C++ script is compiled: there is no source to run at runtime, and "
        "answering non-zero would mean silently ignoring the source the "
        "caller passed. Use jce_script_instantiate(s, \"<class>\", owner).");
    /* The refusal is not silent, but it goes to LOG_ERROR rather than
     * host->log: an unsupported ENTRY POINT is a fact about the engine's
     * configuration, not about the script that is running, and routing it to
     * the game's own log sink would put it in the player's console. */
    CHECK_MESSAGE(tr.logs.empty(),
                  "an unsupported entry point reported through host->log; "
                  "that sink belongs to script-authored errors");
    jce_script_destroy(cpp);
}

TEST_CASE("PINNED: compile_module / rebind / release_module are no-ops for "
          "compiled code")
{
    REQUIRE(ensure_registered());
    Trace         tr;
    JceScriptHost h = make_host(&tr);

    JceScript *lua = jce_script_create_sized(&h, sizeof h);
    REQUIRE(lua != nullptr);
    const JceScriptModule lua_mod =
        jce_script_compile_module(lua, "@x", kLuaSilent, std::strlen(kLuaSilent));
    CHECK_MESSAGE(lua_mod != 0,
                  "the reference compiles a module; the divergence below is "
                  "only a divergence while this holds");
    jce_script_release_module(lua, lua_mod);
    jce_script_destroy(lua);

    JceScript *cpp = jce_script_vm_create("cpp", &h, sizeof h);
    REQUIRE(cpp != nullptr);
    CHECK_MESSAGE(jce_script_compile_module(cpp, "Probe", "", 0) == 0,
                  "hot-reloading a native class is not a recompile: `self` is "
                  "a C++ object of a type that would no longer exist. The "
                  "supported path is jce_script_vm_cpp_unload + _load_library. "
                  "0 is Lua's own 'compile failed' value, so a caller that "
                  "already handles that path handles this one.");
    /* The two no-ops must tolerate the 0 they will be handed. */
    JceScriptInstance i = jce_script_instantiate(cpp, "Probe", kOwner);
    REQUIRE(i != 0);
    jce_script_rebind_instance(cpp, i, 0);
    jce_script_release_module(cpp, 0);
    jce_script_call_update(cpp, i, 0.25f);
    CHECK_MESSAGE(tr.events.size() == 1u,
                  "a no-op rebind must leave the instance dispatching");
    jce_script_release(cpp, i);
    jce_script_destroy(cpp);
}

TEST_CASE("PINNED: cpp globals are process-wide where Lua globals are per-VM")
{
    REQUIRE(ensure_registered());
    Trace         tr;
    JceScriptHost h = make_host(&tr);

    /* A SECOND cpp VM, with no script instantiated in it at all, still
     * resolves the module's globals — native code has one copy of a function
     * per process.  A second Lua VM would not: its globals live in its own
     * lua_State. */
    JceScript *a = jce_script_vm_create("cpp", &h, sizeof h);
    JceScript *b = jce_script_vm_create("cpp", &h, sizeof h);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(jce_script_call_named(b, "g_click", 11u));
    CHECK(tr.events.size() == 1u);

    JceScript    *lua = jce_script_create_sized(&h, sizeof h);
    Trace         lua_tr;
    JceScriptHost lh  = make_host(&lua_tr);
    JceScript    *lua2 = jce_script_create_sized(&lh, sizeof lh);
    REQUIRE(lua != nullptr);
    REQUIRE(lua2 != nullptr);
    REQUIRE(jce_script_instantiate_source(lua, "@probe", kLuaProbe, kOwner) != 0);
    CHECK_MESSAGE(!jce_script_call_named(lua2, "g_click", 11u),
                  "a second lua_State must NOT see the first's globals; if it "
                  "does, this pin describes nothing");

    jce_script_destroy(a);
    jce_script_destroy(b);
    jce_script_destroy(lua);
    jce_script_destroy(lua2);
}
