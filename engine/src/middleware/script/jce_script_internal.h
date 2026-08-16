/* jce_script_internal.h — the script layer's private seam.
 *
 * ENGINE-PRIVATE.  Not under engine/include/, not JCE_API, so
 * check_abi_snapshot.py correctly does not see it and it is not part of the
 * C ABI.  Its consumers are exactly two translation units:
 *
 *   jce_script.c                 — the seven permanently hand-written
 *                                  bindings + lifecycle
 *   jce_script_bindings.gen.c    — the other 71, generated
 *
 * and one test, tests/middleware/script/test_jce_script_internal_header.c,
 * which is the only thing that notices if this stops being self-contained:
 * that test's TU includes this header FIRST and nothing else engine-side, so
 * a missing include here is a compile error there and nowhere else.
 *
 * The generated TU cannot compile without struct JceScript (it dereferences
 * s->have_host and s->host.<member>), the upvalue accessor, and the
 * registration helper.  jce_script_json_null_token is here for a different
 * reason: jce.json_null must be the SAME object l_jce_asset_read_json pushes
 * as its null sentinel, and two file-static tokens in two TUs are two
 * addresses that compare unequal.  Two tests pin that from opposite ends and
 * neither alone is sufficient:
 *
 *   test_jce_script_internal_header.c — the value in the `jce` table IS
 *       &jce_script_json_null_token, the object this header declares.
 *   test_jce_script_asset_json.c      — the value the JSON reader pushes for
 *       a null compares equal to jce.json_null from inside a script.
 *
 * Whichever side drifts, one of them goes red; see the mutation log in the
 * commit that introduced this file.
 */
#ifndef JCE_SCRIPT_INTERNAL_H
#define JCE_SCRIPT_INTERNAL_H

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>

#include <lua.h>
#include <lauxlib.h>

#include <stdbool.h>
#include <stddef.h>

/* Coroutine scheduler slot: a Lua thread (luaL_ref'd into the registry)
 * parked on a jce.wait_seconds() timer.  thread_ref == LUA_NOREF marks a
 * free slot. */
#define JCE_SCRIPT_MAX_COROUTINES 256

typedef struct {
    int   thread_ref;   /* luaL_ref to the coroutine thread, or LUA_NOREF */
    float remaining;    /* seconds left on the current wait */
} JceScriptCoro;

struct JceScript {
    /* MUST BE FIRST.  Every public jce_script_* entry point is a forwarder in
     * jce_script_vm.c that reads the vtable out of the first bytes of the
     * handle, so a member placed above this one would be dispatched through
     * as a function pointer.  This is the same rule every out-of-tree backend
     * obeys; see JceScriptVMHeader in
     * engine/include/jce/middleware/script/jce_script_vm.h.
     * *Enforced by:* jce_script_vm_create() refuses a handle whose first word
     * is not the table it dispatched through — tests/middleware/script/
     * test_jce_script_vm_abi.c :: test_handle_without_the_header_is_refused
     * builds exactly that handle and asserts the refusal. */
    JceScriptVMHeader vm_header;

    lua_State    *L;
    JceScriptHost host;       /* copied; callbacks may be NULL */
    bool          have_host;
    int           instance_count;

    /* Coroutine timer scheduler (jce.start_coroutine / jce.wait_seconds). */
    JceScriptCoro coros[JCE_SCRIPT_MAX_COROUTINES];
    int           coro_count;  /* high-water bound for iteration */
};

/* Each jce.* binding is a C closure carrying the JceScript* as upvalue 1. */
static inline JceScript *jce_script_self_from_upvalue(lua_State *L)
{
    return (JceScript *)lua_touserdata(L, lua_upvalueindex(1));
}

/* Register one closure carrying `s` as upvalue into the table on top. */
void jce_script_register_binding(lua_State *L, JceScript *s,
                                 const char *name, lua_CFunction fn);

/* The one null sentinel.  Defined in jce_script.c; its address is what
 * jce.json_null holds and what asset_read_json pushes for a JSON null. */
extern char jce_script_json_null_token;

#endif /* JCE_SCRIPT_INTERNAL_H */
