# `jce_script` — the JCE scripting surface, for Python

71 entry points, generated from `contracts/script-api.json`, bound with
**ctypes** over the plain C shared library in `scripting/c_abi/`.

No compiler. No extension module. No dependency. `pip install` and it works.

## Using it

```python
import jce_script

lib = jce_script.load_library()          # finds jce_script_api.dll / .so / .dylib
api = jce_script.attach(handle, lib)     # a JceScriptApi* the engine created

api.set_position(entity, 1.0, 2.0, 3.0)

pos = api.get_position(entity)           # (x, y, z), or None if it has no transform
if pos is not None:
    x, y, z = pos

for e in api.find_by_prefix("enemy_"):   # a list, never nil
    api.apply_impulse(e, 0.0, 5.0, 0.0)

first, count = api.find_by_name("boss")  # first-or-None AND the match count
hit = api.raycast([0, 0, 0], [0, 0, 1], 100.0)
if hit != 0:                             # a MISS is integer 0, not None
    entity, px, py, pz, nx, ny, nz, dist = hit
```

`load_library()` searches, in order: an explicit `path=`, `$JCE_SCRIPT_API`,
beside this package, the working directory, then the loader's own search path.
It also performs the version handshake and raises `VersionMismatch` naming
**both** numbers if the library is older than the manifest this package was
generated from.

### Where the handle comes from

`jce_script_api` contains **no engine code at all** — it is marshalling glue
over the `JceScriptHost` callback table the runtime supplies. So a bare
`python.exe` that imports this package can call nothing useful: there is no
host behind the handle. This is the *in-process* binding transport (owner
decision 1), and the handle is created by the process that has the engine in
it.

`open_host(host_ptr, host_size)` exists for the one caller that already **has**
a `JceScriptHost`: a C extension, an embedder, or a test fixture. It is not a
way to build a host *from* Python — that struct's field order is an engine
private and `script-api.json` deliberately publishes no layout for it.

## What you get in an editor

`_generated.pyi` ships beside the module, with `py.typed`. Every entry point
carries its argument names, its argument types and its **return shape**, so
`api.` completes with all 71 and `get_position` is known to return
`tuple[float, float, float] | None` rather than `Any`. The stubs are generated
from the same manifest as the module: writing them by hand would be a second
copy of a machine-readable contract.

## The return-shape rules, all seven

| manifest shape | Python returns |
| --- | --- |
| `void_call` | `None` |
| `value_return` | the value — `bool` / `int` / `float` / `str` |
| `fallible_out` | a tuple of the out values, or `None` on failure (or the manifest's `miss_value` — `jce.raycast` answers integer `0`) |
| `void_out_array` | a tuple of N floats |
| `first_and_count` | `(first_or_None, count)` |
| `entity_table` | `list[int]`, empty rather than `None` |
| `owned_string_release` | `str`, or `None` |

**`None` is Python's spelling of Lua's `nil`, and it is used for nothing else.**
A `const char *` with no host decodes to `""` because that is what the Lua
binding pushes; `absent_value` and `clamp_min` are applied by the C ABI
forwarder and are not re-applied here. The reasoning — and why a per-language
absent-value policy would have weakened the only oracle Python has — is in
`tools/scriptgen/emit_python.py`'s docstring.

Optional arguments accept `None` **in place** as well as being omitted, exactly
as Lua's `luaL_opt*` treats an explicit `nil`. That is the only spelling
available where a required argument follows an optional one
(`gas_apply(e, attr, None, magnitude)`).

## What is deliberately not here

The manifest's seven `hand_written` entries — `log`, `asset_read_text`,
`asset_read_json`, `play_sound`, `start_coroutine`, `wait_seconds`,
`stop_coroutine` — are excluded **as a class**, by the same rule that keeps
them out of the C ABI: three are Lua-VM machinery, and the asset readers carry
sandbox policy that lives in `static` functions inside `jce_script.c`.
Re-implementing that here would be a second copy of a security decision.

`jce.json_null` — the Lua table's 79th key — is also absent, and that is this
binding's own decision. It is a sentinel with **no producer** on this side: the
only thing that ever yields it is `asset_read_json`, and
`owned_string_release` hands back raw JSON *text*, as Lua does. A sentinel no
reader can return is a contract nothing enforces.

Every one of those eight names is in `jce_script.NOT_EXPOSED`, with its reason,
and the differential reads the **live Lua table** to check that the two sets
still account for each other.

## How it is accepted

`tests/scripting/python/differential.py` runs a **cross-language differential**.
The same manifest-derived case is driven through the Lua VM and through this
binding, over one shared recording mock host, in four host modes, and the two
recorded streams are compared as text — result slots with their types, and
every host call with its arguments, in order.

Lua is the reference implementation: batch 1 proved the generated Lua bindings
equivalent to the hand-written originals and then deleted the originals. That
oracle is spent. This is the one that remains.

`tests/scripting/python/test_emit_python.py` covers the four things a
differential between two *running* surfaces cannot see: that `_generated.pyi`
still matches `_generated.py` name for name and signature for signature (both
are emitted, so byte-identity against a fresh emit cannot catch them drifting
apart); the optional-argument ordering rule; this backend's own manifest
condition, which cannot fail against a clean tree; and that the WHEEL still
builds no C — no compiled target under `scripting/python` may name a file
inside `jce_script/`, and exactly one compiled target exists here (the VM
shim, below). It needs no library and no build, so it runs even where the
differential is skipped.

## Regenerating

```
python tools/scriptgen/gen_script_bindings.py --write
```

`_generated.py`, `_generated.pyi` and the differential's mock host are
committed and are checked byte-identical to a fresh emit by the default
(check) mode of the same tool.


---

# The other direction: the engine calling **up**

Everything above is the call-**down** half: a script reaching the engine.
`jce_script.vm` plus `src/jce_script_vm_python.c` are the mirror — a
`JceScriptVM`, so the engine can drive a `.py` attached to an entity through
`on_start` / `on_update` / collisions / messages exactly as it drives Lua.

## Writing a script

A gameplay script is a module whose top-level functions take `self`.  That is
Lua's shape, member for member: `jce_script.c` builds an instance as
`{ entity = owner }` with `metatable.__index = <the module>`, so writes land on
the instance and reads fall through to the module.  `Instance.__getattr__` is
that `__index`.

```python
# scripts/bob.py

def on_start(self):
    self.hp = 100                          # per-INSTANCE, like Lua's inst.hp
    jce.log("bob %d awake" % self.entity)
    jce.set_position(self.entity, 0.0, 1.0, 0.0)

def on_update(self, dt):
    pos = jce.get_position(self.entity)    # (x, y, z), or None
    if pos is not None:
        x, y, z = pos
        jce.set_position(self.entity, x, y + dt, z)

def on_collision(self, other):
    self.hp -= 10
    if self.hp <= 0:
        jce.destroy_entity(self.entity)

def on_destroy(self):
    jce.log("bob %d gone" % self.entity)

def on_volume_changed(entity, value):      # a UISlider handler: a GLOBAL
    jce.audio_set_volume(value)            # NOT a method, and no `self`
```

`self.entity` is the owning entity id.  `jce` is injected into the module's
namespace before the body runs; it is the same 71-entry surface documented
above — the *same object*, opened with `jce_script.open_host()` over the
engine's own `JceScriptHost` — plus the four entries that are VM machinery
rather than host glue: `log`, `start_coroutine`, `wait_seconds`,
`stop_coroutine`.

Coroutines are generators:

```python
def blink(self):
    while True:
        jce.particle_set_emitting(self.entity, True)
        yield jce.wait_seconds(0.5)        # or just: yield 0.5
        jce.particle_set_emitting(self.entity, False)
        yield 0.5

def on_start(self):
    self.co = jce.start_coroutine(blink, self)
```

`jce.wait_seconds(n)` returns `n` for the caller to `yield`, because a plain
function call cannot suspend a Python frame the way `lua_yield` can.  As in
Lua, `start_coroutine` runs the body up to its first `yield` before returning.

## Attaching one

```c
#include <jce_script_vm_python.h>

jce_script_vm_python_add_path("scripting/python");   /* or PYTHONPATH */
if (!jce_script_vm_python_register())
    /* logged with the reason; the engine keeps running on Lua */;

JceScript        *s = jce_script_vm_create("python", &host, sizeof host);
JceScriptInstance i = jce_script_instantiate(s, "scripts/bob.py", entity);

jce_script_call_start(s, i);
/* every frame */
jce_script_call_update(s, i, dt);
jce_script_update_coroutines(s, dt);
```

From `jce_script_vm_create` onward **nothing is Python-specific**: every call
is the public `jce_script_*` entry point the engine already makes for Lua,
forwarded through the vtable.  That is what lets one driver run the lifecycle
differential against both languages.

Link `jce_script_vm_python` to get it.  Nothing links it by default and there
is no central list of backends — registration is a call.

**Not yet wired to the `Script` component.** `jce_rt_script.c:1293` creates the
runtime's VM with `jce_script_create(&host)`, which is `"lua"` by definition,
and `jce_runtime.c:560` instantiates every `Script` component's `script_path`
through it.  Selecting a backend per file extension is one edit in those
engine-owned files, shared by all three new backends, and is deliberately not
made here: three parallel authors editing one dispatch site is the highest-risk
change in this batch.  Until it lands, a game attaches Python scripts by
creating the VM itself, as above.

## What is deliberately not supported

| | why |
|---|---|
| `jce.asset_read_text`, `jce.asset_read_json`, `jce.play_sound` | `hand_written` in the manifest because they are **policy** (a virtual-path validator, a 1 MiB cap, a bounded JSON decoder, arity dispatch across two host members), not glue. The C ABI does not export them either, so this is a gap in the shared surface, not in this backend. They raise `NotImplementedError` carrying the manifest's own reason. |
| A watchdog | Lua re-arms a `LUA_MASKCOUNT` hook before every dispatch, so a runaway handler `luaL_error`s out instead of hanging the frame. CPython's equivalent is `sys.settrace`, which costs roughly an order of magnitude per call on **every** script. A runaway Python handler hangs the frame; that is the stated cost of this backend, not an oversight. |
| A sandbox | Lua opens a reduced library set (no `io`, no `os`). This backend does **not** sandbox: `import os` works, and a Python script is as privileged as native code. Ship only scripts you would ship as a DLL. |
| Sub-interpreters | Isolation between handles is per-`ScriptVM` (its own namespaces, instances and scheduler), not per-interpreter. CPython documents `PyGILState_*` as incompatible with sub-interpreters, and those APIs are how every slot acquires the GIL. *Enforced by:* `test_two_live_vms_do_not_share_script_state`. |

## One documented divergence from Lua

`jce_script_call_named*` finds a handler in the most recently instantiated
script that has one.  Lua looks in `_G`, which every chunk shares and nothing
clears, so a Lua handler outlives the instance that defined it; here it does
not.  The engine-visible case is identical — a UISlider handler lives in a
script attached to a live entity — and the divergence is pinned by
`test_a_named_handler_does_not_outlive_its_instance` so that changing it is
deliberate.

## The GIL, the interpreter, and exceptions

* **The GIL.** Every slot's Python work goes through one door (`py_call`),
  wrapped in `PyGILState_Ensure` / `PyGILState_Release`.  A slot that forgot
  either half would crash or hang, and a crash reports less than a red test —
  so `py_call` refuses to proceed when `PyGILState_Check()` says the GIL is not
  held and counts the refusal.  `jce_script_vm_python_gil_stats()` exports
  enters, leaves, `body_unheld` and `still_held`; *enforced by:*
  `test_every_slot_balances_the_gil`, `test_no_slot_body_ran_without_the_gil`.
* **The interpreter** is started once, on the first `create`, and **never
  finalised**.  A bare interpreter does survive two `Py_Initialize` /
  `Py_FinalizeEx` cycles here (measured), but that does not extend to a game
  whose scripts import an extension module, and a script VM is created and
  destroyed on every scene load.  Per-handle state IS released;
  *enforced by:* `test_a_second_vm_after_the_first_was_destroyed_still_runs`.
* **Exceptions** are caught, logged, and then *that handler is disabled on
  that instance* — THE FAILING-CALLBACK RULE, stated in `jce_script.h` and
  implemented by the Lua reference: the frame is not lost, `"<method> error:
  <detail>"` goes to `host.log` **and** the engine log, the published disable
  notice follows it, and the handler is not dispatched again until the
  instance is rebound (hot reload) or replaced.  Without the last clause a
  handler that raises every frame writes sixty log lines a second about one
  defect.  `BaseException`, not `Exception` — a script calling `sys.exit()`
  raises `SystemExit`, and unwinding that through the frame the engine is
  dispatching from is a process exit mid-frame.  *Enforced by:*
  `test_a_raising_handler_is_logged_then_disabled_and_rebind_revives_it` and,
  cross-language, by the lifecycle differential.

## Acceptance

`tests/scripting/python/lifecycle_differential.py` renders **one** case book
into a Lua chunk and a Python module, drives both through the engine's own
`jce_script_*` entry points with one recording mock host, and compares the
streams line for line.  Every field is logged as `name=type:value`, so
`nil`/`None`, `0` and `false` are three different lines.  The slot list is
parsed out of `jce_script_vm.h` and a slot exercised by no step fails the run.
Both sides must have produced script output *before* anything is compared —
nothing compares equal to nothing.
