# Public SDK render liveness lab

The lab exercises missing colour -> source restored without reset -> reset
recovery, then the same sequence for grass shadow depth, followed by visibility,
caster, main-depth and shadow-mask controls. One scene renderer and one grass
entity remain alive. Receiver casting is off, so grass is the only authored
caster. Wind and renderer time are explicitly zero.

The normal target is JceRenderLivenessLab. JceRenderLivenessLabNoReset compiles
the same source with only the public reset call withheld. It must fail the
ordinary phase witness at colour_recovered after two actual completed captures.

## Prerequisites and standard build

Use a separately installed, matching native release SDK with
JCE_EMBED_ENGINE_SHADERS=OFF. The runtime checks the public embedded-PAK
accessor and refuses an ordinary embedded SDK. Do not replace the shipping SDK.

The shared SDK driver's isolated build/install/embed-switch entry is maintained
by the SDK build owner. Its standard producer entry is:

~~~text
python scripts/jce.py sdk --variant release --arch x64 --build-dir build/fixtures/render-liveness-sdk-off --install-dir dist/sdk/render-liveness-sdk-off --embed-engine-shaders off --profiling off --no-debug
~~~

If those options are unavailable, SDK production is a prerequisite gap; do not
substitute a private accessor, manual compiler recipe or edit the default cache.

The matching OFF engine SDK and the verified shader stock are separate inputs.
An OFF SDK does not advertise or install stock shaders. Supply an explicit stock
root from a verified shader-enabled SDK whose shader sources match the engine
build receipt, and use the OFF SDK's actual public host packer. All grass stages
for the chosen backend must exist in that stock. The example below uses the
separately verified shader-enabled SDK stock; record its identity alongside the
OFF SDK receipt. The tool only packs assets; it does not build the engine or
compile shaders.

~~~text
python -B tests/consumers/render_liveness_lab/tools/prepare_inputs.py --stock-root dist/sdk/win32-x86_64-dist-single/share/jce/shaders --packer dist/sdk/render-liveness-sdk-off/bin/jce_pak.exe --backend d3d11 --out build/tests/render_liveness_lab/inputs
~~~

Set the absolute prepared full resource directory, then invoke the shared
release consumer entry from the repository root (PowerShell shown):

~~~powershell
$env:JCE_LAB_BOOT_RESOURCE_ROOT = (Resolve-Path build/tests/render_liveness_lab/inputs/full).Path
python scripts/jce.py app tests/consumers/render_liveness_lab --sdk dist/sdk/render-liveness-sdk-off --arch x64 --variant release --delivery-profile embedded
python scripts/jce.py app tests/consumers/render_liveness_lab --sdk dist/sdk/render-liveness-sdk-off --arch x64 --variant release --delivery-profile embedded --target JceRenderLivenessLabNoReset --exe jce_render_liveness_lab_no_reset.exe
~~~

CMake's existing jce_add_pak packs/embeds the explicit complete boot shader tree
plus the fixture resource. The separate controlled PAKs are runtime diagnostic
inputs. This is not a final distribution layout or a single-file delivery test.
Release is intentional: Dist's shared selected-content prebuilt PAK path needs
its own selected graph and must not be bypassed with an external CMake directory.

## Actual execution and evidence

Only the root GPU lease may execute:

~~~text
python -B tests/consumers/render_liveness_lab/tools/run.py --exe tests/consumers/render_liveness_lab/build/windows-x86_64-release/jce_render_liveness_lab.exe --inputs build/tests/render_liveness_lab/inputs --out build/tests/render_liveness_lab/normal
python -B tests/consumers/render_liveness_lab/tools/run.py --exe tests/consumers/render_liveness_lab/build/windows-x86_64-release/jce_render_liveness_lab_no_reset.exe --inputs build/tests/render_liveness_lab/inputs --out build/tests/render_liveness_lab/no-reset
~~~

The first command requires all actual readbacks and must exit 0. The second must
exit 2 with failure.json recording phase=2,
reason=production_phase_witness_failed and accepted=0. It must have completed
phase 0/1 PNG and JSON records; a crash or embed-ON refusal is not this control.
Separately, the normal binary built with an ordinary embedded SDK should refuse
admission; it must never yield empty successful captures.

Every phase JSON contains production trace counters consumed immediately through
the public cursor. A lost cursor, changed owner/entity, zero accepted colour,
missing failure reasons or an unnecessary successful colour-program replacement
fails. Nonzero shadow scopes alone do not prove casting: independent captured
pixels must change on the receiver outside the grass geometry mask. Main-depth
and shadow-mask on/off captures are separate controls. Fixed/repeat and
restored-without-reset pictures must remain within the repeat budget.

Retain input stock/PAK/BOM hashes, compiler/build receipts and actual SDK identity,
the application hash, source hashes, process stdout/stderr/OS samples, fifteen
PNG/JSON pairs, complete/failure records, trace.jsonl, trace.json and result.json.
Retain separate receipts for the verified shader stock and the matching OFF
engine SDK, including evidence that the shader source is unchanged between their
builds. Verified shader bytes alone do not prove which engine binary linked.

These artifacts establish a controlled rendering subset. They do not certify
all cascades, independent shadow silhouettes, performance, old hardware,
Editor Play/Bundle parity or another backend. Source implementation and CPU
validator checks are not actual GPU evidence.
