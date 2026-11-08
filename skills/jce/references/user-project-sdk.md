# SDK consumers and distribution

An unsealed stock runtime is a producer component, not an application shipment.
Keep its fixed consumer build cache under build/ and its architecture-specific
runtime package separate from the final dist/apps executable. Compilerless
Lua/JavaScript sealing must inspect the typed Bundle contract, reject native or
external-VM requirements, and use only verified packaged content. The existing
normal application builder still requires a valid startup scene; never add a
fake scene to make an unsealed runtime pass that check. Hash/corruption checks
are not authentication. Build success and a seal receipt do not certify actual
runtime execution, relocation or a zero-environment C/C++ toolchain.

Use jce_fs_host_get_executable_path for the actual executable image path rather
than argv[0] or the working directory. Unsupported platforms return false;
Windows/Linux implementation alone does not establish other-platform support.

Stock payload version2 wraps the exact inspected Bundle in an existing public
secure PAK with one stock/project.jbundle entry. Enforce encrypted/authenticated
flags, successful authentication, uniqueness, budgets and exact source parity;
the envelope's FNV remains corruption detection only. The native reader keeps
decrypted bytes alive through all runtime/Bundle borrowers. The explicit producer
target runs eight CPU controls; source fixtures and actual GUI execution remain
separate gates. Sealing currently requires --pak-key-file bound to the producer's
owned-cache path/hash. Raw keys are not copied into the published component or
application directory; this is not zero-configuration builder distribution.

Consumers include public JCE headers and link the matching installed SDK. They do not include engine/src/ or direct vendor headers. Search public APIs and component configuration before introducing an engine feature.

```bash
python scripts/jce.py sdk --arch x64 --variant dist --no-debug
python scripts/jce.py smoke --arch x64 --variant dist
python scripts/jce.py package editor --arch x64 --variant dist
python scripts/jce.py package editor --arch x64 --variant dist --standalone
```

Controlled native embedded SDK builds can use an isolated cache/install pair:

```bash
python scripts/jce.py sdk --arch x64 --variant dist --build-dir build/fixtures/sdk-without-stock --install-dir dist/sdk/fixture-without-stock --embed-engine-shaders off
```

This explicit OFF SDK has no embedded stock shader capability; it is useful for
asset admission/recovery controls only when the consumer supplies its required
shader bytes. Default SDK production keeps stock embedding ON. Paired custom
paths stay below build/ and dist/sdk/, outside all shared/default caches and
prefixes; `--clean`, authoring/Web/cross selection and path/owner/cache/installed
metadata conflicts are refused before producer mutation. Matching failed
configure requests can be retried. A request stamp is not native runtime proof.

For a project, use supported cook/app/package/accept subcommands with an explicit project directory. Keep its CMake, manifest, icons and dedicated tooling with the project. Read each subcommand's --help before choosing options.

Public-header changes require refreshing the requested SDKs and rebuilding their consumers. Installed headers are build output: never repair them manually. Match the library, headers, scripting adapters, graphics tier and variant from one configuration.

An installed SDK and an editor that launches without extra runtimes do not provide a zero-environment compiler. See [current toolchain discovery and complete payload requirements](toolchain-reference.md); target/CRT admission does not certify bundled build tools.

Dist selects a Release build without Tracy or patented codec paths. Release and dist are distinct products; an old executable cannot be relabeled as dist. Inspect the packaged binary and runtime dependencies, not just the selected preset name.

Asset name confidentiality is optional in the same `jce_project.json` manifest:

```json
{
  "confidential_asset_paths": ["materials/player.mat.json", "scripts/player.lua"]
}
```

Each declared address must already be a canonical relative member of the selected
Bundle/Dist graph. The private Dist boundary uses public host IO/JSON, with a
1 MiB manifest cap, at most 256 addresses, 1023 bytes per address and 64 KiB for
all addresses including their terminators. It rejects duplicate keys/addresses,
non-string or empty items, NUL, wildcard/escape and noncanonical spelling; it does
not expand patterns or silently select unrelated files. Omission or an empty array
declares no confidential names. Project Dist PAK payload encryption, authentication,
deep verification and graph parity remain mandatory regardless of that node;
GUI and single-file checks also remain. An explicit protected-name literal outside
the project PAK fails the audit. Public string addressing remains supported:
ordinary selected address literals are separate observations and do not prove
plaintext payload leakage or runtime use. The public project saver preserves this
original optional node on unrelated edits without a new ABI field or another path
schema. It refuses raw/active encoded NUL, duplicate policy keys and non-JSON tails
without rewriting the original file; escaped backslash text is preserved.

Bundle and Dist compose the same existing public Bundle roots/resolver for present
`render_settings.json`, `physics_layers.json`, `settings/project-settings.json`,
`settings/render_pipeline.rp.json` and
`settings/input_actions.json`. These selected roots carry their descriptor/LUT
closure; absent optional configuration is legal. Public VFS -> canonical source ->
resolver precedence remains. Prepared canonical fallbacks precede project
Settings/.jce fallbacks; source bytes retain precedence over prepared bytes. The
Dist manager passes its actual prepared root and avoids separate duplicate input
entries. Verify the actual embedded configuration bytes and dependent assets in
the produced EXE and Bundle, reopen the Bundle in the editor, and exercise the
relocated application. Configuration declarations and CPU controls cannot certify
executed render/input behavior or current release acceptance.

DIST engine stock shader PAK generation strips its debug path table with
`--strip-debug-paths` while keeping hash addressing. The stock archive remains
public; stripping names does not encrypt its payload or imply a minimal closure.
Record actual rebuilt PAK/EXE/Bundle hashes and inventory separately from the source
policy, SDK size and retained build evidence.

The shipping default is a compact, self-contained native application: embedded Lua and JavaScript plus native C/C++ application code. Python, Java and C# remain optional authoring/SDK integrations with explicitly documented external runtimes. Do not add these VMs or SDK payloads to the shipping executable merely to claim seven embedded languages. Required project scripts must resolve on the target host; reject a missing driver or module instead of silently discarding behavior.

Acceptance includes only the delivered application in a relocated empty directory, clean user state, no source-tree asset fallback and inspected representative captures. Record executable hash, source identity, variant, platform and test limits. Verify the actual embedded assets/notices and ordinary/delayed imports. This Windows evidence does not establish other platforms.

For the current Windows native single-EXE recipe, `--standalone` selects the static-CRT graph and embedded assets with Lua/JavaScript/native ABI support. Native C/C++ ABI support alone does not prove that a project's script classes are statically registered. A `.jcec` or `.jcecpp` path names a class; putting a descriptor or marker file into a content bundle does not create that class registry. Preserve optional native-module developer tests separately, and explicitly validate the actual static registration or use a behavior-preserving supported portable script variant. Shipping must not depend on a sibling project DLL, external VM, SDK installation or extraction wrapper.

Runtime models are canonical glTF/GLB. Convert and validate supported editor source-format imports and all dependent assets before cooking or packaging. Project policy and tools stay with the consumer; do not insert game-specific rules into the generic engine or editor.

Single-executable delivery is the engine invariant for editor and consumers.
A Web shipment is one HTML containing its wasm/bootstrap/assets. Cross-target
probes cannot consume host JNI headers. Static Windows SDK production reuses
the editor dependency closure; the SDK CRT is part of its installed ABI.
Check ordinary/delayed imports and relocate only the application file into an
empty directory. Runtime-created settings are not shipment dependencies.
Do not certify a native-script admission profile using only source declarations;
retain actual compiled controller behavior and artifact identity.

## Build and delivery directory ownership

Keep intermediate objects, generated code, link outputs and validation evidence under build/ (or reports/ for shared reports). SDK installation lives under dist/sdk; it is not an application shipment. Stage final editor applications under dist/editor and consumer applications under an explicit dist/apps/<project>/<platform-architecture-variant>/ directory. The application directory contains only its required platform executable/container; native single-file and Web single-HTML shipments contain only that file. Keep hashes, inventories, logs and captures outside it. Packaging is a separate step from compilation; never point a final output at a build cache or recursively copy a link directory. Honor supported --out/--artifact-dir options after reading their help. Document any explicit staging step and validate the exact staged bytes by empty-directory relocation. Runtime-created caches/settings are recorded separately. Concurrent maintainers update the affected en/zh skill references alongside implementation changes and coordinate ownership before editing shared sections.


Bundle delivery uses one `.jbundle` file containing its selected scene, scripts and asset closure. It is a content package, not an engine/SDK distribution or independently executable application. The app builder forwards the manifest `bundles` list to the installed SDK embedding helpers; relative paths resolve against the project and list order is preserved. A stock project template can therefore embed a Bundle into the final single executable. Verify both editor reopen and an EXE-only relocated directory; pack/build success alone does not certify gameplay or every target platform.

Windows DIST applications default to the GUI subsystem and must not create a console window. Apply target policy before prebuilt PAK/bundle early returns; embedding must not bypass it. Validate the produced PE subsystem (2=GUI), not just CMake declarations. CLI development tools retain their explicit console policy.

Embedded delivery resolves typed shader program stems to actual compiled backend files. Use the installed SDK stock catalog verified from its embedded shader pack for built-in dependencies; authored project files retain precedence. Missing user programs still fail admission. An available variant is evidence for that backend, not qualification of every target platform.

Static C/C++ scene scripts use `jce_script_enable(... NATIVE_FACTORIES provider)`.
Declare `native_script_factories` and `native_script_refs` in the project manifest;
refs bind canonical selected addresses to original class-reference spelling. The
generated SDK registry links providers and checks real class resolution. SDK CLI
shipment requires an actual receipt from the relocated executable. Web builds
record `build/native-registry-proof/web-request.json` and remain unadmitted until
actual browser execution supplies the matching `--registry-proof` receipt.

Tower Defense regression source and dedicated verification tools are maintained
in `tests/consumers/td_lab`; `examples/td_lab` is its Bundle catalog with source
license/provenance. Generated caches/cooked data remain ignored and final
GUI EXE/single HTML live under `dist/apps/td_lab` separately. Migration never
rewrites historical artifact/capture identities as final-source evidence.

Native build-project and compatibility package game both delegate to the editor
headless build/package owner. Their default final layout is the project-owned
dist/apps/<app>/<platform>-<arch>-<variant>, separate from build caches/results.
Before launching the owner, resolve and reject protected source/build/SDK paths
and unknown existing output contents; the native owner rechecks publication.
Never recursively remove an arbitrary output or migrate older dist/games output.
Legacy loose-file delivery and mismatched name/version overrides are rejected
explicitly instead of creating an alternate multi-file publisher.

Windows schema-1 SDK archives require CMake MSVC-compatible frontend mode in both
producer and consumer admission. This permits cl/clang-cl and preserves the
existing target/configuration/CRT checks. GNU/LLVM-MinGW mode fails explicitly
with SDK_ABI_MISMATCH; matching architecture alone does not make the current
MSVC archive usable. Such a toolchain needs a separately produced compatible SDK.
Compiler-free identity-model controls are not compiler/link/runtime evidence.

Completed native and Web SDK producers publish `jce-sdk-product.json` under the
existing SDK lease after installation, pruning, required host tools and metadata.
`tools/build/sdk_product.py` binds the generated version/API, schema-1 target,
language profile and actual install roster to canonical relative file sizes and
streaming SHA-256; it rejects missing files, links, escapes and observed changes.
`verify_product` is read-only and works after relocation. The atomic, unsigned
file-identity manifest excludes itself; an older manifest retained after failure
is valid only if current bytes still verify. `build_payload_available`,
`compiled_abi_verified` and `execution_verified` remain false. This SDK identity
does not supply a compiler/sysroot/CRT payload or enable zero-environment builds.
CPU controlled-port tests are separate from actual compilation and execution.

The Bundle Browser defaults to portable content (Auto). Enable cooking when
a specific target's texture or other cooked representation is needed. Open
uses the current editor platform. The inspection target is an independent
compatibility check. Open Bundle and Open Scene route standalone .jbundle
files through the isolated Bundle loader. Browse uses the host's native
file picker and opens the selected standalone Bundle. In the Open tab, paste
an absolute path, including a quoted path copied from Explorer, and press
Enter or choose Open.

Bundle Browser's Open tab offers explicit package or saved-content inspection
for an exact selected target, independently of loading a scene in the host
runtime. Package inspection keeps the public loader's available-sidecar checks.
Snapshot inspection captures at most 64 MiB once and reuses owned bytes; it
checks that content, not later disk sidecars. Both use the existing async owner
and leave the current scene/VFS unchanged. Source controls and GUI reachability
are separate: actual Editor interaction still requires a rebuilt application.

An explicit build-only SDK staging prefix is available through `sdk` or
`package sdk --staging-dir <repository-build/sdk/owned-prefix>`. It reuses the
compatible canonical native embedded cache while preserving default dist output.
Use the selected verified prefix explicitly in the consumer build. Staging does
not embed a compiler/sysroot or establish zero-environment Editor builds; see
build-and-gate for admission and retained partial-install behavior.

The public host tool can read one selected asset without unpacking a project:
`jce_pak --inspect <archive> --read-entry <logical-path> --out <fresh-file>`
uses the existing archive target/key admission, CRC verification and exclusive
output creation. The selected decoded output is limited to16 MiB; this is not
a whole-archive memory limit. It rejects unsafe/missing/ambiguous entries,
overwrite and unsupported zero-length codec/crypto layouts. Plain empty data
uses mapped-entry admission. Do not interpret `jce_archive_read` returning zero
as success; the legacy API also returns zero on errors. This inspection does
not mount a scene, change assets or certify Editor/runtime consumption.

Bundle provenance should bind the selected authoring scene, scripts, pipeline
settings and staging recipe by byte hashes. Inspect the actual selected entry
when delivery differs from authoring; a package hash alone does not show which
source configuration it contains. Verify reopened Bundle and relocated program
consumption separately. Keep prior failures and distinguish manual exit from
automatic test-guard exit. Authored clear policy and anti-aliasing changes are
content decisions, not evidence of a same-quality performance gain.
