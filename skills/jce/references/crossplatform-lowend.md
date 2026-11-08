# Platform and low-end requirements

Minimum compatibility is an architecture, OS API/ABI, CPU instruction and
graphics-capability contract, not one operating-system version. Keep declared
floors, actual toolchain/dependency floors and real-device validation distinct.
A lower profile must qualify its runtime, allocator, script VMs, assets and
rendering fallback together; a compiler macro alone does not lower a cached
archive or prove that the application boots. Windows XP and older Linux/macOS
profiles remain unverified until their complete selected graphs run.

The default Windows 7+ target retains MSVC. The explicit XP legacy lane uses
LLVM-MinGW-XP GNU/MSVCRT: x86 targets NT 5.1, x64 targets NT 5.2. Both retain
C99 engine/runtime and C++20 Editor. Full XP Editor/GPU and physical low-end
qualification remains work in progress; actual core, OS and resource-probe
execution does not certify the complete application. See the repository's
[Windows target policy](../../../contracts/windows-targets.md).

Build an XP GUI candidate with `scripts/windows/build-editor.bat --xp` from the
repository root. It defaults to x64. Use `--arch x86` for NT 5.1.
`--toolchain-root PATH` overrides `JCE_XP_TOOLCHAIN_ROOT` and the pinned
distribution beside the repository. The launcher discovers shaderc and refreshes
the native PAK writer and binary embedder through the shared build driver.
The candidate is `build/desktop/windows-<arch>-xp/dist/jce_editor.exe`.
Matching owned legacy intermediates may be reused in place.
Publication uses `python scripts/jce.py xp-editor --phase audit` with the same
architecture and toolchain root and the exact guest's `--export-inventory`.

The public graphics compatibility policy appends enum value 4. Stable, modern,
current and the legacy COUNT sentinel retain values 0/1/2/3. Existing bgfx
compatibility packages keep ordinal 3. GL 2.1 / GLES 2.0 version admission is separate
from GPU quality and does not certify render targets, uniform/sampler limits,
shader linking or usable Editor features. The shader-keyword contract generates
one stock-profile catalog shared by runtime acquisition and builtin cooking;
normal full-stock behavior remains unrestricted. Compatibility GL/ES uses a
static-only subset with no cookie arrays or 3D LUT grading. Authored settings
remain retained; unsupported requested effects are diagnosed. A newer API tier
is required for the excluded shader families until real fallbacks are qualified.
Verify actual Scene/Game pixels, input and repeated lifecycle on the selected
driver. CPU ownership controls and GLSL compilation are distinct evidence;
neither establishes whole-frame performance, physical low-end acceptance or
feature parity. Vulkan compatibility policy data is not a compiled-backend
certificate.

Optional Windows thread descriptions are resolved from already-loaded
KernelBase/Kernel32 modules, retaining thread-ID labels when absent. Default
Windows desktop PhysFS builds explicitly select the Windows 7 API path with
package-identity separation; configuration changes require a new dependency
binary. Single-EXE admission examines ordinary and delayed symbol imports for
four known newer APIs. Ordinal coverage is unknown, and passing this limited
policy does not certify every Windows API or replace target execution.
The explicit XP producer separately requires exact-guest named/ordinal exports
and forwarder coverage before static delivery admission; that audit still does
not replace full Editor execution or rendered-device validation.

Consult AGENTS.md for compatibility targets and CMakePresets.json plus conan/profiles/ for actual build configurations. A target declaration is not evidence that a toolchain or device was tested.

The LOW-END design baseline is a graphical desktop, one CPU core, 512 MiB RAM
and 2008/2010-class integrated graphics without a discrete GPU. It covers usable
lightweight editor and rendered runtime paths as well as core/headless services.
PS2 is a resource-budget analogy, not a device support or equivalence claim.
Complex scenes and advanced features have explicit workload/tier budgets; they
must not silently redefine the baseline as headless-only. Record the actual
high-end development machine, and report physical low-end validation separately
from forced low settings on a modern host.

Desktop/mobile/console and x86/x64/ARM/ARM64 are compatibility goals. Default
driver languages are Java, Python, Lua, C, C++, C# and JavaScript through one
native C ABI. Compiler, licensed console toolchains, device and package evidence
remain separate from catalog entries. A missing backend or device is a gap.

Put platform-specific code behind OS adapters and public capability queries. Keep deterministic fallback behavior when hardware acceleration, a codec, a scripting runtime or an advanced rendering feature is unavailable.

Validate only the platforms the request covers. Record compiler/runtime floors, hardware/backend, memory and elapsed-time evidence. Report unavailable cross builds and devices separately; a native cached build is not a portable-source proof.

Never alter upstream code to work around a platform build. Use an approved upstream release, first-party adapter outside the upstream tree, or generated configuration supported by the dependency.

Windows native D3D11/D3D12 Auto first considers the top hardware adapter in the optional DXGI high-performance order. Its LUID must map to the classic backend's exact first vendor/device match. Missing or unsafe ranked choices use the display and driver fallback. Failed preferred initialization retires that attempt before one legacy retry.

The display fallback resolves SDL HMONITOR through GetMonitorInfoW
to its GDI name, then active Windows CCD source-name packets to the source LUID.
The pinned bgfx classic protocol (CreateDXGIFactory1 with IID_IDXGIFactory,
EnumAdapters and GetDesc) resolves the unique source LUID to a selection key.
GetDesc1 supplies only the software flag. Distinct LUIDs sharing a key remain
distinct: accept only when bgfx's first key match is the CCD candidate; refuse
later candidates as backend_key_mismatch. Do not blindly choose or deduplicate.
DXGI Output.HMONITOR is not physical display ownership on hybrid drivers
and is never queried by this production owner. Source and target LUIDs are
distinct; bgfx still receives vendor/device rather than a LUID selection input.
Two bounded path/mode snapshots and repeated window/GDI identity checks reject
observed topology changes. Ambiguous source LUIDs, malformed/ambiguous names, indirect
or unknown paths, unavailable CCD, incomplete enumeration and candidates beyond
the pinned bgfx `Caps::gpu` range keep the original fallback and explicit reason.
Four retries per snapshot, 1024 paths, 4096 modes and a 250 ms observed-time
deadline bound first-party work; an OS call cannot be interrupted by that check.
Release acquired buffers/library/COM resources once. Query per renderer creation
and reuse across backend attempts, never per frame or as device hot-migration.
Explicit vendor choices, non-D3D and headless paths keep their existing policy;
no dependency or public ABI is added. Refusal retains validated CCD source
identity/counts and a separately labeled backend-expected LUID/index prediction.
That prediction is not actual selected GPU ownership: verify native caps and
process-bound GPU LUID observations separately.

Current Windows PE policy still exports `NvOptimusEnablement=1` and
`AmdPowerXpressRequestHighPerformance=1`. Read the separate preferred render, display owner and selected device startup records. Verify Copy-engine traffic separately. The public `integrated` and `discrete` names still resolve to Intel and
NVIDIA vendor aliases respectively; topology-aware category selection remains
an open gap. Verify actual selected caps and the target PE, not the alias name.

`test_jce_window_adapter` consumes the complete private observation owner with
counted SDL/CCD/library/COM/clock/heap ports; `test_jce_renderer_display_adapter`
consumes actual init policy, both capacity boundary indices and startup source /
backend-prediction diagnostics. Wrong-LUID, former DXGI-output, old-strict-key,
blind-first-key, old-device and missing-wire controls
must compile and fail ordinary runtime assertions.
These CPU controls are not positive native GPU evidence. Match physical display,
backend, effective presentation and workload for separate native measurements;
GPU Engine counters alone do not identify PCI transfer or physical GPU routing.

Renderer startup transient budgets are 8/2 MiB for ordinary native applications
and 6/2 MiB for native low-memory and Web paths. Native multithreaded bgfx owns
two Frame pool pairs; this logical capacity is separate from backend buffers,
process working set and private commit. Large geometry/UI workloads can use
JCE_TRANSIENT_VB_MB / JCE_TRANSIENT_IB_MB (whole MiB, 1..256). The same startup
owner sets the uniform command buffer's initial capacity to 128 KiB per encoder
per bgfx Frame without reducing the 4/16 encoder policy. Backend growth remains
enabled; this is not a hard uniform-data limit. Sixteen encoders across two
native Frames have 4 MiB of logical initial capacity, a theoretical 28 MiB less
than the backend's 1 MiB default. Neither that calculation nor the startup
renderer.uniform_initial_encoder_frame_bytes counter measures total heap,
private commit, RSS or GPU residency. The actual apply owner and its same-source
missing-limit control are CPU wiring tests; require ordinary runtime assertion
failures from the control, not a crash or compile failure. Validate backend
growth, capacity exhaustion and rendered output before workload acceptance.

Static primitive vegetation uses change-driven resident roots by default on
CPU and compute paths, avoiding a repeated matrix upload for each viewport.
`JCE_PERSIST_FOLIAGE=0` retains the explicit transient diagnostic; compute
culling requires resident roots. Count/hash/compute-read identity controls reuse;
failed replacement uploads keep prior ownership without binding stale contents.
CPU matrix publication and existing authored scatter shadows require current
contents. Prepare visible, enabled shadow scatters before CSM cache admission;
placement, geometry, count and readiness must invalidate old depth. Validate
cold frames, mutations, disabled/deleted fields and bounded-cache streaming.
Incomplete submissions cannot become reusable cascades. Non-tiled model depth
must preserve node transforms and supported primitive coverage; tiled residency
and same-path external asset invalidation retain their explicit policies.
These CPU ownership controls do not replace actual dual-view GPU and
capacity-exhaustion acceptance.

Capture-only renderer snapshots distinguish requested encoder capacity from
effective capacity and frame handle usage, including the default/API slot.
Adapter vendor/device IDs and raw bgfx renderer type describe reported backend
metadata; the raw type is not the public JCE backend enum. Missing caps remain
-1, while a reported zero remains zero. These sparse CPU observations do not
prove GPU completion, every-frame peaks or instantaneous thread concurrency.

Offscreen retirement ages by actual central submission calls, including frames
where a viewport is idle. Its private ordinal is separate from the backend's
raw frame index used by readback. Process working set includes shareable mapped
pages: map region size is not resident memory. Attribute mapped resident pages
with an owned-process observation, retain whole-process totals, and never
subtract driver cache pages to declare the 512 MiB physical-device target met.

The engine resolves effective low-memory policy before publishing the default
worker budget to the existing shared frame pool. Structured async executors
consume the same budget; no second scheduler/configuration mirror is added.
LOW defaults to one worker, ordinary auto to cores-minus-one capped at eight.
Explicit worker pins keep the existing owner limits; Web retains cooperative
execution. Public machine-class/pin getters retain authored values. Validate
actual worker counts, initialization failure/restart and historical pin reset.
This budget does not cap all process/driver threads, impose CPU affinity or
turn an uncapped hidden rendering loop into measured presentation FPS.

Readback queues attach to the actual renderer backend lifetime. Their destroy
path uses the central submission owner so raw frame, submission ordinal,
offscreen retirement and instance-upload advancement cannot diverge. At most
eight progress attempts are made; no progress or exact half-range ambiguity
retains destinations until backend stop. The shared uint32 serial predicate
handles readiness across UINT32_MAX/zero; related observations must remain less
than 2^31 advances apart. Stop retires handles and detaches queues before bgfx
shutdown, then releases borrowed CPU pixels after shutdown without GPU calls.
Detached wrappers can be destroyed after restart. Transition-only trace counters
report service/queue counts and owned CPU destinations/bytes; they describe CPU
ownership, not GPU residency. Do not turn latched stats, frame FLUSH, screenshot
CPU-worker flush or D3D11 asynchronous Flush into a whole-device fence. Preserve
submitted API frames, provider-latched source frames and particular readback
readiness separately; presented/full-device completed remain unavailable without
a backend contract and physical evidence. Legacy leakstats are explicitly a
pre-shutdown backend handle snapshot, never a nonzero-equals-leak verdict.

Shared texture cooking defaults Web and portable AUTO content to RGBA8. Desktop
BC and mobile ASTC defaults remain unchanged; ASTC is an optional WebGL extension,
not a universal Web capability. Explicit per-asset format overrides remain
authored requests and must be supported by the actual target. CLI cache revision
3 and Bundle writer revision 4 invalidate prior default-format artifacts; verify
real incremental reuse/rebuild through both producers, not only the policy helper.
RGBA8 is GPU-uncompressed; Archive compression and transport size are separate
from texture residency and upload cost. Measure the rendered workload again.

The actual cooked decoder/upload owner records rejected stage, authored path hash,
cooked source hash, format, backend, capability availability and payload sizes.
CPU objects transport identity to upload; workers do not query GPU caps. Rejection
adds one warning and a correlated FAILED service span per admitted request;
existing scene/UI caches own retry admission. Direct public cooked uploads and
borrowed-memory decode may have unknown authored identity. Hashes identify trace
correlation only, not authentication. Trace-off adds no ring work, no second cache
and no per-frame diagnostic loop. CPU controls exercise the complete texture TU
with real cooked/Pak bytes and same-source missing-cap-log/identity contrasts;
they establish API behavior, not format support or pixels on a physical device.

Web sample-query compatibility uses an owned private platform C/JS boundary,
installed with the SDK and linked only into Web targets. The actual Emscripten
proc-address pointer is intercepted by a verified linker wrap; pristine upstream
remains unchanged. Legal renderbuffer SAMPLES queries keep actual browser counts;
empty lists leave the owned first output zero. Illegal requests preserve GL error
semantics without browser invalid-query calls. RGB/RGBA query aliases and enabled
float/half/norm16 extensions matter; renderbuffer storage aliases are not query
aliases. The one current-context cache clears when a query observes loss; the next
valid observation or context replacement re-observes it. VERSION/RENDERER strings are actual observations, not inferred
API-floor admission or GPU completion. Complete C and JS CPU controls plus the
real pointer probe remain separate from installed-SDK Web builds and physical
browser capability/pixel acceptance. Never solve these warnings with console
filtering, browser prototype edits, fabricated caps or upstream source changes.

Web version admission reads the actual current context through the existing
private interop cache. API/GLSL and equal begin/end epochs must be observed
before the post-init backend is accepted. WebGL's leading version is not the
GLES version: use the reported embedded OpenGL ES value, without increasing
feature caps. Unknown/missing, mixed-epoch or below-floor readings refuse with
existing cleanup and clear verified flags. Public API info/backend names observe
epoch invalidation; restoration/replacement requires renderer recreation. This
metadata safeguard is consumed by the actual private frame gate. Begin/View/End
report real refusal to the CPU trace adapter; central submission refuses without
calling bgfx_frame or advancing submission/retirement/instance ordinals. The
public void ABI remains unchanged. Engine entry/event/watch and post-callback
checks terminate lost-context work with the existing application failure/cleanup
path, independently of minimization and background CPU permission. This stops
new owned frame work after observed loss; teardown may still retire resources.
Native unavailable-version policy remains unchanged. Automatic recovery,
resource reconstruction and GPU completion require independent evidence. CPU provider/JS controls are
separate from actual Web context-loss/rebuild and rendered acceptance.

The current CPU baseline passes 11 API-admission cases and 12 sample-query
cases. The four complete-owner version negatives report 5/5/1/3 Unity failures
(namespace/publish/epoch/init wire); the two query negatives report 5/7
(uninitialized output/query gate). CTest accepts these ordinary assertion
failures through WILL_FAIL, not crashes or build failures. The complete JS owner
passes its CPU ports; its three negatives each terminate at one explicit Node
AssertionError (error propagation/context reset/epoch publication). That JS
harness reports no case total. These counts do not certify real browser
context loss, resource reconstruction or presentation.

```bash
python scripts/jce.py test --suite engine --filter "^test_jce_(web_api_admission(_old_namespace|_missing_publish|_old_epoch_carry|_missing_init_wire)?|webgl_interop_js(_missing_error|_missing_context_reset|_missing_epoch)?)$"
```
