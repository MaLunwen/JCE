/*
 * jce_script.c  Gameplay scripting VM — Lua 5.4 host implementation.
 *
 * Generic Lua host (Phase 0 keystone). Knows nothing of the scene/ECS: all
 * engine access goes through the JceScriptHost callback bridge the runtime
 * installs at jce_script_create(). See api_script.h for the scripting model.
 *
 * Layering: depends only on jce_core + Lua. The `jce.*` Lua bindings retrieve
 * the owning JceScript* via a C-closure upvalue and dispatch to host->*.
 */

#include <jce/middleware/script/jce_script.h>

/* struct JceScript, the upvalue accessor, jce_script_register_binding and the
 * shared json_null token -- the seam the generated binding TU compiles
 * against.  Engine-private; see the header for who else may include it. */
#include "jce_script_internal.h"

/* The 71 generated bindings + json_null.  There is one binding set now: the
 * 71 hand-written originals were deleted with the differential harness that
 * proved them equivalent, and this file keeps only the seven that are
 * hand-written on purpose (script_exposure.json's hand_written[]). */
#include "jce_script_bindings.gen.h"

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <math.h>
#include <string.h>

#define LOG_TAG "script"

#define JCE_SCRIPT_JSON_MAX_DEPTH 32u
#define JCE_SCRIPT_JSON_MAX_NODES 16384u

/* The one null sentinel, declared extern in jce_script_internal.h so the
 * generated binding TU pushes the SAME address.  Two tokens compare unequal
 * and every script testing `v == jce.json_null` silently stops matching.
 * test_jce_script_internal_header pins the value in the `jce` table to this
 * object; test_jce_script_asset_json pins the JSON reader's null to the
 * table's value.  Neither alone is sufficient -- see the header. */
char jce_script_json_null_token;

/* Re-arms the per-dispatch instruction-budget watchdog (defined below); used by
 * the coroutine + chunk paths above its definition. */
static void script_arm_watchdog(lua_State *L);

/* ── jce.* bindings ─────────────────────────────────────────────────────── */

static int l_jce_log(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *msg = luaL_optstring(L, 1, "");
    if (s->have_host && s->host.log)
        s->host.log(s->host.user, msg);
    else
        LOG_INFO(LOG_TAG, "[lua] %s", msg);
    return 0;
}

static bool script_virtual_asset_path_valid(const char *path)
{
    const char *segment;
    const char *cursor;

    if (!path || !path[0] || path[0] == '/' || path[0] == '\\')
        return false;
    if (path[1] == ':')
        return false;

    segment = path;
    for (cursor = path; ; ++cursor) {
        size_t segment_len;

        if (*cursor == '\\' || *cursor == ':')
            return false;
        if (*cursor != '/' && *cursor != '\0')
            continue;
        segment_len = (size_t)(cursor - segment);
        if (segment_len == 0u ||
            (segment_len == 1u && segment[0] == '.') ||
            (segment_len == 2u && segment[0] == '.' && segment[1] == '.'))
            return false;
        if (*cursor == '\0')
            return true;
        segment = cursor + 1;
    }
}

static int l_jce_asset_read_text(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *path = luaL_checkstring(L, 1);
    uint64_t size = JCE_SCRIPT_TEXT_ASSET_MAX_BYTES;
    void *bytes;

    if (!script_virtual_asset_path_valid(path) || !s->have_host ||
        !s->host.read_file) {
        lua_pushnil(L);
        return 1;
    }
    bytes = s->host.read_file(s->host.user, path, &size);
    if (!bytes || size == 0u || size > JCE_SCRIPT_TEXT_ASSET_MAX_BYTES) {
        jce_free(bytes);
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)bytes, (size_t)size);
    jce_free(bytes);
    return 1;
}

static int script_json_fail(lua_State *L, const char *code)
{
    lua_pushnil(L);
    lua_pushstring(L, code);
    return 2;
}

static bool script_json_push_value(lua_State *L, const JceJson *node,
                                   uint32_t depth, uint32_t *node_count,
                                   const char **out_error)
{
    if (!node) {
        *out_error = "invalid_json";
        return false;
    }
    if (depth > JCE_SCRIPT_JSON_MAX_DEPTH) {
        *out_error = "depth_limit";
        return false;
    }
    if (++(*node_count) > JCE_SCRIPT_JSON_MAX_NODES) {
        *out_error = "node_limit";
        return false;
    }

    if (jce_json_is_object(node)) {
        lua_newtable(L);
        for (JceJson *it = jce_json_first_child(node); it;
             it = jce_json_next_sibling(it)) {
            const char *key = jce_json_member_key(it);

            if (!key) {
                lua_pop(L, 1);
                *out_error = "invalid_json";
                return false;
            }
            lua_getfield(L, -1, key);
            if (!lua_isnil(L, -1)) {
                lua_pop(L, 2);
                *out_error = "duplicate_key";
                return false;
            }
            lua_pop(L, 1);
            if (!script_json_push_value(L, it, depth + 1u, node_count,
                                        out_error)) {
                lua_pop(L, 1);
                return false;
            }
            lua_setfield(L, -2, key);
        }
        return true;
    }
    if (jce_json_is_array(node)) {
        int count = jce_json_array_size(node);

        lua_createtable(L, count, 0);
        for (int i = 0; i < count; ++i) {
            if (!script_json_push_value(L, jce_json_array_at(node, i),
                                        depth + 1u, node_count, out_error)) {
                lua_pop(L, 1);
                return false;
            }
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        return true;
    }
    if (jce_json_is_string(node)) {
        lua_pushstring(L, jce_json_string_value(node, ""));
        return true;
    }
    if (jce_json_is_number(node)) {
        double value = jce_json_number_value(node, 0.0);

        if (!isfinite(value)) {
            *out_error = "invalid_number";
            return false;
        }
        lua_pushnumber(L, (lua_Number)value);
        return true;
    }
    if (jce_json_is_bool(node)) {
        lua_pushboolean(L, jce_json_bool_value(node, false));
        return true;
    }
    if (jce_json_is_null(node)) {
        lua_pushlightuserdata(L, &jce_script_json_null_token);
        return true;
    }
    *out_error = "invalid_json";
    return false;
}

static int l_jce_asset_read_json(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *path = luaL_checkstring(L, 1);
    uint64_t size = JCE_SCRIPT_TEXT_ASSET_MAX_BYTES;
    const char *error = NULL;
    uint32_t node_count = 0;
    JceJson *root;
    void *bytes;

    if (!script_virtual_asset_path_valid(path))
        return script_json_fail(L, "invalid_path");
    if (!s->have_host || !s->host.read_file)
        return script_json_fail(L, "not_found");

    bytes = s->host.read_file(s->host.user, path, &size);
    if (size > JCE_SCRIPT_TEXT_ASSET_MAX_BYTES) {
        jce_free(bytes);
        return script_json_fail(L, "too_large");
    }
    if (!bytes || size == 0u) {
        jce_free(bytes);
        return script_json_fail(L, "not_found");
    }
    root = jce_json_parse_strict((const char *)bytes, (size_t)size);
    jce_free(bytes);
    if (!root)
        return script_json_fail(L, "invalid_json");
    if (!jce_json_is_object(root) && !jce_json_is_array(root)) {
        jce_json_free(root);
        return script_json_fail(L, "invalid_root");
    }
    if (!script_json_push_value(L, root, 1u, &node_count, &error)) {
        jce_json_free(root);
        return script_json_fail(L, error ? error : "invalid_json");
    }
    jce_json_free(root);
    lua_pushnil(L);
    return 2;
}

/* jce.play_sound(path [, x,y,z] [, volume [, min,max,rolloff]])
 *   path required.  Optional 3 coords make it positional (3D); optional final
 *   numbers configure volume and 3D attenuation.  Arities supported:
 *     play_sound(path)                  -> 2D, vol 1.0
 *     play_sound(path, vol)             -> 2D, vol
 *     play_sound(path, x,y,z)           -> 3D, vol 1.0
 *     play_sound(path, x,y,z, vol)      -> 3D, default attenuation
 *     play_sound(path, x,y,z, vol,
 *                min,max,rolloff)       -> 3D, authored attenuation */
static int l_jce_play_sound(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    const char *path = luaL_checkstring(L, 1);
    int top = lua_gettop(L);

    const float *pos_ptr = NULL;
    float pos[3];
    float volume = 1.0f;

    if (top >= 4) {
        /* coords present (args 2,3,4); optional volume at 5. */
        pos[0] = (float)luaL_checknumber(L, 2);
        pos[1] = (float)luaL_checknumber(L, 3);
        pos[2] = (float)luaL_checknumber(L, 4);
        pos_ptr = pos;
        volume = (float)luaL_optnumber(L, 5, 1.0);
    } else if (top == 2) {
        /* single trailing number => 2D volume. */
        volume = (float)luaL_optnumber(L, 2, 1.0);
    }
    /* top == 1 (or 3, ambiguous) => 2D default volume. */

    if (s->have_host && pos_ptr && top >= 8 && s->host.play_sound_spatial) {
        float min_distance = (float)luaL_checknumber(L, 6);
        float max_distance = (float)luaL_checknumber(L, 7);
        float rolloff = (float)luaL_checknumber(L, 8);
        s->host.play_sound_spatial(s->host.user, path, pos_ptr, volume,
                                   min_distance, max_distance, rolloff);
    } else if (s->have_host && s->host.play_sound) {
        s->host.play_sound(s->host.user, path, pos_ptr, volume);
    }
    return 0;
}

/* ── Coroutine timer scheduler (jce.start_coroutine / wait_seconds) ────────
 *
 * Unity-style cooperative coroutines: jce.start_coroutine(fn) runs `fn` as a
 * Lua thread immediately up to its first jce.wait_seconds(t) (so synchronous
 * setup before the first yield happens at once), then the runtime resumes it t
 * seconds later via jce_script_update_coroutines().  Pure Lua-thread state — no
 * host callback needed.  Returns an integer handle for jce.stop_coroutine. */

/* Find a free scheduler slot, growing the high-water bound; -1 when full. */
static int coro_alloc_slot(JceScript *s)
{
    for (int i = 0; i < JCE_SCRIPT_MAX_COROUTINES; ++i) {
        if (s->coros[i].thread_ref == LUA_NOREF) {
            if (i >= s->coro_count) s->coro_count = i + 1;
            return i;
        }
    }
    return -1;
}

static int l_jce_start_coroutine(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);

    /* New thread on the MAIN state so the registry ref keeps it alive even when
     * start_coroutine is itself called from inside another coroutine. */
    lua_State *co = lua_newthread(s->L);        /* [main: .., co] */
    lua_pushvalue(L, 1);                        /* [L: .., fn] (copy of arg) */
    lua_xmove(L, co, 1);                        /* move fn onto co: [co: fn] */
    int ref = luaL_ref(s->L, LUA_REGISTRYINDEX);/* pops co (top of main), refs it */

    int nres = 0;
    script_arm_watchdog(co);                     /* hook on the coroutine thread */
    int st = lua_resume(co, L, 0, &nres);       /* run up to first yield/return */
    if (st == LUA_YIELD) {
        float wait = (nres >= 1) ? (float)lua_tonumber(co, -1) : 0.0f;
        lua_pop(co, nres);                      /* clear yielded results */
        int slot = coro_alloc_slot(s);
        if (slot < 0) {                         /* scheduler full → drop it */
            luaL_unref(s->L, LUA_REGISTRYINDEX, ref);
            lua_pushinteger(L, 0);
            return 1;
        }
        s->coros[slot].thread_ref = ref;
        s->coros[slot].remaining  = wait < 0.0f ? 0.0f : wait;
        lua_pushinteger(L, ref);                /* handle for stop_coroutine */
        return 1;
    }
    if (st != LUA_OK) {                          /* errored before first yield */
        const char *err = lua_tostring(co, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "coroutine error: %s", err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "coroutine error: %s", err ? err : "?");
    }
    /* Completed (or errored) without ever waiting — nothing to schedule. */
    luaL_unref(s->L, LUA_REGISTRYINDEX, ref);
    lua_pushinteger(L, 0);
    return 1;
}

static int l_jce_wait_seconds(lua_State *L)
{
    double t = luaL_optnumber(L, 1, 0.0);
    if (!lua_isyieldable(L))
        return luaL_error(L, "jce.wait_seconds must be called inside jce.start_coroutine");
    lua_pushnumber(L, t);
    return lua_yield(L, 1);                      /* yields t to the resumer */
}

static int l_jce_stop_coroutine(lua_State *L)
{
    JceScript *s = jce_script_self_from_upvalue(L);
    int handle = (int)luaL_optinteger(L, 1, 0);
    if (handle != 0 && handle != LUA_NOREF) {
        for (int i = 0; i < s->coro_count; ++i) {
            if (s->coros[i].thread_ref == handle) {
                luaL_unref(s->L, LUA_REGISTRYINDEX, handle);
                s->coros[i].thread_ref = LUA_NOREF;
                break;
            }
        }
    }
    return 0;
}

/* Register one closure carrying `s` as upvalue into the table on top.
 * Extern (declared in jce_script_internal.h) because the generated binding TU
 * registers through this same helper. */
void jce_script_register_binding(lua_State *L, JceScript *s,
                                 const char *name, lua_CFunction fn)
{
    lua_pushlightuserdata(L, s);
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, -2, name);
}

/* The call sites below keep the short spelling.  Concrete reason, not taste:
 * _REGISTER_RE in tools/scriptgen/gen_script_bindings.py matches the call
 * shape verbatim -- in BOTH translation units, since it is a substring of the
 * generated TU's jce_script_register_binding( -- and a rename here would
 * blind condition 5 on the seven names this file still owns.  The macro is
 * undefined right after the installer so it cannot leak into anything the
 * generated TU shares.
 *
 * That regex scans the whole file, comments included -- spelling the call
 * shape out in prose here made the gate report an extra binding named after
 * the placeholder, which is why this paragraph describes it instead. */
#define register_binding(L, s, n, f) jce_script_register_binding((L), (s), (n), (f))

/* The one `jce` table: 71 generated functions + json_null out of
 * jce_script_bindings.gen.c, then the seven that stay hand-written.
 *
 * The 71 hand-written originals were deleted in the commit that retired the
 * differential harness, so nothing here compares two implementations any
 * more.  What keeps this table honest instead:
 *   - gen_script_bindings.py condition 5, registration parity, which reads
 *     BOTH this file and jce_script_bindings.gen.c and fails in either
 *     direction (registered-but-unnamed, named-but-unregistered);
 *   - tests/middleware/script/test_jce_script_table_shape.c, the 79 keys
 *     hand-authored and read out of a live VM with pairs(), which is the only
 *     check that can see jce.json_null at all;
 *   - test_jce_script_internal_header.c, which counts those keys again from C
 *     through a TU that includes only the private header. */
static void install_bindings(JceScript *s)
{
    lua_State *L = s->L;
    lua_newtable(L);                         /* the `jce` table */
    jce_script_install_generated_bindings(s);   /* 71 functions + json_null */
    /* The seven that are hand-written permanently.  Their reasons live in
     * script_exposure.json's hand_written[]; asset_read_text and
     * asset_read_json are hand-written FOREVER, because a generated
     * read_file template emits no path validator, no size cap and no bounded
     * JSON walker -- a generated sandbox escape.  Condition 5 of the gate
     * fails if anyone marks them otherwise. */
    register_binding(L, s, "log",             l_jce_log);
    register_binding(L, s, "asset_read_text", l_jce_asset_read_text);
    register_binding(L, s, "asset_read_json", l_jce_asset_read_json);
    register_binding(L, s, "play_sound",      l_jce_play_sound);
    register_binding(L, s, "start_coroutine", l_jce_start_coroutine);
    register_binding(L, s, "wait_seconds",    l_jce_wait_seconds);
    register_binding(L, s, "stop_coroutine",  l_jce_stop_coroutine);
    lua_setglobal(L, "jce");
}

#undef register_binding

/* ── Sandboxed standard libs ────────────────────────────────────────────── */

/* Replacement for the base library's `load`, with two restrictions the stock
 * one does not have:
 *
 *   1. TEXT CHUNKS ONLY.  Stock load() defaults to mode "bt" and will happily
 *      compile a *binary* chunk.  The Lua VM does not validate bytecode, so a
 *      crafted binary chunk is a well-known way to read and write arbitrary
 *      process memory — i.e. a full sandbox escape, from a plain string.
 *      The caller's `mode` argument is accepted and IGNORED; it is always "t".
 *   2. STRING CHUNKS ONLY.  The reader-function form is refused, so a script
 *      cannot assemble a chunk from sources this function cannot inspect.
 *
 * Everything else is preserved, including the custom-environment argument
 * that first-party scripts rely on (space_director.lua:335 passes one). */
static int l_sandbox_load(lua_State *L)
{
    size_t      len = 0;
    const char *src;

    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushnil(L);
        lua_pushstring(L, "load: only string chunks are allowed in the JCE "
                          "sandbox (reader functions are refused)");
        return 2;
    }
    src = lua_tolstring(L, 1, &len);

    /* Reject a binary chunk explicitly rather than letting luaL_loadbufferx
     * produce a vaguer "attempt to load a binary chunk" — the caller should
     * see WHY this is policy, not a mode mismatch. */
    if (len > 0 && src[0] == LUA_SIGNATURE[0]) {
        lua_pushnil(L);
        lua_pushstring(L, "load: binary chunks are refused (unvalidated "
                          "bytecode is a sandbox escape)");
        return 2;
    }

    {
        const char *name = luaL_optstring(L, 2, "=(load)");
        /* arg 3 (mode) deliberately ignored — always text. */
        const int rc = luaL_loadbufferx(L, src, len, name, "t");
        if (rc != LUA_OK) {
            lua_pushnil(L);
            lua_insert(L, -2);          /* nil, errmsg */
            return 2;
        }
    }

    if (!lua_isnoneornil(L, 4)) {       /* custom _ENV upvalue */
        lua_pushvalue(L, 4);
        if (lua_setupvalue(L, -2, 1) == NULL)
            lua_pop(L, 1);              /* chunk has no _ENV; drop the value */
    }
    return 1;
}

/* Open base/table/string/math only — NOT io/os/package/debug, so scripts
 * can't touch processes or load native code.
 *
 * That list alone is NOT a filesystem sandbox, which is what this comment
 * used to claim: luaopen_base installs `dofile` and `loadfile`, and both open
 * a HOST path directly — bypassing the VFS, the PAK, and every mount policy
 * around them.  A script shipped inside a signed PAK could read anything the
 * process could.  Neither has a single first-party user, so both are removed
 * outright rather than redirected.
 *
 * `load` IS used (space_director.lua), so it stays — hardened, see above. */
static void open_sandboxed_libs(lua_State *L)
{
    static const luaL_Reg libs[] = {
        { LUA_GNAME,      luaopen_base },
        { LUA_TABLIBNAME, luaopen_table },
        { LUA_STRLIBNAME, luaopen_string },
        { LUA_MATHLIBNAME, luaopen_math },
        { NULL, NULL },
    };
    for (const luaL_Reg *lib = libs; lib->func; lib++) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }

    /* Host-filesystem escapes from luaopen_base. */
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");

    /* Text-only, string-only load. */
    lua_pushcfunction(L, l_sandbox_load);
    lua_setglobal(L, "load");
}

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

static JceScript *script_lua_create_sized(const JceScriptHost *host, size_t host_size)
{
    JceScript *s = (JceScript *)jce_malloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));

    /* The handle's first bytes are the vtable the public forwarders dispatch
     * through.  jce_script_vm_create() refuses a handle where this is not the
     * table it called, so forgetting it is a clean failure, not a call
     * through whatever the struct happens to start with. */
    s->vm_header.vm = jce_script_vm_lua();

    s->L = luaL_newstate();
    if (!s->L) {
        jce_free(s);
        LOG_ERROR(LOG_TAG, "luaL_newstate failed (OOM)");
        return NULL;
    }
    if (host && host_size > 0) {
        /* `s->host = *host` copied at THIS engine's sizeof.  JceScriptHost is
         * a caller-allocated table of function pointers that grows as bindings
         * are added, so a consumer built against an older header hands us a
         * SHORTER object — and the struct copy read past its end, then called
         * whatever bytes happened to follow.  A garbage function pointer
         * invoked as a binding is not a crash you can debug from the stack it
         * produces.
         *
         * Copy min(caller, engine) over a zeroed destination instead: members
         * the caller never knew about stay NULL, and every call site here
         * already null-checks its callback.  A LONGER host (caller newer than
         * this engine) is equally fine — the tail is ignored.
         *
         * Same contract as jce_engine_set_app_desc_sized(); see the ABI note
         * in contracts/language-driver-abi.md. */
        const size_t n = host_size < sizeof(s->host) ? host_size
                                                     : sizeof(s->host);
        memcpy(&s->host, host, n);
        s->have_host = true;
    }

    /* Coroutine slots start free (memset 0 would read as a valid ref). */
    for (int i = 0; i < JCE_SCRIPT_MAX_COROUTINES; ++i)
        s->coros[i].thread_ref = LUA_NOREF;
    s->coro_count = 0;

    open_sandboxed_libs(s->L);
    install_bindings(s);

    LOG_SUCCESS(LOG_TAG, "Lua %s VM created%s",
                LUA_VERSION_MAJOR "." LUA_VERSION_MINOR,
                s->have_host ? " (host bridged)" : "");
    return s;
}

/* jce_script_create's legacy entry point moved to jce_script_vm.c with the
 * rest of the public surface.  It is NOT a JceScriptVM slot — see the note on
 * struct JceScriptVM for why a size-less create must not be one. */

static void script_lua_destroy(JceScript *s)
{
    if (!s) return;
    if (s->L) lua_close(s->L);
    jce_free(s);
}

/* ── Instantiation ──────────────────────────────────────────────────────── */

/* Stack on entry: [ module ] (the value the chunk returned).
 * Builds an instance table { entity=owner, <meta __index=module> }, refs it,
 * and returns the ref. Pops the module. Returns 0 on bad module. */
static JceScriptInstance build_instance(lua_State *L, JceScriptEntity owner)
{
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);                       /* drop bad return */
        return 0;
    }
    /* instance = {} */
    lua_newtable(L);                         /* [ module, inst ] */
    lua_pushinteger(L, (lua_Integer)owner);
    lua_setfield(L, -2, "entity");           /* inst.entity = owner */
    /* meta = { __index = module } */
    lua_newtable(L);                         /* [ module, inst, meta ] */
    lua_pushvalue(L, -3);                    /* push module */
    lua_setfield(L, -2, "__index");          /* meta.__index = module */
    lua_setmetatable(L, -2);                 /* setmetatable(inst, meta) -> [ module, inst ] */
    /* ref the instance, pop it, then drop the module */
    int ref = luaL_ref(L, LUA_REGISTRYINDEX); /* pops inst -> [ module ] */
    lua_pop(L, 1);                           /* drop module */
    return (JceScriptInstance)ref;
}

static JceScriptInstance run_chunk(JceScript *s, JceScriptEntity owner,
                                   const char *what)
{
    lua_State *L = s->L;
    /* chunk is on stack top (loaded by caller). Run it expecting 1 return. */
    script_arm_watchdog(L);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "script run error (%s): %s", what,
                     err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "run error (%s): %s", what, err ? err : "?");
        lua_pop(L, 1);
        return 0;
    }
    JceScriptInstance inst = build_instance(L, owner);
    if (inst == 0)
        LOG_ERROR(LOG_TAG, "script '%s' did not return a table", what);
    else
        s->instance_count++;
    return inst;
}

static JceScriptInstance script_lua_instantiate_source(JceScript *s, const char *name,
                                                const char *source,
                                                JceScriptEntity owner)
{
    if (!s || !source) return 0;
    const char *chunkname = name ? name : "=chunk";
    if (luaL_loadbuffer(s->L, source, strlen(source), chunkname) != LUA_OK) {
        const char *err = lua_tostring(s->L, -1);
        LOG_ERROR(LOG_TAG, "compile error (%s): %s", chunkname, err ? err : "?");
        lua_pop(s->L, 1);
        return 0;
    }
    return run_chunk(s, owner, chunkname);
}

static JceScriptInstance script_lua_instantiate(JceScript *s, const char *path,
                                         JceScriptEntity owner)
{
    if (!s || !path) return 0;
    if (!s->have_host || !s->host.read_file) {
        LOG_ERROR(LOG_TAG, "no read_file host callback; cannot load '%s'", path);
        return 0;
    }
    uint64_t size = 0;
    void *buf = s->host.read_file(s->host.user, path, &size);
    if (!buf || size == 0) {
        if (buf) jce_free(buf);
        LOG_ERROR(LOG_TAG, "cannot read script '%s'", path);
        return 0;
    }
    char chunkname[256];
    snprintf(chunkname, sizeof(chunkname), "@%s", path);
    int load = luaL_loadbuffer(s->L, (const char *)buf, (size_t)size, chunkname);
    jce_free(buf);
    if (load != LUA_OK) {
        const char *err = lua_tostring(s->L, -1);
        LOG_ERROR(LOG_TAG, "compile error (%s): %s", path, err ? err : "?");
        lua_pop(s->L, 1);
        return 0;
    }
    return run_chunk(s, owner, path);
}

/* ── Script watchdog: abort a runaway dispatch (infinite loop) ─────────────
 * A LUA_MASKCOUNT hook, RE-ARMED before every pcall/resume so each dispatch
 * gets a fresh instruction budget.  If one call burns past the budget it
 * luaL_error()s out of the pcall instead of hard-hanging the single-threaded
 * runtime (editor AND shipped).  Legit per-frame script work is orders of
 * magnitude under the budget, so the hook never fires in normal use. */
#define JCE_SCRIPT_WATCHDOG_INSTR 40000000
static void script_watchdog_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    luaL_error(L, "script watchdog: call exceeded %d instructions (infinite loop?)",
               JCE_SCRIPT_WATCHDOG_INSTR);
}
static void script_arm_watchdog(lua_State *L)
{
    lua_sethook(L, script_watchdog_hook, LUA_MASKCOUNT, JCE_SCRIPT_WATCHDOG_INSTR);
}

/* ── THE FAILING-CALLBACK RULE, Lua side ──────────────────────────────────
 *
 * jce_script.h states the rule; this is the reference implementation of it,
 * and the three other JceScriptVM backends are written against what this does.
 *
 * WHERE THE FLAG LIVES.  A JceScriptInstance is a luaL_ref into the registry
 * and there is no C-side per-instance struct to hang a flag on, so the mask
 * lives in the instance's OWN METATABLE — the table build_instance() creates
 * per instance and script_lua_rebind_instance() re-points __index inside.
 * Three properties fall out of that choice and all three are load-bearing:
 *
 *   - it is per instance, which is what the rule requires;
 *   - it dies with the instance, so a luaL_ref number RECYCLED by a later
 *     luaL_unref cannot inherit a dead instance's disables.  A C-side table
 *     keyed by the ref integer would have exactly that bug;
 *   - a rebind reaches the same metatable it already reaches for __index, so
 *     clearing on hot reload is one rawset in a function that already has the
 *     metatable on the stack.
 *
 * It is NOT reachable as `self.__jce_disabled` from a script: field lookup on
 * the instance falls through __index to the MODULE, never to the metatable.
 *
 * PARTICIPATION IS PASSED IN, NOT DERIVED FROM THE NAME.  call_message()
 * dispatches a method whose name the caller chose, and a game that sends a
 * message literally named "on_update" must not be able to disable the real
 * on_update.  So every dispatcher states its own slot and call_message states
 * CB_NONE. */
#define JCE_SCRIPT_DISABLED_FIELD "__jce_disabled"

typedef enum {
    /* CB_NONE is not "no slot yet"; it is a STATED non-participation, and two
     * dispatchers pass it on purpose: call_message (caller-chosen name) and
     * on_destroy (dispatched once, by a release that frees the instance on the
     * next line — there is no second call to suppress, and the notice would
     * tell the user to hot-reload an instance that no longer exists).  Both
     * reasons are written out in jce_script.h. */
    CB_NONE      = -1,
    CB_START     = 0,
    CB_UPDATE    = 1,
    CB_COLLISION = 2,
    CB_ANIM      = 3
} ScriptCallbackSlot;

/* Is `bit` marked disabled on the instance at stack index `idx`?
 * Raw access throughout: the metatable is ours, but a script that reached it
 * with getmetatable() could give it an __index, and a policy flag must not be
 * answerable by script code. Leaves the stack as it found it. */
static bool inst_cb_disabled(lua_State *L, int idx, int bit)
{
    lua_Integer mask;
    int         abs = lua_absindex(L, idx);

    if (bit < 0) return false;
    if (!lua_getmetatable(L, abs)) return false;         /* [ ..., meta ] */
    lua_pushliteral(L, JCE_SCRIPT_DISABLED_FIELD);
    lua_rawget(L, -2);                                   /* [ ..., meta, mask ] */
    mask = lua_tointeger(L, -1);                         /* nil / non-number -> 0 */
    lua_pop(L, 2);
    return (mask & ((lua_Integer)1 << bit)) != 0;
}

static void inst_cb_disable(lua_State *L, int idx, int bit)
{
    lua_Integer mask;
    int         abs = lua_absindex(L, idx);

    if (bit < 0) return;
    if (!lua_getmetatable(L, abs)) return;               /* [ ..., meta ] */
    lua_pushliteral(L, JCE_SCRIPT_DISABLED_FIELD);
    lua_rawget(L, -2);                                   /* [ ..., meta, mask ] */
    mask = lua_tointeger(L, -1);
    lua_pop(L, 1);                                       /* [ ..., meta ] */
    lua_pushliteral(L, JCE_SCRIPT_DISABLED_FIELD);
    lua_pushinteger(L, mask | ((lua_Integer)1 << bit));
    lua_rawset(L, -3);                                   /* meta[field] = mask */
    lua_pop(L, 1);                                       /* [ ... ] */
}

/* The ONE place a failed per-instance dispatch is reported, so that "which
 * sink, in which order, with which wording" is decided once for all six
 * per-instance dispatchers rather than six times.
 *
 * Stack on entry: the error object on top, the instance at `inst_idx` (which
 * is read only when `bit >= 0`).  Pops the error object; leaves the rest. */
static void report_dispatch_error(JceScript *s, lua_State *L, int inst_idx,
                                  const char *method, int bit)
{
    const char *err = lua_tostring(L, -1);
    char        buf[512];
    int         abs = lua_absindex(L, inst_idx);

    snprintf(buf, sizeof(buf), "%s error: %s", method, err ? err : "?");
    if (s->have_host && s->host.log) s->host.log(s->host.user, buf);
    LOG_ERROR(LOG_TAG, "%s", buf);
    lua_pop(L, 1);                                       /* drop error */

    if (bit < 0) return;
    inst_cb_disable(L, abs, bit);
    snprintf(buf, sizeof(buf), JCE_SCRIPT_DISABLED_NOTICE_FMT, method);
    if (s->have_host && s->host.log) s->host.log(s->host.user, buf);
    LOG_ERROR(LOG_TAG, "%s", buf);
}

/* ── Lifecycle dispatch ─────────────────────────────────────────────────── */

/* Push instance[fn]; if it's a function, push the instance as `self` and any
 * extra args pushed by the caller before this call are NOT supported here —
 * we handle the fixed arities inline below. */
static void call_method(JceScript *s, JceScriptInstance inst,
                        const char *method, bool has_dt, float dt, int bit)
{
    if (!s || inst == 0) return;
    lua_State *L = s->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, (lua_Integer)inst);  /* [ inst ] */
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    if (inst_cb_disabled(L, -1, bit)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, method);                            /* [ inst, fn ] (via meta) */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return; }
    lua_pushvalue(L, -2);                                   /* [ inst, fn, self ] */
    int nargs = 1;
    if (has_dt) { lua_pushnumber(L, (lua_Number)dt); nargs = 2; } /* [ ..., dt ] */
    script_arm_watchdog(L);
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK)                /* pops fn+args */
        report_dispatch_error(s, L, -2, method, bit);       /* inst is under it */
    lua_pop(L, 1);                                          /* drop inst */
}

static void script_lua_call_start(JceScript *s, JceScriptInstance inst)
{
    call_method(s, inst, "on_start", false, 0.0f, CB_START);
}

static void script_lua_call_update(JceScript *s, JceScriptInstance inst, float dt)
{
    call_method(s, inst, "on_update", true, dt, CB_UPDATE);
}

static void script_lua_release(JceScript *s, JceScriptInstance inst)
{
    if (!s || inst == 0) return;
    call_method(s, inst, "on_destroy", false, 0.0f, CB_NONE);
    luaL_unref(s->L, LUA_REGISTRYINDEX, (int)inst);
    if (s->instance_count > 0) s->instance_count--;
}

/* on_collision(self, other_entity) — like call_method but with a second
 * integer argument (the other body's entity id).  No-op if the script defines
 * no on_collision (so scripts opt in by simply declaring the method). */
static void script_lua_call_collision(JceScript *s, JceScriptInstance inst,
                               JceScriptEntity other_entity)
{
    if (!s || inst == 0) return;
    lua_State *L = s->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, (lua_Integer)inst);   /* [ inst ] */
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    if (inst_cb_disabled(L, -1, CB_COLLISION)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, "on_collision");                    /* [ inst, fn ] */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return; }
    lua_pushvalue(L, -2);                                   /* [ inst, fn, self ] */
    lua_pushinteger(L, (lua_Integer)other_entity);         /* [ inst, fn, self, other ] */
    script_arm_watchdog(L);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK)                   /* pops fn + args */
        report_dispatch_error(s, L, -2, "on_collision", CB_COLLISION);
    lua_pop(L, 1);                                          /* drop inst */
}

/* msg_name(self, number_arg, str_arg) — script-to-script message dispatch.
 * Mirrors jce_script_call_collision's instance-method dispatch, but with a
 * configurable method name and a (number, string|nil) payload.  No-op if the
 * receiver defines no method named msg_name (so receivers opt in by simply
 * declaring the method); str_arg NULL pushes nil. */
static void script_lua_call_message(JceScript *s, JceScriptInstance inst,
                             const char *msg_name, double number_arg,
                             const char *str_arg)
{
    if (!s || inst == 0 || !msg_name || !msg_name[0]) return;
    lua_State *L = s->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, (lua_Integer)inst);   /* [ inst ] */
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, msg_name);                          /* [ inst, fn ] (via meta) */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return; }  /* missing handler: clean no-op */
    lua_pushvalue(L, -2);                                   /* [ inst, fn, self ] */
    lua_pushnumber(L, (lua_Number)number_arg);             /* [ inst, fn, self, num ] */
    if (str_arg) lua_pushstring(L, str_arg);               /* [ ..., str ] */
    else         lua_pushnil(L);                            /* [ ..., nil ] */
    script_arm_watchdog(L);
    /* CB_NONE, and stated here rather than left to the absence of an argument:
     * `msg_name` is the CALLER's string, so deriving a slot from it would let
     * jce.send_message(e, "on_update") disable the real on_update.  See the
     * jce_script_call_message comment in jce_script.h. */
    if (lua_pcall(L, 3, 0, 0) != LUA_OK)                   /* pops fn + 3 args */
        report_dispatch_error(s, L, -2, msg_name, CB_NONE);
    lua_pop(L, 1);                                          /* drop inst */
}

/* on_anim_event(self, id, name|nil, f0, f1, i0) — animation frame-event
 * dispatch.  Mirrors jce_script_call_message's instance-method dispatch
 * exactly, but with the fixed method name "on_anim_event" and the 5-scalar
 * (id, name, f0, f1, i0) animation payload.  No-op if the receiver defines no
 * on_anim_event (so receivers opt in by simply declaring the method); a NULL /
 * empty name pushes nil for that parameter. */
static void script_lua_call_anim_event(JceScript *s, JceScriptInstance inst,
                                uint32_t id, const char *name,
                                float f0, float f1, int i0)
{
    if (!s || inst == 0) return;
    lua_State *L = s->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, (lua_Integer)inst);   /* [ inst ] */
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    if (inst_cb_disabled(L, -1, CB_ANIM)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, "on_anim_event");                   /* [ inst, fn ] (via meta) */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return; }  /* missing handler: clean no-op */
    lua_pushvalue(L, -2);                                   /* [ inst, fn, self ] */
    lua_pushinteger(L, (lua_Integer)id);                   /* [ ..., id ] */
    if (name && name[0]) lua_pushstring(L, name);          /* [ ..., name ] */
    else                 lua_pushnil(L);                    /* [ ..., nil ] */
    lua_pushnumber(L, (lua_Number)f0);                     /* [ ..., f0 ] */
    lua_pushnumber(L, (lua_Number)f1);                     /* [ ..., f1 ] */
    lua_pushinteger(L, (lua_Integer)i0);                   /* [ ..., i0 ] */
    if (lua_pcall(L, 6, 0, 0) != LUA_OK)                   /* pops fn + 6 args */
        report_dispatch_error(s, L, -2, "on_anim_event", CB_ANIM);
    lua_pop(L, 1);                                          /* drop inst */
}

static bool script_lua_call_named(JceScript *s, const char *fn_name,
                           JceScriptEntity arg_entity)
{
    if (!s || !s->L || !fn_name || !fn_name[0]) return false;
    lua_State *L = s->L;
    lua_getglobal(L, fn_name);                              /* [ fn|nil ] */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
    lua_pushinteger(L, (lua_Integer)arg_entity);           /* [ fn, arg ] */
    script_arm_watchdog(L);   /* fresh per-dispatch budget, like call_method */
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {                 /* pops fn + arg */
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s error: %s", fn_name, err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "%s error: %s", fn_name, err ? err : "?");
        lua_pop(L, 1);                                      /* drop error */
    }
    return true;   /* a function existed and was invoked (error caught above) */
}

static bool script_lua_call_named_num(JceScript *s, const char *fn_name,
                               JceScriptEntity arg_entity, double value)
{
    if (!s || !s->L || !fn_name || !fn_name[0]) return false;
    lua_State *L = s->L;
    lua_getglobal(L, fn_name);                              /* [ fn|nil ] */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
    lua_pushinteger(L, (lua_Integer)arg_entity);           /* [ fn, ent ] */
    lua_pushnumber(L, (lua_Number)value);                  /* [ fn, ent, v ] */
    script_arm_watchdog(L);   /* fresh per-dispatch budget, like call_method */
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {                 /* pops fn + 2 args */
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s error: %s", fn_name, err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "%s error: %s", fn_name, err ? err : "?");
        lua_pop(L, 1);                                      /* drop error */
    }
    return true;
}

static bool script_lua_call_named_str(JceScript *s, const char *fn_name,
                               JceScriptEntity arg_entity, const char *str)
{
    if (!s || !s->L || !fn_name || !fn_name[0]) return false;
    lua_State *L = s->L;
    lua_getglobal(L, fn_name);                              /* [ fn|nil ] */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
    lua_pushinteger(L, (lua_Integer)arg_entity);           /* [ fn, ent ] */
    if (str) lua_pushstring(L, str);                       /* [ fn, ent, s ] */
    else     lua_pushnil(L);                               /* [ fn, ent, nil ] */
    script_arm_watchdog(L);   /* fresh per-dispatch budget, like call_method */
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {                 /* pops fn + 2 args */
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s error: %s", fn_name, err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "%s error: %s", fn_name, err ? err : "?");
        lua_pop(L, 1);                                      /* drop error */
    }
    return true;
}

static int script_lua_instance_count(const JceScript *s)
{
    return s ? s->instance_count : 0;
}

static void script_lua_update_coroutines(JceScript *s, float dt)
{
    if (!s || !s->L) return;
    /* Snapshot the bound so coroutines started DURING a resume this tick (which
     * already ran to their first wait) aren't double-advanced this same tick. */
    int n = s->coro_count;
    for (int i = 0; i < n; ++i) {
        int ref = s->coros[i].thread_ref;
        if (ref == LUA_NOREF) continue;
        s->coros[i].remaining -= dt;
        if (s->coros[i].remaining > 0.0f) continue;

        lua_rawgeti(s->L, LUA_REGISTRYINDEX, ref);   /* [ thread ] */
        lua_State *co = lua_tothread(s->L, -1);
        lua_pop(s->L, 1);
        if (!co) { luaL_unref(s->L, LUA_REGISTRYINDEX, ref); s->coros[i].thread_ref = LUA_NOREF; continue; }

        int nres = 0;
        script_arm_watchdog(co);                     /* hook on the coroutine thread */
        int st = lua_resume(co, s->L, 0, &nres);     /* run to next yield/return */
        if (st == LUA_YIELD) {
            float wait = (nres >= 1) ? (float)lua_tonumber(co, -1) : 0.0f;
            lua_pop(co, nres);
            s->coros[i].remaining = wait < 0.0f ? 0.0f : wait;
        } else {
            if (st != LUA_OK) {
                const char *err = lua_tostring(co, -1);
                if (s->have_host && s->host.log) {
                    char buf[512];
                    snprintf(buf, sizeof(buf), "coroutine error: %s", err ? err : "?");
                    s->host.log(s->host.user, buf);
                }
                LOG_ERROR(LOG_TAG, "coroutine error: %s", err ? err : "?");
            }
            luaL_unref(s->L, LUA_REGISTRYINDEX, ref);
            s->coros[i].thread_ref = LUA_NOREF;       /* completed/errored */
        }
    }
}

/* ── Hot-reload ───────────────────────────────────────────────────────────── */

static JceScriptModule script_lua_compile_module(JceScript *s, const char *name,
                                          const char *source, size_t len)
{
    if (!s || !source) return 0;
    lua_State *L = s->L;
    const char *chunkname = name ? name : "=reload";
    if (luaL_loadbuffer(L, source, len, chunkname) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        if (s->have_host && s->host.log) {
            char buf[512];
            snprintf(buf, sizeof(buf), "reload compile error (%s): %s",
                     chunkname, err ? err : "?");
            s->host.log(s->host.user, buf);
        }
        LOG_ERROR(LOG_TAG, "reload compile error (%s): %s", chunkname, err ? err : "?");
        lua_pop(L, 1);
        return 0;
    }
    script_arm_watchdog(L);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        LOG_ERROR(LOG_TAG, "reload run error (%s): %s", chunkname, err ? err : "?");
        lua_pop(L, 1);
        return 0;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        LOG_ERROR(LOG_TAG, "reload: '%s' did not return a table", chunkname);
        return 0;
    }
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);   /* pops module, refs it */
    return (JceScriptModule)ref;
}

static void script_lua_rebind_instance(JceScript *s, JceScriptInstance inst,
                                JceScriptModule mod)
{
    if (!s || inst == 0 || mod == 0) return;
    lua_State *L = s->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, (lua_Integer)inst);   /* [ inst ] */
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    if (!lua_getmetatable(L, -1)) { lua_pop(L, 1); return; } /* [ inst, meta ] */
    lua_rawgeti(L, LUA_REGISTRYINDEX, (lua_Integer)mod);     /* [ inst, meta, module ] */
    lua_setfield(L, -2, "__index");                          /* meta.__index = module */
    /* And every callback THE FAILING-CALLBACK RULE disabled comes back.  A
     * rebind is the engine saying the code behind this instance may have
     * changed; without this line the script you just fixed and saved stays
     * dead until the process restarts, which is worse than the log spam the
     * rule exists to stop.  Same metatable the __index above went into. */
    lua_pushliteral(L, JCE_SCRIPT_DISABLED_FIELD);
    lua_pushinteger(L, 0);
    lua_rawset(L, -3);                                       /* meta[field] = 0 */
    lua_pop(L, 2);                                           /* [] */
}

static void script_lua_release_module(JceScript *s, JceScriptModule mod)
{
    if (!s || mod == 0) return;
    luaL_unref(s->L, LUA_REGISTRYINDEX, (int)mod);
}

/* ── The Lua JceScriptVM ──────────────────────────────────────────────────
 *
 * The first implementation of the vtable, and the one that proves the vtable
 * was copied rather than designed: every function above kept its body and its
 * signature exactly as it was when it WAS the public entry point.  Only the
 * name changed, and `static` was added.  Nothing here adapts, wraps or
 * reorders arguments — if any slot needed a shim, the slot's signature would
 * not be the public one and the whole claim would be false.
 *
 * POSITIONAL initialisers on purpose.  Designated initialisers would survive
 * a reordering of JceScriptVM; these do not.  Combined with the public
 * signature pin in jce_script_vm.c, a reorder or a retype of any slot is a
 * compile error in two places rather than a silent mis-dispatch. */
static const JceScriptVM k_lua_vm = {
    sizeof(JceScriptVM),
    "lua",
    script_lua_create_sized,
    script_lua_destroy,
    script_lua_instantiate,
    script_lua_instantiate_source,
    script_lua_call_start,
    script_lua_call_update,
    script_lua_release,
    script_lua_call_collision,
    script_lua_call_message,
    script_lua_call_anim_event,
    script_lua_call_named,
    script_lua_call_named_num,
    script_lua_call_named_str,
    script_lua_instance_count,
    script_lua_update_coroutines,
    script_lua_compile_module,
    script_lua_rebind_instance,
    script_lua_release_module,
};

const JceScriptVM *JCE_CALL jce_script_vm_lua(void)
{
    return &k_lua_vm;
}
