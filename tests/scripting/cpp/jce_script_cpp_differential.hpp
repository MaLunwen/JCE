/* jce_script_cpp_differential.hpp — support for the CROSS-LANGUAGE differential.
 *
 * HAND-WRITTEN, and that is the load-bearing property of this file.
 *
 * The differential drives the same manifest-derived case through a real Lua VM
 * and through the C++ wrapper over one recording mock host, then compares the
 * full result (type AND value per slot) and the host-call trace.  For that
 * comparison to mean anything, the projection of a C++ VALUE onto the
 * comparison form must not come from the thing under test.  So it lives here,
 * as overloads keyed on the C++ TYPE:
 *
 *     record(Slots &, float)                       -> "number:<%.9g>"
 *     record(Slots &, bool)                        -> "boolean:true"
 *     record(Slots &, const std::string &)         -> "string:<s>"
 *     record(Slots &, const std::array<float,N> &) -> N number slots
 *     ...
 *
 * There is NO per-entry projection.  A per-entry defect in the wrapper —
 * a transposed argument, a wrong member, a shape rendered as another shape —
 * therefore has nothing here to be mirrored by.  That is the difference
 * between a differential and a tautology, and this campaign has already caught
 * three assertions that were the latter, most recently one comparing two
 * identical error strings.
 *
 * THE CANONICAL FORM is Lua's, not C++'s, because Lua is the reference
 * implementation.  `probe(...)` in the prelude below renders a Lua result list
 * as
 *
 *     <n>|<canon_1>|<canon_2>|...
 *
 * and every function in this file produces exactly that spelling from a C++
 * value.  %.9g round-trips a float through a double exactly and is what both
 * sides print with, so a disagreement is behavioural and never a printf.
 */
#ifndef JCE_SCRIPT_CPP_DIFFERENTIAL_HPP
#define JCE_SCRIPT_CPP_DIFFERENTIAL_HPP

#include <jce/middleware/script/jce_script.h>
#include <jce/script_api/jce_script_api.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace jce {
namespace diff {

/* ------------------------------------------------------------------ *
 *  What the mock host answers.
 *
 *  Four modes, and every one of them exists because a manifest modifier is
 *  otherwise UNREACHABLE from both sides at once — which is batch 1's own
 *  lesson: "a mock that never answers with a negative count leaves the clamp
 *  unexercised on both sides", i.e. an immune mutation with a silent reason.
 *
 *    MOCK_OK        every member succeeds.
 *    MOCK_FAIL      every fallible member fails: bool members return false
 *                   (fallible_out -> nil / miss_value), count members return
 *                   0, and the STRING members return NULL — which is not the
 *                   same as an absent host and is the only way to reach
 *                   `absent_value`'s NULL branch.
 *    MOCK_NEGATIVE  count members return -3.  Reaches `clamp_min`.
 *    MOCK_LONG      the owned-string members answer longer than the wrapper's
 *                   inline buffer.  Reaches the second-call path.
 * ------------------------------------------------------------------ */
enum MockMode { MOCK_OK, MOCK_FAIL, MOCK_NEGATIVE, MOCK_LONG };

inline MockMode g_mode = MOCK_OK;

/* The bytes the MOCK_LONG owned-string members answer with.  Chosen larger
 * than jce::script::Api::kOwnedStringInlineBytes so the second call is forced;
 * the static_assert is what fails if either number moves under the other. */
inline constexpr std::size_t kLongJsonBytes = 1000;
static_assert(kLongJsonBytes > (std::size_t)jce::script::Api::kOwnedStringInlineBytes,
              "MOCK_LONG must exceed the wrapper's inline buffer, or the "
              "second-call path is never taken and its test is vacuous");

/* ------------------------------------------------------------------ *
 *  The host-call trace.
 *
 *  One line per call: `<member>|<arg>|<arg>`.  It is written by the mock,
 *  so both languages produce it through identical code — a difference in the
 *  trace is a difference in what the binding DID, never in how it was
 *  recorded.
 * ------------------------------------------------------------------ */
inline std::string &trace_buf()
{
    static std::string t;
    return t;
}

inline void trace_reset() { trace_buf().clear(); }
inline void trace_add(const char *member) { trace_buf() += member; }

inline void trace_num(double v)
{
    char b[64];
    std::snprintf(b, sizeof b, "|%.9g", v);
    trace_buf() += b;
}

inline void trace_str(const char *s)
{
    trace_buf() += '|';
    trace_buf() += (s != nullptr ? s : "(null)");
}

inline void trace_end() { trace_buf() += '\n'; }
inline std::string trace_text() { return trace_buf(); }

/* How many times `member` appears in a trace.  Used only by the tests that
 * pin a KNOWN divergence in call COUNT; the differential itself compares the
 * traces whole. */
inline int trace_calls(const std::string &trace, const char *member)
{
    int n = 0;
    std::string needle = std::string(member) + "|";
    std::size_t at = 0;
    while (true) {
        std::size_t hit = trace.find(needle, at);
        if (hit == std::string::npos)
            break;
        if (hit == 0 || trace[hit - 1] == '\n')
            ++n;
        at = hit + 1;
    }
    return n;
}

/* ------------------------------------------------------------------ *
 *  What the Lua side says.  The host's `log` member is the wire back to C++;
 *  it is deliberately NOT traced, or the answer would be inside the evidence.
 * ------------------------------------------------------------------ */
struct Capture {
    std::string log;
};

inline Capture &capture()
{
    static Capture c;
    return c;
}

/* Heap strings for the owned-string shape.  malloc/free, because the pointer
 * travels through the host's `json_free` member, which is this file's own
 * mock — both ends of the allocation are here. */
inline char *dup(const char *s)
{
    std::size_t n = std::strlen(s) + 1u;
    char *p = static_cast<char *>(std::malloc(n));
    if (p != nullptr)
        std::memcpy(p, s, n);
    return p;
}

inline char *dup_long_json()
{
    char *p = static_cast<char *>(std::malloc(kLongJsonBytes + 1u));
    if (p == nullptr)
        return nullptr;
    for (std::size_t i = 0; i < kLongJsonBytes; ++i)
        p[i] = static_cast<char>('a' + static_cast<int>(i % 26u));
    p[kLongJsonBytes] = '\0';
    return p;
}

inline std::string long_json_text()
{
    std::string s;
    s.reserve(kLongJsonBytes);
    for (std::size_t i = 0; i < kLongJsonBytes; ++i)
        s += static_cast<char>('a' + static_cast<int>(i % 26u));
    return s;
}

inline void release(char *s) { std::free(s); }

/* Defined by the generated half: it is the only place that knows which host
 * members the manifest's `expose` entries reach. */
void install_recording_host(JceScriptHost *h);
void install_log_only_host(JceScriptHost *h);

/* ------------------------------------------------------------------ *
 *  The comparison form.
 * ------------------------------------------------------------------ */
class Slots {
public:
    void add(const std::string &v)
    {
        if (n_ > 0)
            body_ += '|';
        body_ += v;
        ++n_;
    }

    /* `<n>|<slots joined by |>`.  With no slots this is "0|", which is exactly
     * what probe() produces from an empty result list — table.concat of an
     * empty table is the empty string. */
    std::string str() const { return std::to_string(n_) + "|" + body_; }

    int count() const { return n_; }

private:
    int         n_ = 0;
    std::string body_;
};

inline std::string num(double v)
{
    char b[64];
    std::snprintf(b, sizeof b, "%.9g", v);
    return std::string(b);
}

/* --- the type-keyed projection.  Declared before record_optional, because a
 *     template's unqualified calls are bound at DEFINITION for names that ADL
 *     cannot reach — and ADL on JceScriptRaycastHit looks in the GLOBAL
 *     namespace, not in this one. --- */
inline void record(Slots &s, bool v)
{
    s.add(std::string("boolean:") + (v ? "true" : "false"));
}

inline void record(Slots &s, float v) { s.add("number:" + num(static_cast<double>(v))); }
inline void record(Slots &s, double v) { s.add("number:" + num(v)); }
inline void record(Slots &s, int v) { s.add("number:" + num(static_cast<double>(v))); }

inline void record(Slots &s, std::uint32_t v)
{
    s.add("number:" + num(static_cast<double>(v)));
}

/* jce::script::Entity IS std::uint64_t, so this is the entity overload too. */
inline void record(Slots &s, std::uint64_t v)
{
    s.add("number:" + num(static_cast<double>(v)));
}

inline void record(Slots &s, const std::string &v) { s.add("string:" + v); }

template <std::size_t N>
inline void record(Slots &s, const std::array<float, N> &a)
{
    for (std::size_t i = 0; i < N; ++i)
        record(s, a[i]);
}

/* raycast's POD, expanded in DECLARATION ORDER — which is the order the Lua
 * binding pushes it, because that binding flattens the same struct. */
inline void record(Slots &s, const JceScriptRaycastHit &h)
{
    record(s, h.entity);
    for (int i = 0; i < 3; ++i)
        record(s, h.point[i]);
    for (int i = 0; i < 3; ++i)
        record(s, h.normal[i]);
    record(s, h.distance);
}

inline void record(Slots &s, const jce::script::FirstAndCount &r)
{
    if (r.first.has_value())
        record(s, *r.first);
    else
        s.add("nil:nil");
    record(s, r.count);
}

/* Lua renders a table as 'table:[' .. tostring(v[i]) joined by ',' .. ']'. */
inline void record(Slots &s, const std::vector<jce::script::Entity> &v)
{
    std::string t = "table:[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0)
            t += ',';
        t += std::to_string(v[i]);
    }
    t += ']';
    s.add(t);
}

/* The generated structs on this surface: get_touch's four out parameters and
 * get_param's three.
 *
 * HAND-WRITTEN HERE ON PURPOSE, naming the fields.  Field ORDER is what the
 * differential checks about it, and an emitted projection would read the
 * order out of the same list the struct was emitted from.  A further entry of
 * this shape is a COMPILE ERROR here ("no matching function for call to
 * record"), which is the correct way to find out that a new overload is
 * owed -- and it worked: get_param landed as a build break in this file
 * before it could land as a differential that silently compared nothing. */
inline void record(Slots &s, const jce::script::GetTouchResult &t)
{
    record(s, t.id);
    record(s, t.x);
    record(s, t.y);
    record(s, t.pressure);
}

inline void record(Slots &s, const jce::script::GetParamResult &p)
{
    record(s, p.out_kind);
    record(s, p.out_number);
    record(s, p.out_entity);
}

template <class T>
inline void record_optional(Slots &s, const std::optional<T> &o, const char *miss)
{
    if (o.has_value())
        record(s, *o);
    else
        s.add(miss);
}

/* ------------------------------------------------------------------ *
 *  Driving the Lua side.
 *
 *  The canonicalising prelude runs INSIDE the VM.  Results are compared as
 *  TYPE plus VALUE so nil / 0 / false are three different strings; tables are
 *  expanded because their tostring() is an address.
 *
 *  Only base / table / string / math are open in the sandbox
 *  (open_sandboxed_libs in jce_script.c), so this uses table.pack,
 *  table.concat and string.format and nothing else.
 * ------------------------------------------------------------------ */
inline const char *lua_prelude()
{
    return
        "local function canon(v)\n"
        "  local t = type(v)\n"
        "  if t == 'table' then\n"
        "    local p = {}\n"
        "    for i = 1, #v do p[i] = tostring(v[i]) end\n"
        "    return 'table:[' .. table.concat(p, ',') .. ']'\n"
        "  end\n"
        "  if t == 'number' then return 'number:' .. string.format('%.9g', v) end\n"
        "  return t .. ':' .. tostring(v)\n"
        "end\n"
        "local function probe(...)\n"
        "  local r = table.pack(...)\n"
        "  local p = {}\n"
        "  for i = 1, r.n do p[i] = canon(r[i]) end\n"
        "  return r.n .. '|' .. table.concat(p, '|')\n"
        "end\n";
}

inline bool &last_run_ok()
{
    static bool ok = false;
    return ok;
}

/* Runs `call` inside a fresh VM over `host` and returns the canonical result
 * string.  Resets the trace first, so trace_text() afterwards is this call's.
 *
 * The chunk MUST run: two sides erroring identically would satisfy every
 * comparison in the differential, which is the shape of a test that cannot
 * fail.  last_run_ok() is what the driver REQUIREs, and on a failure this
 * returns the VM's own log so the message is visible rather than swallowed. */
inline std::string run_lua_case(const JceScriptHost *host, std::size_t host_size,
                                const char *call)
{
    trace_reset();
    capture().log.clear();
    last_run_ok() = false;

    JceScript *s = jce_script_create_sized(host, host_size);
    if (s == nullptr)
        return std::string("LUA-VM-CREATE-FAILED");

    std::string src = lua_prelude();
    src += "jce.log(probe(";
    src += call;
    src += "))\nlocal M = {}\nreturn M\n";

    JceScriptInstance inst = jce_script_instantiate_source(s, "@diff",
                                                           src.c_str(), 1);
    std::string out = capture().log;
    jce_script_destroy(s);

    if (inst == 0)
        return std::string("LUA-CHUNK-FAILED: ") + out;

    /* mk_log appends a newline; probe's own string never contains one. */
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    last_run_ok() = true;
    return out;
}

}  /* namespace diff */
}  /* namespace jce */

#endif /* JCE_SCRIPT_CPP_DIFFERENTIAL_HPP */
