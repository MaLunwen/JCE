# Elemental Serenity

A JCE SDK consumer recreating the nature diorama to exercise scene authoring,
seasons, weather, lighting, vegetation, water, multilanguage behavior and
bounded AI scene orchestration.

Open this directory in JCE Editor. The startup scene is
resources/assets/scenes/elemental_serenity.scene.json. From the JCE root:

~~~sh
python scripts/jce.py app examples/elemental_serenity --arch x64 --variant release
~~~

Target: ElementalSerenity. Output: elemental_serenity.exe.
Relocation requires a fresh CMake build directory; old output is preserved
locally and does not establish a current build.

Authoring tools and palette inputs live in tools/. They do not build the
engine. extract_palettes.py --reference-root <checkout> reads an unmodified
upstream checkout. ES_REFERENCE_SHOTS locates captures for comparison tools.

See [source/terms](SOURCE_AND_TERMS.md), [contract](AGENTS.md) and
[historical measurements](MULTILANG.md). Imported assets remain locally
preserved and ignored; scene, scripts and particle data are versioned.

Editor C/C++ scripts load from native/bin/; the SDK app registers the same
classes in process. Native plugins are generated output and are not versioned.
The current manifest targets Windows; other platforms must record their module
extension/output paths. Public SDK main templates and scripting module-writing
headers are part of the supported consumer surface.

CPython requires its standard library and dependent shared libraries on the
runtime search path. For an Anaconda runtime, set PYTHONHOME to that installation
and set ES_RUNTIME_PRELOAD to its Library/bin/ffi.dll. The consumer loads this
external dependency through jce_library_open before language initialization;
no upstream file is copied or changed. PATH alone does not satisfy CPython's
extension-module DLL search policy. This is an external runtime dependency,
not evidence of a self-contained single-executable deployment.

For Editor Play with that runtime, also put this project's
`tools/python_bootstrap` directory on `PYTHONPATH` before starting the editor.
Its CPython `sitecustomize.py` holds an `os.add_dll_directory` handle for the
parent directory of `ES_RUNTIME_PRELOAD`, before the VM imports ctypes. This
bootstrap is opt-in and project-owned; it does not modify the installed
Python/JCE packages or the editor. Preserve any other required PYTHONPATH
entries when adding it. Example launch environment on this machine:

~~~powershell
$env:PYTHONHOME = 'D:/anaconda3'
$env:ES_RUNTIME_PRELOAD = 'D:/anaconda3/Library/bin/ffi.dll'
$env:PYTHONPATH = (Resolve-Path './tools/python_bootstrap').Path
# Launch JCE Editor and open this project.
~~~
