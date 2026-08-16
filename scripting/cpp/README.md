# `scripting/cpp` — the C++ binding, and why there is no `scripting/c`

## If you are writing C, you already have the binding

The C binding of the scripting surface is
`scripting/c_abi/include/jce/script_api/jce_script_api.h`. It is generated from
`contracts/script-api.json` by `tools/scriptgen/gen_script_c_abi.py`, it
is plain C99, it takes an opaque `JceScriptApi *`, and every manifest entry is a
real exported symbol whose presence — *and the absence of everything else* — is
asserted against the built object's export table by
`tests/scripting/c_abi/test_jce_script_api_abi.c`.

```c
#include <jce/script_api/jce_script_api.h>

JceScriptApi *api = jce_script_api_open(&host, sizeof host,
                                        JCE_SCRIPT_API_VERSION);
float xyz[3];
if (jce_script_api_get_position(api, entity, xyz)) { /* ... */ }
jce_script_api_close(api);
```

Link `jce_script_api`. That is the whole of it. There is no `scripting/c/`
directory and there should never be one: a second C layer over a C ABI has no
marshalling to do, so it would be a rename of 71 functions and a second place
for the surface to drift. The claim is checked rather than asserted —
`emit_cpp.py`'s `validate()` (condition **CPP1**) reads that header and fails by
name if any manifest entry has no `jce_script_api_<name>` declaration in it, and
fails again if one of the seven hand-written entries ever appears there.

## What C++ gets, and why each piece is here

`#include <jce/script_api/jce_script_api.hpp>` and link `jce_script_cpp`. The
wrapper is **header-only** and adds four things, none of them cosmetic:

| | |
|---|---|
| **RAII** | `jce::script::Api` owns the `JceScriptApi *`, closes it in the destructor, and is move-only. The handle is the only resource on this surface. |
| **The owned-string shape cannot leak** | The C entry is `int f(..., char *out, int out_cap)`; the C++ entry is `std::optional<std::string>`. No `char *` and no buffer size reach the caller. |
| **The seven shapes as types** | `std::optional<T>`, `std::array<float,N>`, `std::vector<Entity>` (plus a `std::span` overload under C++20), `FirstAndCount`. |
| **`index_base`, restored** | The Lua binding refuses an index below its base *without calling the host*; the C ABI passes the index straight through. The wrapper puts the refusal back at the 0-based floor. This is the only place it adds behaviour rather than sugar. |

```cpp
#include <jce/script_api/jce_script_api.hpp>

auto api = jce::script::Api::open(host);          // host_size defaults to
                                                  // the CALLER's sizeof
if (auto p = api.get_position(entity))            // std::optional<array<float,3>>
    use((*p)[0], (*p)[1], (*p)[2]);

api.set_position(entity, 1.0f, 2.0f, 3.0f);

if (auto json = api.comp_get(entity, "Water"))    // optional<string>; nothing
    parse(*json);                                 // to free, ever
```

### Header-only is a correctness decision, not a packaging one

`Api::open`'s `host_size` parameter defaults to `sizeof(JceScriptHost)`, and the
entire point of the short-host ABI is that this number must be the **caller's**.
A compiled wrapper would evaluate that `sizeof` in its own translation unit,
against its own copy of the engine header, and hand the C ABI a size the caller
never agreed to — which is precisely the over-read `jce_script_create_sized`
exists to prevent. Header-only puts the `sizeof` in the consumer's TU.
A test named for that property is what fails if the default ever becomes a
fixed number — it lives in the suite under `tests/`, which is not part of
this repository, so it is described here rather than cited by name: a
citation nobody can resolve reads the same whether the test is missing or
merely renamed.

The second consequence is that there is no new ABI to version: the only ABI on
this path is the C one, which already has `jce_script_api_version()` and a
symbol-table test.

### Language level: **requires C++17, uses C++20**

The header `#error`s below C++17, and the requirement is enforced rather than
documented: `tests/scripting/cpp/test_jce_script_cpp17_floor.cpp` compiles it at
`-std=c++17` / `/std:c++17` and runs it, so a C++20-only construct slipping in
fails a build. (The `#error` reads `_MSVC_LANG` where it exists — MSVC reports
`199711L` in `__cplusplus` unless `/Zc:__cplusplus` is passed, and a bare
`__cplusplus` test would reject every default MSVC build of a conforming C++17
consumer.)

C++20 is **used**: where `<span>` is available (`__cpp_lib_span`), the
`entity_table` entries gain an extra caller-buffer overload
`int find_by_prefix(CStr, std::span<Entity>)` that allocates nothing. A C++17
SDK consumer loses that overload and nothing else.

Both halves of that are enforced, and by a *pair* of assertions rather than by
one. The floor TU asserts `JCE_SCRIPT_CPP_HAS_SPAN == 0` — at C++17 the
overload must not be admitted, or the guard is not what is keeping it out. The
C++20 TU asserts `== 1` **and calls the overload** —
and a test named for that property checks, against the host-call trace, that the *span's* size is what reached the
host as `max`. It is a non-template member of a non-template class, so merely
including the header at C++20 type-checks its body and runs none of it — which
is how an advertised feature ends up built-but-unwired.

### String arguments are `CStr`, deliberately not `std::string_view`

`CStr` converts implicitly from `const char *` and from `const std::string &`
(via `c_str()`, no copy) and **not** from `std::string_view`. A view is not
NUL-terminated, the C ABI needs a `const char *`, and the only way to bridge
that is to copy the bytes to add a NUL — a heap allocation per call, inside
`on_update`. The omission is the point. A default-constructed `CStr` is a null
pointer, which is what the Lua binding passes for an omitted nil-able string
argument.

## What is NOT exposed

The manifest's seven `hand_written` entries — `log`, `asset_read_text`,
`asset_read_json`, `play_sound`, `start_coroutine`, `wait_seconds`,
`stop_coroutine` — as a class, by the same rule the C ABI excludes them by. The
generated header prints all seven with the manifest's own reason text.

Two of them matter more than the others. `asset_read_text` and
`asset_read_json` carry sandbox policy — path validation, a 1 MiB cap, a
depth-capped JSON walk — that lives in `static` functions inside
`jce_script.c`. A convenience wrapper reaching around them to the raw
`read_file` host member is the escape the manifest itself calls **P0-2**, and a
C++ ergonomics layer is the most tempting place in this tree to write it. The
gate refuses it from both directions: `read_file` is not in the C ABI at all, so
there is no symbol to call, and `validate()` fails if the C ABI ever grows one.

## How it is verified

`tests/scripting/cpp/` holds a **cross-language differential**. Batch 1 spent
the free correctness oracle — the hand-written Lua bindings it replaced — and
said so in its own commit. What did not disappear is Lua itself: a shipped,
tested, independently generated implementation of the same manifest. So every
case is derived from the manifest and driven through **both** a real Lua VM and
this wrapper over **one** recording mock host, comparing

* the full result, **type and value per slot** — `nil`, `0` and `false` are three
  different strings, so none can pass for another;
* the **host-call trace** — which member fired, in what order, with which
  arguments. A wrapper that produces the right value by calling the wrong member
  is invisible to a result comparison and visible here.

It is not circular: the C++ result is projected onto the comparison form by
hand-written, **type-keyed** overloads in `jce_script_cpp_differential.hpp`.
There is no per-entry projection, so a per-entry defect in the wrapper has
nothing to be mirrored by.

---

# The other direction: `JceScriptVM` for C++

Everything above is the **call-down** half — a script calling the engine.
`scripting/cpp` also implements the **call-up** half: the `"cpp"` `JceScriptVM`,
which is how the engine drives a script written as a C++ class, through exactly
the entry points it drives Lua with.

```cpp
#include <jce/script_vm/jce_script_cpp.hpp>

class Spinner : public jce::script::Script {
public:
    void on_update(float dt) override {
        angle_ += 90.0f * dt;
        api().set_rotation(entity(), 0.0f, angle_, 0.0f);   // the C ABI
    }
private:
    float angle_ = 0.0f;
};

JCE_CPP_SCRIPT_CLASS(Spinner, "Spinner")

JCE_CPP_MODULE_BEGIN()
    JCE_CPP_MODULE_CLASS(Spinner)
JCE_CPP_MODULE_GLOBALS()
JCE_CPP_MODULE_END("demo", demo_module)
```

Attach it the way a Lua script is attached:

```c
jce_script_vm_cpp_register();                    /* once, at startup */
jce_script_vm_cpp_add_module(demo_module());     /* linked into the game */
/* or: jce_script_vm_cpp_load_library("<absolute path to demo.dll>"); */

JceScript *s = jce_script_vm_create("cpp", &host, sizeof host);
JceScriptInstance i = jce_script_instantiate(s, "Spinner", entity);
jce_script_call_start(s, i);
jce_script_call_update(s, i, dt);
```

A module author links `jce_script_cpp_module` (headers plus the call-down
wrapper) and **not** `jce_script_vm_cpp`: the registry is per-process state
belonging to the host, and a plugin carrying its own copy would publish its
classes into a registry the engine never reads — a module that loads, reports
success, and whose classes no `instantiate()` can find.

## The three decisions, and what each is enforced by

### "Instantiate a script" is a name lookup, not a file read

`jce_script_instantiate(s, path, owner)`'s `path` is a **class key**. A C++
script is already compiled; there is nothing to read and nothing to run.

The alternative — each plugin fills its own `JceScriptVM` — is **refused by the
core**, not by taste: `jce_script_vm_register()` is keyed by language name and
refuses a duplicate, so at most one table can ever own `"cpp"`. A
plugin-supplies-the-VM design would allow exactly **one** native script plugin
per process, and the second would fail with a message about language names. So
the plugin boundary sits one level *below* the VM: this directory owns the
single `"cpp"` table, and modules contribute **classes** to it.
*Enforced by:* `test_jce_script_vm_cpp_modules.cpp` :: "a second table under the
name 'cpp' is refused by the core".

### A module stays mapped while an instance lives

Every pointer in a `JceCppScriptClass` points into the module's image. Unload it
while an instance is alive and the next `on_update` calls through an unmapped
page — inside the engine's own forwarder, with no script in the backtrace — and
**no lifecycle test finds it**, because every lifecycle test unloads last.

So the registry refcounts live instances per module and
`jce_script_vm_cpp_unload()` **refuses** while the count is non-zero, logging how
many. The live instances keep working; release them and unload again.
*Enforced by:* "unloading a module with a live instance is refused, and the
instance still dispatches" — which also asserts the instance still dispatches
*after* the refusal, because a refusal that unmapped anyway would pass a check
that only read the return value. Deleting the refcount check makes that test
print two named failures and then **SIGSEGV**.

### Exceptions never reach the C ABI

Every slot is a C function pointer, so an exception unwinding through one is
undefined behaviour rather than an error. The policy is **the module catches**,
in the frame that called the user code, and reports the failure as a value:
`JceCppStatus` is `NULL` for "returned normally" and otherwise the exception's
text. `JCE_CPP_SCRIPT_CLASS` generates those thunks in the **module's own**
translation unit — catching in the engine would mean an object thrown by one CRT
and caught by another.

Every thunk is additionally `noexcept`, so if a catch is ever narrowed the
program stops at a diagnosable `std::terminate` at the boundary instead of
unwinding into C frames. Measured: deleting the catch turns the run into
`CRASHED: Terminate handler called` at the named case.
*Enforced by:* "an erroring callback is logged on both sides, both sides then
stop calling it, and neither side stops calling anything else", and "a throwing
global handler is caught at the module boundary".

What the engine does with a non-NULL status is **what Lua does, measured rather
than read** — which is now THE FAILING-CALLBACK RULE, stated in full in
`jce_script.h`: log `"<method> error: <text>"` through `host->log` and the
engine log, write the published disable notice after it, and stop calling that
hook on that instance.

**This paragraph used to say the opposite, and the history is the point.** When
this backend was written, `jce_script.h` already promised the disable and
`jce_script.c`'s `call_method()` only logged and returned — no per-instance flag
existed anywhere in the reference. So this backend deliberately matched the
**code** and not the comment, because its acceptance test is a differential
against Lua and obeying the documentation would have made every erroring case
disagree with the reference for a reason that was not this backend's defect.
That reasoning was correct and it is now spent: the reference implements the
sentence, so matching the reference and matching the documentation are the same
thing again.

One divergence stays, pinned: `jce_script.h` says a rebind clears the disables,
and this backend's `rebind_instance` is a documented no-op because a compiled
class cannot be recompiled in-process. Its re-enable is `unload` +
`load_library` + re-instantiate, which is its only hot-reload path anyway.
*Enforced by:* "PINNED: a rebind cannot re-enable a disabled callback for
compiled code, and re-instantiating does".

### Header-only, again — but for a different reason each time

| piece | shape | why |
|---|---|---|
| `jce_script_api.hpp` (call down) | header-only | `Api::open`'s default `host_size` must be the **caller's** `sizeof`. |
| `jce_script_vm_cpp.c` (the VM) | **compiled** | It never originates a size — it *receives* one and clamps against its own `sizeof`, which is the size of the buffer it writes into, so its own TU's `sizeof` is the correct number rather than a leak of one. And it owns the module registry, which must be **one object per process**: a header-only registry gives every TU — and on Windows every DLL, since an `inline` variable is not shared across a module boundary — its own copy, so `unload()` would not see the instances another TU created. |
| `jce_script_cpp.hpp` (module side) | header-only | Twice over, and both stronger than the original: the **try/catch must be compiled in the module**, and `JceCppScriptClass::struct_size` / `JceCppModuleDesc::struct_size` must be the **module's** `sizeof`, because the engine clamps to `min(module, engine)`. |

## What the cpp VM deliberately does **not** support

Each is a slot the core requires to be non-NULL, so each is an explicit no-op
whose reason is in its own source — and each is **pinned by name** in the
differential with the expected value on *both* sides, because an unpinned known
divergence is an immune mutation with a silent reason.

| entry point | cpp VM | why |
|---|---|---|
| `jce_script_instantiate_source` | `0`, logged once per VM | A C++ script is compiled. Returning non-zero would mean silently ignoring the `source` the caller passed. |
| `jce_script_compile_module` | `0` — Lua's own "compile failed" value | Hot-reloading a native class is not a recompile; `self` is a C++ object of a type that would no longer exist. The supported path is `jce_script_vm_cpp_unload` + `_load_library` + re-instantiate. |
| `jce_script_rebind_instance` / `_release_module` | no-op | They only ever receive the `0` above. |
| `jce_script_update_coroutines` | no-op | There is no cooperative scheduler here — `jce.start_coroutine` / `wait_seconds` are two of the manifest's seven hand-written entries and they park a *Lua* coroutine. A C++ script keeps its own timer, which is what `on_update` is for. |

One further divergence, also pinned: **cpp globals are process-wide** where Lua
globals live in one `lua_State`. Native code has one copy of a function per
process, and pretending otherwise would be a fiction with a registry behind it.

## What is *not* re-exposed

A script class reaches the engine through `jce::script::Api` — the same generated
C ABI every other binding uses, which declares none of the manifest's seven
hand-written entries. Nothing in `jce_script_vm_cpp.h` or `jce_script_cpp.hpp`
adds them back. `asset_read_text` / `asset_read_json` carry sandbox policy (path
validation, a 1 MiB cap, a depth-capped JSON walk) living in `static` functions
inside `jce_script.c`; a convenience that reached the raw `read_file` host member
instead is the escape the manifest calls **P0-2**.

The honest scope of that rule, stated rather than implied: **native code inside
the process cannot be sandboxed by the shape of an API.** What is enforceable,
and what is enforced, is that this backend ships no such helper — so nothing here
makes the escape the path of least resistance.
*Enforced by:* "the module-facing surface names none of the hand-written
entries", which strips comments from both headers (they discuss the seven at
length) and searches the remaining code.

## How the lifecycle is verified

`tests/scripting/cpp/test_jce_script_vm_cpp_lifecycle.cpp` is a **cross-language
differential against Lua**, the reference lifecycle: the same script semantics
written twice, driven through `jce_script_call_*`, over **one** recording mock
host, compared on the sequence of host calls, on **type as well as value** (the
Lua side gets its type from `type(x)` — the reference's own answer, not the
test's belief), and on the return of every call that has one.

Before any comparison, `require_lively()` runs on **each side independently** and
demands that every lifecycle kind the drive sequence exercises appears at least
once. A differential in this worktree shipped 150/151 dead because the reference
side produced nothing, and **nothing compares equal to nothing**.

`test_jce_script_vm_cpp_modules.cpp` covers the two things a lifecycle
differential structurally cannot reach: the module-side ABI clamp (with tables
hand-written rather than macro-produced, because a macro can only ever emit
tables the macro can emit) and symbol lifetime across a real shared object.
