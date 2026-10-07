# tools/ — reusable automation

Automated build, asset generation, analysis and validation implementations live
here. They take explicit inputs, derive repository paths from their own location,
produce outputs under ignored build/dist/reports directories and carry no secrets.

- build/: build driver, installed-SDK acceptance and pristine source acquisition.
- lint/ and audit/: source, ABI, dependency, format and policy gates.
- scriptgen/ and shadergen/: generators from maintained contracts.
- provenance/: generic content hashes and externally supplied signatures.
- benchmark_suite.py / benchmark_scene.py: separate guarded Core-5 editor/Player runs and
  deterministic fixtures. process_sampler.py owns engine-neutral OS sampling;
  perf_bench.py owns process/backend/log validation and derives native artifact
  paths from the build driver's target matrix. See BENCHMARK.md. The SDK-only
  Player host and its scenario/capture policy belong to tests/consumers/benchmark.
  Optional named live markers record readiness while the child is running;
  stale files and exit-only observations cannot become startup latency.
- benchmark_report.py: explicit suite inputs, raw-CSV revalidation, separate
  identity groups, static plot export and public-reference coverage. References
  are labelled separately from measurements and cannot become timing bars.
  benchmark_probe_report.py validates scoped CPU workloads from integer clocks
  and completed work. CPU batches retain their scope and never become frames.
- render/media/asset CLIs: general measurements, cooking and inspection.
  media_preview_probe.py runs real hidden-window editor playback under an
  explicit JCE_EDITOR_PROFILE_DIR. Its default fresh profile lives beside the
  output; --profile supplies an intentional reusable profile. It never snapshots
  or restores the user's HOME settings and requires actual profile confirmation.
  Playback cadence intervals end at actual seek-generation events or paused
  snapshots. Clock-delta heuristics must not join seek waits or split away slow
  playback. Keep failed controls and report short unmeasured intervals honestly.

A dedicated game's generator, palette, fixture or packaging default belongs to
its consumer project. Reproduction tools and engine regression fixtures remain
under tests/consumers/<project>/ or the corresponding native test module.
Unpublished AI workflows live under private/tools and are optional; reusable
SDK/editor capabilities never depend on their presence. No automated recipe
may depend on scratch docs, a user's home configuration or a copied vendor tree.

SDK binary embedding uses the existing jce_bin2obj host tool for both PAKs
and bundles; the packer only produces archives. JCESDKHelpers activates ASM
at directory scope for native non-MSVC consumers, including prebuilt assembly.
The wrapper owns COFF C-symbol decoration and preprocessed assembly target
sections, symbol prefixes and size_t width. Recipe mocks do not certify a
platform linker; retained native SDK-consumer evidence is still required.

Shader compilation keeps each native tree's generated output isolated.
compile_shaders.cmake routes through shader_cache.py when its existing Python
interpreter is available; otherwise the direct compiler path remains. The cache
uses the shared build/cache_lock.py kernel lock and verifies each blob checksum.
Keys include compiler bytes, complete arguments except the output destination,
and declared source/varying/include bytes. Compiler failure or changed inputs
must preserve the previous local output. Never seed it from unverified old
shader binaries or copy native build trees; JCE_SHADER_CACHE_DIR selects an
explicit alternative cache location.

The C99 jce_bundle_pack CLI only forwards public JceBundlePackOptions to the
existing resource packer. Explicit cooking warnings are failures: raw fallback
is never claimed as successful cooking. Missing selected scenes fail early.

Private tools/cli/jce_tool_main_sdl.c is the sanctioned CLI entry bridge. Only
this tool bridge includes SDL_main; consumers implement SDL-free jce_tool_main.
It does not initialize SDL subsystems, renderer, engine or an application loop.
bundle/pak/cook/bin2obj share the UTF-8 entry. Legacy host path streams use
private jce_tool_io_sdl.c, preserving malloc buffer ownership and bounded archive
inspection. Directory enumeration and atomic replacement reuse public host FS;
unchanged packed bytes retain their mtimes. No platform path API appears in tools.
The object target is linked into the host binary, not an installed public API.

Host asset tools share cmake/JCEHostAssetSources.cmake offline implementations.
The bundle CLI must not link renderer/scene/resource layer targets merely to
reach the public packer; retain real cooking and VFS behavior without shaders.

Web SDK archive embedding uses --embed-file plus SINGLE_FILE so code and MEMFS assets live in one HTML executable. Verify deployment with only that artifact; preloaded companion data is not single-file delivery.

SDK embedding packs authored .jce/project-settings.json under settings/project-settings.json beside the runtime boot contract. Configure dependencies refresh edits, and absence removes stale generated settings; host working-directory settings are not a shipping dependency.
