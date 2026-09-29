cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L4 script-layer unit tests.                                            #
#                                                                         #
#  The gameplay scripting VM (jce_script) is a generic Lua host: every    #
#  engine reach goes through the JceScriptHost callback table.  These      #
#  tests install a MOCK host and drive the bindings headlessly (no        #
#  runtime, no physics/audio/scene, no bgfx) — purely the C<->Lua glue.   #
#                                                                         #
#  jce_script links lua::lua PRIVATELY, so the test only needs the public #
#  jce_script API surface — link jce_core + jce_script (mirrors the link  #
#  set of the runtime script-load test, minus the full-engine aggregate   #
#  it needs only because it drives jce_runtime_create end-to-end).        #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_script_bindings
    SOURCES test_jce_script_bindings.c
    LINK    jce_core jce_script)

jce_add_unit_test(test_jce_script_asset_text
    SOURCES test_jce_script_asset_text.c
    LINK    jce_core jce_script)

jce_add_unit_test(test_jce_script_asset_json
    SOURCES test_jce_script_asset_json.c
    LINK    jce_core jce_script)

jce_add_unit_test(test_jce_script_touch
    SOURCES test_jce_script_touch.c
    LINK    jce_core jce_script)

jce_add_unit_test(test_jce_script_message
    SOURCES test_jce_script_message.c
    LINK    jce_core jce_script)

jce_add_unit_test(test_jce_script_netparticle
    SOURCES test_jce_script_netparticle.c
    LINK    jce_core jce_script)

# Cooperative coroutine timers — jce.start_coroutine / wait_seconds /
# stop_coroutine + jce_script_update_coroutines.  Pure Lua-thread scheduler on a
# headless VM (no runtime/scene), asserts wait/resume/chain/instant/cancel/misuse.
jce_add_unit_test(test_jce_script_coroutine
    SOURCES test_jce_script_coroutine.c
    LINK    jce_core jce_script)

# VM watchdog: an infinite loop in a dispatched handler is aborted by a re-armed
# LUA_MASKCOUNT hook instead of hard-hanging the single-threaded runtime.
jce_add_unit_test(test_jce_script_watchdog
    SOURCES test_jce_script_watchdog.c
    LINK    jce_core jce_script)

# THE FAILING-CALLBACK RULE (jce_script.h): a fixed-name lifecycle callback
# that raises is caught, logged, and disabled ON THAT INSTANCE until a rebind
# or a respawn.  The header promised this for a long time and call_method()
# only logged, so a script erroring every frame wrote sixty lines a second.
# This is the enforcement, on the reference VM the other three backends are
# written against — including the two dispatchers that deliberately do NOT
# participate (call_message, the named globals), which are pinned here so a
# later "make it uniform" has to argue with a named test.
jce_add_unit_test(test_jce_script_disable
    SOURCES test_jce_script_disable.c
    LINK    jce_core jce_script)

# Script-driven animation API (P0): jce.anim_set_float/int/bool/trigger Lua
# bindings reach the host, and the scene anim-command relay round-trips/drains.
jce_add_unit_test(test_jce_script_anim
    SOURCES test_jce_script_anim.c
    LINK    JCE)

# Data-driven action/axis input bridge — jce.is_action_down / is_action_pressed
# / get_axis.  Mock-host recorder asserts the C<->Lua marshalling + bool/number
# return round-trip + NULL-host safe defaults; the runtime backs them with the
# live JceInputActions map (jce_action_find + down/pressed/value).
jce_add_unit_test(test_jce_script_action
    SOURCES test_jce_script_action.c
    LINK    jce_core jce_script)

# A script error has to say who CALLED it.  Every dispatch passed 0 as
# lua_pcall's message-handler index, so luaL_traceback never ran; the fixture
# raises three frames deep and asserts the OUTER frame name, because a handler
# that ran after the stack unwound would still print the "stack traceback"
# header over an empty trace.
jce_add_unit_test(test_jce_script_stack_trace
    SOURCES test_jce_script_stack_trace.c
    LINK    jce_core jce_script)

# Global event bus binding — jce.broadcast(msg [, number] [, string]).  Mock-host
# recorder asserts the C<->Lua marshalling (full arity / defaults / NULL-host
# no-op); the runtime fan-out is a thin loop over the already-tested call_message.
jce_add_unit_test(test_jce_script_broadcast
    SOURCES test_jce_script_broadcast.c
    LINK    jce_core jce_script)

# Lightweight runtime reflection — jce.has_component / is_component_enabled /
# set_component_enabled.  Mock-host recorder asserts the C<->Lua marshalling +
# bool return round-trip + NULL-host safe defaults; the runtime backs them with
# the already-covered dense component registry.
jce_add_unit_test(test_jce_script_component
    SOURCES test_jce_script_component.c
    LINK    jce_core jce_script)

# Scripted RPC send binding — jce.rpc_send(entity, event [, target] [, payload]).
# Mock-host recorder asserts marshalling (full arity / defaults / bool return /
# NULL-host); the runtime packs onto a generic channel + dispatches event as a
# method on receive (thin over already-tested jce_rpc_send + call_message).
jce_add_unit_test(test_jce_script_rpc
    SOURCES test_jce_script_rpc.c
    LINK    jce_core jce_script)

# Lua sandbox: dofile/loadfile removed (both open HOST paths, around the VFS
# and the PAK), and load() constrained to text+string chunks (unvalidated
# bytecode is a full escape).  Probes run INSIDE a script — the only vantage
# point that proves anything about a sandbox.
jce_add_unit_test(test_jce_script_sandbox
    SOURCES test_jce_script_sandbox.c
    LINK    jce_core jce_script)

# JceScriptHost short-struct ABI: the host is CALLER-allocated and gains
# members over releases, so copying it at the
# engine's sizeof over-reads a host built against an older SDK — and files
# the bytes that follow under a FUNCTION POINTER the binding then calls.
# Probes hand the engine a truncated, 0xFF-poisoned host; without
# jce_script_create_sized these crash rather than fail.
jce_add_unit_test(test_jce_script_host_abi
    SOURCES test_jce_script_host_abi.c
    LINK    jce_core jce_script)

# The `jce` table's KEY SET, read out of a live VM with pairs().  Every other
# gate on this surface scans source; a `register_binding(` regex structurally
# cannot see jce.json_null, which is a lua_pushlightuserdata + lua_setfield
# pair.  The expected 79 keys are hand-authored in the test, deliberately NOT
# derived from the manifest — a generated expectation only proves the
# generator agrees with itself.
jce_add_unit_test(test_jce_script_table_shape
    SOURCES test_jce_script_table_shape.c
    LINK    jce_core jce_script)

# jce_script_internal.h is the seam the generated binding TU compiles against.
# It is engine-private, so no ABI gate watches it; INCLUDES reaches it the same
# way tests/os/platform reaches jce_input_sdl.h.  lua::lua is linked here and
# nowhere else in this suite: jce_script keeps Lua PRIVATE, but a TU that
# includes the internal header sees lua_State and calls into the VM directly to
# read the `jce` table's json_null out of it -- which is the whole point of the
# header being a seam.
jce_add_unit_test(test_jce_script_internal_header
    SOURCES test_jce_script_internal_header.c
    LINK    jce_core jce_script lua::lua
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/middleware/script")

# JceScriptVM short-struct ABI, mirrored from test_jce_script_host_abi with the
# direction reversed: the PLUGIN allocates the vtable and the ENGINE reads it,
# so an over-read yields a FUNCTION POINTER THE ENGINE CALLS.  Probes register
# a table truncated at its current last member with 0xFF after it and assert
# the registration is refused naming that slot; also covers the two slots whose
# absence is invisible (call_named_num / call_named_str drive every UI
# handler and return false for "no such global"), the handle-header rule, and
# the owning-thread rule.
jce_add_unit_test(test_jce_script_vm_abi
    SOURCES test_jce_script_vm_abi.c
    LINK    jce_core jce_script)

# The extension registry: which VM a script PATH belongs to.  Backends claim
# their own extensions from inside their own register(), so a sixth backend
# needs no engine edit -- which is only true if the claim path is a public
# call with public refusals.  Pins those: a claim for an unregistered
# language, a second claim on a taken extension, and a claim that could never
# match (interior dot / separator / over-long) are all refused, and a
# registered language that claimed nothing stays unreachable by path.
jce_add_unit_test(test_jce_script_vm_ext
    SOURCES test_jce_script_vm_ext.c
    LINK    jce_core jce_script)

# on_fixed_update: the callback that makes a jump clear the same gap at 30 Hz
# and at 144 Hz.  Scripts had nothing on the fixed clock, so a force applied
# from on_update was frame-rate dependent -- the thing Unity's FixedUpdate,
# Unreal's substepped tick and Godot's _physics_process all exist to prevent.
# The load-bearing case is that it has its OWN disable bit: sharing on_update's
# would let a handler that throws every physics step take the render-frame
# callback down with it, and the report would name the wrong function.
jce_add_unit_test(test_jce_script_fixed_update
    SOURCES test_jce_script_fixed_update.c
    LINK    jce_core jce_script)
