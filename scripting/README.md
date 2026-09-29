# `scripting/` — what a PROJECT has to do to get Python, Java, C++ or C

This file answers one question, because until 2026-08-16 nothing in the
repository did and a real project hit every edge of it in one afternoon:

> My scene has an entity whose `Script` component says `turret.py`. What do I
> have to do so that it runs?

The per-backend headers explain their own internals well. What was missing is
the outside view — the four things an application must do, in order, and what
each one looks like when you skip it.

Everything below was measured on the Elemental Serenity project (four scripted
entities, one per language) on 2026-08-16.

---

## The one-paragraph version

The engine registers exactly **one** language for itself: the built-in Lua VM,
lazily, on first use. Every other backend registers itself from a function
**your application calls**. There is no central list of backends anywhere and
that is deliberate (`jce_script_vm.h`, *REGISTERING A LANGUAGE*) — which also
means nothing will do it for you. Skip it and the scene still loads, the Lua
scripts still run, and each non-Lua entity is refused by name in the log.

---

## The four steps

### 1. Build the backend

|            | option / condition                                            | default |
|------------|---------------------------------------------------------------|---------|
| **Python** | `JCE_BUILD_SCRIPT_VM_PYTHON`, **and** CMake must find `Python3 COMPONENTS Development.Embed` | ON, but the target does not exist without an embeddable CPython |
| **Java**   | `JCE_BUILD_SCRIPT_JAVA=ON` + `JAVA_HOME` pointing at a **JDK** | **OFF** |
| **C++**    | no option of its own — `scripting/cpp` always builds `jce_script_vm_cpp` | always |
| **C**      | no option of its own — `scripting/c` always builds `jce_script_vm_c` (it links `jce_script_vm_cpp`) | always |
| all three  | `JCE_BUILD_SCRIPT_API_SHARED=ON` — the C ABI the Python and Java bindings CALL | ON |

Test for the **target**, not for the option: an option that is ON and a
toolchain that was not found produce the same absent target and only one of
them prints anything.

```cmake
if(TARGET jce_script_vm_python)
    target_link_libraries(MyGame PRIVATE jce_script_vm_python jce_script_api)
    target_compile_definitions(MyGame PRIVATE MYGAME_HAVE_PYTHON=1)
else()
    message(WARNING "MyGame: Python scripting OFF — scripts/*.py will be refused at runtime")
endif()
```

**Order matters and it fails silently.** `if(TARGET ...)` reads FALSE for a
target declared by a *later* `add_subdirectory()`. Both `editor/` and
`elemental_serenity/` were first written above `add_subdirectory(scripting)`
in the root `CMakeLists.txt`, configured green with all three backends ON, and
linked none of them. The only thing that caught it was that each site printed
a warning naming the missing backend. Write that warning.

### 2. Link it, and ship `jce_script_api` beside your executable

`jce_script_api` is a **shared** library. The Python binding loads it with
`ctypes`; the Java binding's JNI shim links it. A missing DLL is a loader
failure *before* `main()`, which prints nothing and looks exactly like a
crash. Copy it next to the exe in a POST_BUILD step.

### 3. Call the backend's `register()` — **before your first scene loads**

A scene's `Script` components are instantiated inside `jce_runtime_create()`.
A language registered after that point is a language every entity in that
scene has already been refused by.

```c
/* first statement of the app's init, before the stock app_init() */
if (!jce_script_vm_python_register())
    /* logged with the reason; the engine keeps running on Lua */;
```

Two complete, working examples in this repository:

* `tools/gpu_task/languages/*/task.cmake` + `src/es_gpu_task_main.c` — all
  four languages, generated language table.
* `editor/src/core/jce_editor_script_backends.cpp` — the editor's own, with
  the environment-override / build-time-default path resolution a *shipped*
  tool needs.

### 4. Tell it where its runtime lives

No backend can discover this for itself.

* **Python** — the `jce_script` package on `sys.path`
  (`jce_script_vm_python_add_path()`), and `$JCE_SCRIPT_API`. Also set
  `PYTHONDONTWRITEBYTECODE=1` before the first VM is created if the package
  directory is inside a source tree: CPython invalidates `.pyc` on
  (mtime, size), and a same-size edit inside one mtime second leaves the old
  bytecode running.
* **Java** — `jce_script_vm_java_configure()` with an **absolute** `jvm.dll` /
  `libjvm.so` (a bare name is refused on purpose — `jce_library_open()` would
  attach to whatever JVM another component loaded first), a class path holding
  `com.jce.script.*` (build it with `scripting/java/build_java.py`), and
  `-Djce.script.library=<absolute path to the JNI shim>`.
* **C++** — publish your module with `jce_script_vm_cpp_add_module()` (linked
  in) or `jce_script_vm_cpp_load_library()` (a shared object, absolute path).
* **C** — **the same two calls.** One native class registry serves both
  languages, so there is no `jce_script_vm_c_add_module`; see *C, and what a
  `.jcec` script is* below.

---

## How a script is routed to a language

`jce_script_vm_language_for_path()` looks at the **file extension** of the
`Script` component's `scriptPath`, and each backend claims its own extensions
from inside its own `register()`:

| extension        | language | claimed by |
|------------------|----------|------------|
| `.lua`           | lua      | the engine, when the built-in registers |
| `.py`            | python   | `jce_script_vm_python_register()` |
| `.java`, `.class`| java     | `jce_script_vm_java_register()` |
| `.jcecpp`        | cpp      | `jce_script_vm_cpp_register()` |
| `.jcec`          | c        | `jce_script_vm_c_register()` |

`JCE_SCRIPT_LANGUAGE` overrides this **for the whole process**. It is not a
per-entity setting and a scene with more than one language must not use it.

### The C++ path form: `.jcecpp`, and how it becomes a class

A cpp "script" is a **class in a compiled native module**, not a file the
engine opens — so its `scriptPath` *names* code instead of containing it. Two
questions follow, and they have two different answers:

**Which VM runs it?** `jce_script_vm_language_for_path()`, which answers only
by extension. So `jce_script_vm_cpp_register()` claims one, `.jcecpp`.

> It is deliberately **not** `.cpp`. That would classify every translation
> unit in the project as an attachable script — the Script-picker defect
> `eedff3ea` closed — and would tell the cooker and the publication policy to
> pack the project's C++ **source** into the shipped game. `.jcecpp` is
> engine-namespaced and collides with no build input.

**Which class?** `cpp_instantiate` tries three candidates, in this order:

| # | candidate | `"gameplay/FlowerSway.jcecpp"` resolves to |
|---|---|---|
| 1 | the whole stored string, exactly | `"gameplay/FlowerSway.jcecpp"` |
| 2 | its last path component | `"FlowerSway.jcecpp"` |
| 3 | that component without its final extension | `"FlowerSway"` |

So a module author writes the class name the way C++ spells it:

```cpp
JCE_CPP_SCRIPT_CLASS(FlowerSway, "FlowerSway")   /* scriptPath: "FlowerSway.jcecpp" */
```

**Candidate 1 is first, always, and that is the compatibility guarantee.**
Every class name that resolved when the match was a single `strcmp` still
resolves through it, including one deliberately named after a whole stored
path. A project that invented its own claim before `.jcecpp` existed — e.g.
`jce_script_vm_register_extension("escpp", "cpp")` with
`JCE_CPP_SCRIPT_CLASS(MyClass, "MyClass.escpp")` — keeps working unchanged;
both claims can coexist, because they are different extensions.

**A path with no extension is not a special case.** Candidate 3 simply does
not exist for it, and candidates 1 and 2 are what a bare `"MyClass"` always
meant. Nothing here validates that the extension is one the cpp backend
claimed: making resolution depend on whether the claim call had run yet would
make the same path resolve differently depending on startup order.

**There need not be a file.** The path can be typed straight into the Script
component. A project that *does* drop a stub `FlowerSway.jcecpp` beside its
other scripts gets the editor's asset browser, the Script picker and the
publication policy for free, because all three read the offline catalog row
(`engine/src/resource/jce_asset_ext.c`, form `REFERENCE` — nothing compiles
those bytes). `check_script_language_catalog.py` fails if the
runtime claim and that row ever disagree.


### C, and what a `.jcec` script is

**C is a driver language, not a dialect of `cpp`.** It has its own
`jce_script_vm_register()` name (`"c"`), its own claimed extension (`.jcec`),
its own row in the offline catalog, and every message the backend prints for a
C script says *c*. What it deliberately does **not** have is its own class
registry: one native registry serves both languages.

That split is where the boundary can be *observed*. A compiled class has no
language at run time — what reaches the engine is a table of C function
pointers — so a second registry would only mean a second loader, a second
unload refcount and a second answer to "is this module already loaded", for one
set of `.dll` files. What a *user* meets is different in every respect, and
that is what "c" names.

| | C | C++ |
|---|---|---|
| header | `<jce/script_vm/jce_script_vm_c.h>` | `<jce/script_vm/jce_script_cpp.hpp>` |
| module target | `jce_script_c_module` | `jce_script_cpp_module` |
| host links | `jce_script_vm_c` | `jce_script_vm_cpp` |
| register | `jce_script_vm_c_register()` | `jce_script_vm_cpp_register()` |
| extension | `.jcec` | `.jcecpp` |
| publish a module | `jce_script_vm_cpp_add_module()` / `_load_library()` | the same two |
| class registry | **shared** | **shared** |

> `.c` and `.h` are refused by construction, exactly as `.cpp`/`.hpp` are. A
> `.c` row would make every translation unit in the project an attachable
> script; a `.h` row would additionally sweep in every vendored third-party
> header — and both would tell the cooker and the publication policy to pack
> that source into the shipped game.

A C script class fills the same struct a C++ one does, because that struct was
always a C struct. **There are no thunks**: `jce_script_cpp.hpp` exists mostly
to wrap every callback in a try/catch compiled in the module's own TU, and C
has nothing to catch. A C author writes the ABI signature directly.

```c
#include <jce/script_vm/jce_script_vm_c.h>

typedef struct Spinner { float angle; } Spinner;

static void *spinner_create(const JceCScriptContext *ctx)
{
    Spinner *self = (Spinner *)calloc(1, sizeof *self);
    (void)ctx;                       /* ctx->entity, ctx->host, ctx->host_size */
    return self;                     /* NULL == this instance failed */
}
static void spinner_destroy(void *self) { free(self); }

static JceCStatus spinner_update(void *self, float dt)
{
    ((Spinner *)self)->angle += 90.0f * dt;
    return JCE_C_OK;                 /* or a string: that IS the error report */
}

JCE_C_SCRIPT_CLASS_BEGIN(Spinner, "Spinner", spinner_create, spinner_destroy)
    JCE_C_ON_UPDATE(spinner_update)  /* write the handlers you have, any order */
JCE_C_SCRIPT_CLASS_END()

JCE_C_MODULE_BEGIN()
    JCE_C_MODULE_CLASS(Spinner)
JCE_C_MODULE_GLOBALS()
JCE_C_MODULE_END("demo", demo_module)
```

The scene stores `"Spinner.jcec"`; the host does this once, before the first
scene loads:

```c
jce_script_vm_c_register();                    /* language + ".jcec"        */
jce_script_vm_cpp_add_module(demo_module());   /* linked in                 */
/* or jce_script_vm_cpp_load_library(absolute); -- a shared object          */
```

Error reporting is the return value. `JCE_C_OK` (which is `NULL`) means the
callback returned normally; any other pointer is the error text, and the engine
then does exactly what it does for an escaped C++ exception and for a Lua
error — logs `"<method> error: <text>"` through the host log and the engine
log, and stops calling **that** hook on **that** instance. The pointer must
outlive the call, so use a string literal or a per-instance buffer.

**A C script has no `log`**, for the same reason a C++ one does not: it is one
of the seven hand-written entries `scripting/c_abi` deliberately does not
export. A C script that must report liveness writes it into the scene.

#### The three candidate forms, unchanged

`.jcec` resolves through the *same* `class_resolve()` as `.jcecpp`, so
`"gameplay/Spinner.jcec"`, `"Spinner.jcec"` and a bare `"Spinner"` all reach a
class published as `"Spinner"` — and **candidate 1, the whole stored string, is
tried first, always**. That order is what makes every added form additive: a
class published under a whole stored path still wins, so nothing that resolved
before the C language existed resolves anywhere else now.

#### Two native modules in one binary — `JCE_SCRIPT_MODULE_NO_ENTRY`

A project that writes both native languages publishes **two modules** (one
descriptor is one translation unit, and a translation unit is C or C++). If
both are compiled **into the same binary** — which is what a shipped game
wants, because then there is no plugin to find — you must define
`JCE_SCRIPT_MODULE_NO_ENTRY` on that compile:

```cmake
# the game executable: two native script modules linked in
target_compile_definitions(MyGame PRIVATE JCE_SCRIPT_MODULE_NO_ENTRY=1)

# the loadable modules: NOT defined here — the entry symbol is their only door
add_library(my_scripts   MODULE src/my_flowers.cpp)
add_library(my_scripts_c MODULE src/my_props.c)
```

Without it:

```
my_props.c.obj : error LNK2005: jce_cpp_script_module already defined in
                 my_flowers.cpp.obj
MyGame.exe : fatal error LNK1169: one or more multiply defined symbols found
```

`JCE_C_MODULE_END` and `JCE_CPP_MODULE_END` both emit the shared-object entry
point `jce_cpp_script_module`, and that is **one external symbol** because one
loader looks up one name. Inside an executable the entry is dead code — a
statically linked host reaches a module through the accessor the macro also
emits and hands it to `jce_script_vm_cpp_add_module()`, and nothing `dlsym()`s
a symbol out of its own process image — so suppressing it there costs nothing.
Suppressing it in a **shared** build does not: that module loads and is then
refused as *not a JCE script module*, which is a failure that survives
shipping, unlike a link error.

`tests/scripting/c/` links two static C modules for exactly this reason;
removing the define from that target is a mutation whose result is a build
failure rather than a red assertion.

#### What each step looks like when you skip it

| skipped | symptom |
|---|---|
| **link `jce_script_vm_c`** | link succeeds; at run time `jce_script_vm_create("c")` returns NULL and the log says *no script VM registered for language 'c'*. `if(TARGET jce_script_vm_c)` is the guard — and it reads FALSE for a target declared by a *later* `add_subdirectory()`, so print from both branches. |
| **call `jce_script_vm_c_register()`** | the scene loads, Lua still runs, and every `.jcec` entity is refused with *no script VM claims its extension* — followed by the registered languages, which will list `cpp` and not `c`. Registering the **cpp** backend does not register C: they are two calls. |
| **register after the first scene loads** | silent-looking: `Script` components are instantiated inside `jce_runtime_create()`, so every entity in that scene was already refused before the claim existed. |
| **publish the module** | *no c script class resolves 'Spinner.jcec'* … *NO NATIVE SCRIPT MODULE IS LOADED IN THIS PROCESS*. The message names the three candidates it tried, so a typo and an unloaded module do not look alike. |
| **build the module as a plain shared library** | the editor reports *loads but is NOT A JCE SCRIPT MODULE: it exports no `jce_cpp_script_module`*. `JCE_C_MODULE_END` emits that symbol; a module built only INTO the game executable has no entry symbol and nothing else can load it. |
| **use `.c` as the extension instead** | there is no such row and there must not be: the path resolves to no language and the entity is refused. Rename the `scriptPath`, not the catalog. |
| **name the class after the file with its extension** | works. Candidate 1 matches the whole stored string, which is why a project that did this before the language existed keeps running unchanged. |

## What editor Play can and cannot run

`JCE_Editor` registers the interpreted backends it was built with (Python,
Java) plus the cpp and c VMs, so Play runs what the game runs. C and C++ are
**two separate `register()` calls** — registering one would leave the other
resolving to no language in Play while the shipped game ran it — and they need
only **one** loaded module between them, because the class registry is shared. Measured on the
Elemental Serenity scene: **1 of 4 languages before any of this wiring
existed, 3 of 4 once Python and Java were registered, 4 of 4 once the editor
also loaded the project's C++ module.**

C++ is the one that needs more than a `register()` call, because a cpp script
is a class in a module the **project** builds and the editor is a different
process. The project names its module in `jce_project.json`:

```json
"script_modules": ["build/release/my_scripts.dll"]
```

Each entry is a path to a shared object, relative to the project root or
absolute; the editor resolves it, loads it with
`jce_script_vm_cpp_load_library()` when a project is opened, and unloads it
when the project closes. `JCE_SCRIPT_CPP_MODULES` overrides the list for one
run (`;`-separated on Windows, `:` elsewhere) so a developer can aim the
editor at a different build without editing the manifest.

The module must be built as a **shared library** linking
`jce_script_cpp_module`; `JCE_CPP_MODULE_END` emits the exported entry symbol
that makes it loadable. A module compiled only INTO the game executable is
reachable by that executable and by nothing else.

**Every way it can fail says which one it is** — missing file, a file the OS
refuses to map (wrong architecture, a missing dependent DLL, a build
configuration this editor cannot load), a library that exports no entry
symbol, and a module built against a different module ABI are four different
messages with four different fixes. None of them is "unknown language".

## Out-of-tree, against an installed SDK

**This section said "nothing, yet" until 2026-08-16.** It was true and it was
measured: the SDK installed the engine fat library, the headers and the host
tools, and nothing whatever from `scripting/` — no `jce_script_api`, no
`jce_script_vm_*` archive, no `jce_script` Python package, no compiled Java
classes — and `JCEConfig.cmake` exported no target for any of them. On the SDK
this repository had produced (`dist/sdk/win32-x86_64`, commit `d3ecc452`) a
`find_package(JCE REQUIRED)` consumer saw `JCE::JCE` and no `JCE::Script*`
target and no `JCE_SCRIPT_*` variable, **with no error**: `if(TARGET
jce_script_vm_python)` was simply FALSE, which is also what it says on a
machine that merely has no CPython.

The SDK now ships the layer. `scripting/cmake/JCEScriptingInstall.cmake`
installs it and generates `lib/cmake/JCE/JCEScripting.cmake`, which
`JCEConfig.cmake` includes **OPTIONALly** — because every SDK produced before
2026-08-16 has no such file, and a required include would turn each of them
from "engine-only" into "broken".

### Producing an SDK that can do this

The four steps below are the CONSUMER's. The producer has one, and it has a
trap in it: `python scripts/jce.py sdk` ships **cpp and python but never
java**, because `JCE_BUILD_SCRIPT_JAVA` defaults OFF (so that producing an SDK
never requires a JDK) and nothing turned it on.

```
python scripts/jce.py sdk --script-java --smoke
```

`--script-java` needs `JAVA_HOME`, `jni.h` and `javac` on the PRODUCER's
machine — never on the consumer's, which supplies only a JVM at runtime.
Python needs no flag but does need an embeddable CPython at SDK-build time
(`Python3 COMPONENTS Development.Embed`); without one the backend is skipped
with a STATUS line and the SDK simply ships without it.

What actually arrived is stamped in the SDK's `VERSION.txt`, read off the
installed files rather than off the options that were passed:

```
scripts: lua (built in) + backends shipped: cpp, java, python
```

A backend counts only when every piece it cannot run without is there — Java
needs the JNI shim and the compiled `com.jce.script` classes as well as the
archive. Shipping a backend is necessary, not sufficient: the consumer still
supplies the matching CPython or the JVM, and `JCEScripting.cmake` decides
that on their machine.

### The four steps, out-of-tree

They are **the same four steps**, and mostly the same lines:

```cmake
# 1. Build it -> the SDK already did.  Ask for what you need BY COMPONENT:
#    on an SDK that cannot run Python this fails HERE, naming the component
#    and the reason, instead of leaving a target quietly missing.
find_package(JCE REQUIRED COMPONENTS ScriptPython)

add_executable(MyGame src/main.c)

# 2. Link it.  jce_script_vm_python and jce_script_api are ALIASES of
#    JCE::ScriptVmPython / JCE::ScriptApi, so this line is identical to the
#    in-tree one -- `if(TARGET jce_script_vm_python)` answers the same
#    question in both worlds.
target_link_libraries(MyGame PRIVATE JCE::JCE jce_script_vm_python jce_script_api)

#    ...and stage the shared library the bindings load BY NAME.  Skipping it
#    is a loader failure that prints nothing.
jce_script_stage_runtime(MyGame)

# 4. Tell the backend where its runtime lives (step 3, register(), is C).
target_compile_definitions(MyGame PRIVATE
    MYGAME_PY_PACKAGE_DIR="${JCE_SCRIPT_PYTHON_PACKAGE_DIR}")
```

### What the SDK installs, and under what name

| artefact | installed as | reached as |
|---|---|---|
| C ABI shared library | `lib/<CONFIG>/jce_script_api.{dll,so}` + import lib | `JCE::ScriptApi` / `jce_script_api`, `JCE_SCRIPT_API_LIBRARY` |
| `jce_script_api.h` / `.hpp` | `include/jce/script_api/` | via `JCE::ScriptApi` |
| C++ call-down wrapper | `include/jce/script_vm/jce_script_cpp.hpp` | `JCE::ScriptCpp` / `jce_script_cpp` |
| what a C++ MODULE links | (headers only) | `JCE::ScriptCppModule` / `jce_script_cpp_module` |
| `jce_script_vm_cpp` | `lib/<CONFIG>/` + `include/jce/script_vm/` | `JCE::ScriptVmCpp` / `jce_script_vm_cpp` |
| `jce_script_vm_c` | `lib/<CONFIG>/` + `include/jce/script_vm/jce_script_vm_c.h` | `JCE::ScriptVmC` / `jce_script_vm_c` |
| what a C MODULE links | (headers only) | `JCE::ScriptCModule` / `jce_script_c_module` |
| `jce_script_vm_python` | `lib/<CONFIG>/` + `include/jce_script_vm_python.h` | `JCE::ScriptVmPython` / `jce_script_vm_python` |
| the `jce_script` package | `share/jce/scripting/python/jce_script/` | `JCE_SCRIPT_PYTHON_PACKAGE_DIR` |
| `jce_script_vm_java` | `lib/<CONFIG>/` + `include/jce/script_vm/` | `JCE::ScriptVmJava` / `jce_script_vm_java` |
| the JNI shim | `lib/<CONFIG>/jce_script_java.{dll,so}` | `JCE_SCRIPT_JAVA_JNI_LIBRARY` |
| compiled `com.jce.script` classes | `share/jce/scripting/java/classes/` | `JCE_SCRIPT_JAVA_CLASS_PATH` |

`JCE_SCRIPT_LANGUAGES` lists what this SDK can run beyond Lua, and
`jce_script_java_find_jvm(<out>)` resolves the `jvm.dll` / `libjvm.so` from
`JAVA_HOME` (the JDK is a RUNTIME choice; nothing here links one).

The Python shim's header installs FLAT, as `include/jce_script_vm_python.h`,
because in-tree the target's public include directory is
`scripting/python/src` and every existing consumer writes
`#include "jce_script_vm_python.h"`. Namespacing it in the SDK only would give
one header two spellings depending on how the project was built.

### What is checked, and where

* `check_sdk_scripting_export.py` — every library `scripting/`
  declares either ships or carries a reasoned `# SDK-EXEMPT:` line, every
  installed target has the in-tree-named alias, and `JCEConfig.cmake.in` still
  includes the fragment. Source-only; runs in `tools/lint/run_all.py`.
* `tests/sdk_smoke_scripting/` — a plain-C99 project built **out of tree**
  against an installed SDK: registers the Python backend, resolves `.py` to
  `python`, reads its script **out of its own cooked PAK**, and asserts
  `on_start` once and `on_update` three times. It also starts a JVM from the
  SDK's own classes and shim, and creates the `cpp` VM. Run by
  `scripts/jce.py smoke` / `jce.py sdk --smoke`; it reports the SKIP with a
  reason on an SDK that ships no scripting layer.

### One platform note the engine's own message does not make

`jce_script_vm_python.c` says the C ABI library should be *"beside the
executable"*. On Windows that is found, because the loader's search path
includes the executable's directory. On Linux and macOS it is **not**: the
Python binding's search order is `$JCE_SCRIPT_API`, then the directory holding
`jce_script/`, then the working directory, then the loader's own path — and
none of those is the exe's directory. Set `JCE_SCRIPT_API` to
`JCE_SCRIPT_API_LIBRARY` there.

### The in-tree path is still the in-tree path

`JCE_BUILD_INTREE_ES=ON` builds Elemental Serenity inside this repository and
`tools/gpu_task` is the other in-tree consumer. Those exist because they need
the tree for other reasons, not because the SDK cannot do this any more.

## Diagnosing a script that does nothing

The runtime already prints everything needed; the order to read it in is:

1. `script VM registered: '<lang>'` — step 3 happened.
2. `script extension claimed: '.<ext>' -> '<lang>'` — step 3's claim happened.
3. `script VM created for language '<lang>'` — an entity asked for it.
4. `script: loaded '<path>' (<lang>) for entity <id>` — it instantiated.

A missing (1) is step 1/2/3. A missing (2) with (1) present is a backend that
registered but claimed nothing. `no script VM claims its extension` prints the
registered languages and the claimed extensions next to it, which distinguishes
"no backend implements it" from "the backend is not in this exe".

And note what a non-Lua script CAN print. `jce.log` is reachable from Lua
(built in), Python (`jce_script/vm.py`'s `_Jce.log`) and Java
(`JceEntityScript.log`) — all three go to `JceScriptHost::log`. **A C or C++
module has no log at all**: it reaches the engine only through `jce::script::Api`, the
wrapper over `scripting/c_abi`, and `log` is one of the seven entries that ABI
deliberately does not export. A C++ script that needs to report liveness has
to write it into the scene (Elemental Serenity writes counters into a
transform-only probe entity and prints the table from C).

The host log line itself is tagged `[script]`, not with the language: one
`JceScriptHost` is shared by every VM the runtime stands up, and a host
callback receives only `user`. The line that names the language is (4) above.
