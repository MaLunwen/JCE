# Elemental Serenity

A JCE SDK consumer recreating the nature diorama to exercise scene authoring,
seasons, weather, lighting, vegetation, water and bounded scene orchestration.

The ordinary project, portable Lua Bundle and single-executable product select
the same portable scene and Lua behavior. Open this directory in JCE Editor;
its default `jce_project.json` selects
`resources/assets/scenes/elemental_serenity.bundle.scene.json`. File/Open Scene
or Assets/scenes can select that same source without a forced Bundle launch.
The six scene-bound scripts are portable Lua; the default manifest requires
only Lua and declares no external language VM or project DLLs.

From the JCE root, the supported product build uses a matching embedded SDK:

~~~sh
python scripts/jce.py build-project tests/consumers/elemental_serenity --sdk dist/sdk/windows-x64-dist-embedded --arch x64 --variant dist --delivery-profile embedded
~~~

The default C99 entry `src/main_portable.c` instantiates the installed public
empty-project template over `jce_default_main.inc.h`. It runs the serialized
Lua scene through the shared app callbacks, with no custom native plan provider,
N shortcut, probe loop or performance HUD. The original custom entry and those
extensions remain explicitly available in the multilang SDK fixture. Ordinary
project packing consumes the selected portable Director's own dependency sidecar,
which preserves the existing season-texture/audio declaration bytes.

The target is ElementalSerenity; output is elemental_serenity.exe. Reuse its
compatible target/variant/profile build cache. Product acceptance still needs
actual Editor Play/Stop, a packaged Bundle reopened in Editor, and the final
GUI executable relocated into an otherwise empty directory. Source and policy
checks do not establish those runtime, visual or performance results.

For the published content entry see the
[portable Lua catalog](../../../examples/elemental_serenity/README.md).
The Bundle is content; a stock runtime component alone is not an ES application.
Authoring source assets and the selected packed asset closure are distinct;
this default correction does not copy, regenerate or remove either scene,
portable scripts, authored pipeline settings or historical Bundle bytes.

## Explicit five-language SDK fixture

`jce_project.multilang.json` preserves the original manifest bytes and selects
`resources/assets/scenes/elemental_serenity.scene.json`. Its Lua, Python, Java,
C++ and C requirements, native class references, factories and two Editor module
paths remain intact. This is a separate SDK capability fixture, not the default
single-executable product or performance baseline.

The existing consumer CMake owner selects that manifest and full registration
closure only with `ES_MULTILANG=ON`. Use the same source tree with a distinct
compatible authoring-SDK cache; no source or asset-tree copy is required:

~~~powershell
cmake -S tests/consumers/elemental_serenity -B build/consumers/elemental-serenity/windows-x64-release-authoring-multilang -DJCE_DIR="$env:JCE_AUTHORING_SDK/lib/cmake/JCE" -DCMAKE_BUILD_TYPE=Release -DJCE_BUILD_VARIANT=release -DES_MULTILANG=ON
cmake --build build/consumers/elemental-serenity/windows-x64-release-authoring-multilang --config Release --target ElementalSerenity --parallel 2
~~~

`JCE_AUTHORING_SDK` must name an installed SDK built with all five backends and
its actual external runtimes. Missing required backends refuse configuration;
no scene-bound script is silently omitted. Embedded SDK success for the default
Lua project does not certify Python/Java availability. `build-project` and
ordinary Editor project-open consume `jce_project.json`, not the named fixture
manifest; the command above is the explicit installed-SDK fixture route.

The explicit fixture's public generated shim remains its sole static native
publisher and must pass registration before the selected scene loads. The
Lua-only default requires no generated shim or explicit VM registration. Optional
fixture modules compile the same native sources against the public call-down
ABI. Their generated output and declared paths are not proof that an Editor
loaded them or that every language ran. The selected probe table reports actual
script-owner paths; the portable case does not claim five languages are live.

CPython/JVM bootstrap is relevant only to this explicit fixture. CPython needs
its standard library and dependent shared libraries, while Java needs its real
JVM, compiled classes and JNI shim. The existing optional `ES_RUNTIME_PRELOAD`
loads a deployment-provided dependency through `jce_library_open`; Python's
`tools/python_bootstrap/sitecustomize.py` can retain its required DLL directory.
Those are external authoring dependencies, not single-executable delivery.

Authoring tools and palette inputs live in tools/. They do not build the engine.
`extract_palettes.py --reference-root <checkout>` reads an unmodified upstream
checkout; `ES_REFERENCE_SHOTS` selects comparison inputs. See
[source/terms](SOURCE_AND_TERMS.md), [contract](AGENTS.md) and
[historical measurements](MULTILANG.md). Native outputs remain generated and
unversioned. Historical measurements do not certify either current entry.

## Consumer regression source location

This directory retains authoring source and project-specific regression tools.
The redistributable single-file scene Bundle and canonical source/license notices
are in `../../../examples/elemental_serenity/`. Generated build trees, `.jce`, cooked
assets and local original/unknown resources were not migrated or published.
Elemental Serenity staging accepts an explicit licensed `--resource-root`; do not
copy private assets into the tracked fixture tree to satisfy missing files.

For repeatable portable Lua capture runs, the existing staging tool accepts
`--capture-seeds 317 91` alongside its required `--project`, `--resource-root`, `--out` and
`--packer` arguments. It writes numeric `capture_seed_a` / `capture_seed_b`
Script parameters only into the staged Director, and records the pair and input
hashes in `staging-manifest.json`. Both values must be integers in
`0..16777215`, exactly representable by the public float32 parameter field.
Omitting the flag preserves the source scene bytes and normal random policy.
This does not mute audio; `--qa-mute` remains an independent content choice.

Pass this authored project as `--project`; the public Bundle packer collects
its `Settings/RenderPipeline.rp.json` under the canonical runtime settings key.
The stager records its source SHA-256 and the actual Bundle BOM content identity,
rejecting omission, duplication or source changes during export. This carries
the complete authored policy, including disabled SSAO/SSR/volumetric fog/motion
blur and the authored shadow settings. No runtime feature is hardcoded by the
stager. BOM identity is packaging evidence; native reopened payload and applied
renderer settings remain separate checks. Existing output directories are
refused; retain prior Bundle evidence before regenerating the same build tuple.

The Director seeds Lua once at startup before its first random use and emits
`ES_CAPTURE_SEED`. It controls the shared Lua VM stream, not native particle or
GPU random sources. Compare the same staged Bundle in fresh runtimes with the
same simulation dt, input schedule and capture frame. CPU replay tests do not
certify pixel identity or normal-clock performance; those need actual captures.
