# JCE repository contract

This is the single engineering rule source. User instructions take precedence.
Read this file, README.md, the affected module's AGENTS.md and the relevant
public API before editing. The maintained module map is
[contracts/module-memory-index.md](contracts/module-memory-index.md).
Use the repository [JCE skill](skills/jce/SKILL.md) for task-specific procedures.
Local scratch documents and private extensions are not required by a clone.

## §0 Architecture and language

- Engine and runtime: C99. Editor: C++20. C++ dependencies stay behind
  sanctioned C bridges; their types never cross the public native C ABI.
- Consumers use public <jce/api*.h> SDK interfaces. No engine/src or third-party
  includes in games, editor panels or language consumers.
- Low layers never depend on higher layers. Platform code belongs to
  engine/src/os/platform; use JCE_PLATFORM_* outside platform adapters.
- Reuse existing APIs and implementations. One implementation owns each service.
- Use jce_math, PhysFS wrappers, enkiTS, mimalloc and the approved dependencies
  in conanfile.py and THIRD_PARTY_LICENSES.md. New dependencies need owner approval.
- Engine allocation, filesystem and threading use their jce_* wrappers.
- No game logic or project names in reusable engine/editor behavior.
- Soft file cap: 2000 lines; hard cap: 3000, enforced by tools/lint/check_file_size.py.
  Split at a responsibility boundary rather than raising a baseline.

## §9 Repository ownership

- engine/: reusable implementation and public SDK; editor/: SDK consumer UI.
- tools/: automated build, analysis, generation, validation and asset tooling.
  tools/build/jce.py owns build orchestration; tools/lint and tools/audit own gates.
- scripts/: supported build entry points, native launchers and shell bootstrap
  helpers. scripts/jce.py forwards to tools/build/jce.py without a second recipe.
  Users invoke these scripts; dependency/compiler command construction belongs
  to the shared implementation, not hand-written per-machine commands.
- examples/<project>/: single-file Bundle catalogs, source/terms and provenance.
  Caged Kingdom retains its game-project source. Consumer regression projects,
  dedicated fixtures and their tools belong under tests/consumers/<project>/.
  A project-only generator or packaging default stays with that consumer.
- contracts/: maintained machine-readable authorities and stable interface specs.
  No dated plans, conversational notes or local acceptance logs here.
- docs/ and .docs/: local content, delivery notes and scratch; not published.
  Other dot directories are local except the explicit CI/configuration allowlist.
- private/: unpublished AI workflows, their specs, contracts and own tests.
  Public SDK/editor mechanisms and ordinary scene/physics capabilities stay public.
  The public core must configure and validate without this optional directory.
- skills/jce/: default en-US public skill; skills/jce-zh-cn/: corresponding translation.
  Local runtime pointers resolve to these versioned directories.
- build/, dist/, reports/: generated local output, never source authorities.

Original third-party code is fetched from immutable pins, verified byte-for-byte
and kept outside the source tree. Never patch, format, move or repair an upstream
checkout or Conan package. First-party ports live outside upstream trees.

## Application lifecycle authority

[contracts/application-lifecycle.md](contracts/application-lifecycle.md) defines
optional callback dispatch, listener mutation, Play failure recovery and trace
completeness. Reuse the existing runtime, history and bounded trace authorities;
CPU submission records never substitute for GPU execution or presentation.

## §11 Change and verification invariants

1. Search public headers, implementation and adjacent modules before adding code.
2. Keep public headers self-contained C99; add APIs to their proper umbrella.
3. Preserve LF for owned text; BAT/CMD use CRLF on disk and LF in Git. Preserve
   binary and original upstream bytes. Use .gitattributes for Git line endings.
4. Update affected AGENTS.md and the module map when ownership or paths change.
5. Run python scripts/jce.py lint (both lint and architecture suites) and the
   appropriate python scripts/jce.py test checks. A missing/private suite is
   unavailable, never a PASS; report it separately from public-core validation.
6. Public-header edits require python scripts/jce.py sdk and consumer smoke tests.
   Binding changes require both script generators in check mode.
7. Pixel claims need captures and controls; timing claims need wall-clock evidence.
8. Verify staged paths and the real index/ignore policy; do not infer tracking
   from a pathname search. New gates need a negative control.
9. Respect the user's Git workflow. Do not commit or push when they reserve those
   actions. Preserve unrelated edits, ignored assets and source backups.
   Each push requires explicit user authorization for that push; previous
   publication permission does not authorize later pushes. Keep one release
   commit per version on main; consolidate same-version fixes locally rather
   than creating duplicate release subjects. Local history cleanup does not
   authorize rewriting the remote.
   Commit only after the full requested implementation, testing, optimization
   and defect fixes are complete. Do not turn partial acceptance into a release.

## §2 Product and compatibility goals

- Target desktop, mobile and console platforms and x86, x64, ARM and ARM64.
  Native callers in other languages use the same flat C ABI. Default shipped
  applications prioritize embedded Lua/JavaScript and native C/C++. Python,
  Java and C# remain optional authoring/SDK backends when their runtimes cannot
  be embedded compactly. Shipping must not require an external language VM.
  Reject a project requiring an unavailable backend before packaging; never
  silently omit gameplay scripts. A declared architecture, language or preset
  is a goal/catalog entry, not proof that it runs.
- Compatibility comes first: Windows XP+ is an explicit legacy target goal
  through LLVM-MinGW-XP GNU/MSVCRT (x86 NT 5.1, x64 NT 5.2); the default desktop
  lane remains Windows 7+ with MSVC. Both keep C99 engine/runtime and C++20
  Editor. Full XP Editor/GPU acceptance is work in progress; core, OS and
  resource probes do not certify it. See [Windows targets](contracts/windows-targets.md).
  Other floors remain macOS 10.13+, glibc 2.28+;
  Android 21+, iOS 12+ and wasm32 are capability targets. Console builds need
  their licensed toolchains and device validation. Record actual compiler,
  OS/runtime and graphics floors; do not silently turn a recipe's higher floor
  into a universal compatibility claim.
- The LOW-END design baseline is a graphical desktop, one CPU core, 512 MiB RAM
  and older integrated graphics with no discrete GPU, including 2008/2010-class
  devices. It applies to the lightweight editor and rendered runtime as well
  as core services. It is not a headless-only exemption. PS2 is a resource-budget
  analogy, not a claim to run the desktop binary on a PS2 or match its hardware.
- Keep a deterministic usable low-end path and capability-based fallbacks.
  Advanced graphics and large projects scale by explicit workload/tier budgets.
  Measure UI, startup, CPU/GPU frames, peak/resident memory and assets separately.
  Forced low settings on a modern host do not certify a physical old device.
  The high-end benchmark records the actual development machine configuration.
- Runtime models are glTF/GLB. Common authoring model formats belong to editor
  import/offline conversion; cooking must produce valid canonical glTF/GLB and
  verify dependent assets before runtime packaging. Do not introduce competing
  runtime import paths or dependencies into consumer applications.
- Single executable delivery is a cross-platform invariant for the editor and
  user applications, independent of target architecture or directory layout.
  Web delivery is one HTML executable with its wasm, bootstrap and assets inline.
  Companion JS/wasm/data files, project DLLs or a self-extracting launcher do not
  satisfy this invariant. OS/browser services are the platform runtime. Verify
  relocation into an otherwise empty directory; a build recipe alone is no proof.
- Editor and user applications target compact, feature-complete native single
  applications, relocatable for their declared platform/architecture. Statically
  link shipped driver runtimes where supported, embed required assets/notices,
  and measure contents and size. An embedded SDK, archive extraction launcher,
  companion runtime bundle or silently omitted language is not completion of
  this goal. OS-required application containers remain platform adapters.
  Report the current supported language/package subset and remaining gaps.
- Compare functionality, rendering, lighting, audio, particles, physics,
  performance, debugging and generality against Unity, Unreal and Godot using
  scoped evidence. Modern techniques must keep supported fallbacks and shared
  implementations. Public reference results are not local comparative timings.
- Use The Forge as an architectural reference for shared systems, consumers
  and tools, adapted to JCE's C ABI and existing owners:
  https://github.com/ConfettiFX/The-Forge. Do not copy its platform code, naming
  or minimum requirements into JCE without analysis and authorization.

## §4 Consumer lifecycle and build discipline

AI CLI is a development-time application consumer; AI runtime is application
behavior during play. AI CLI uses only public API/SDK, documentation, repository
Skill, examples, editor operations and supported build tools. It does not read
or modify private engine/editor implementation to bypass a missing consumer
capability. Report a reusable API/editor gap; any engine maintenance is a
separately scoped task with its own verification. The generic model transport
can connect hosted or local model APIs without embedding private prompts,
credentials or unpublished patent-oriented policy in public source.

Prefer scene/component values and the supported driver language over duplicating
engine behavior in an application. Validate the development loop repeatedly in
editor simulation, the corresponding platform executable, and a packaged bundle
reopened in the editor. Check inputs, lifecycle, resources, script behavior and
render output for each host; compilation or a successful API response is not
application acceptance. Editor Game View and runtime share the same services.

Use scripts/jce.py and maintained platform launchers for builds, tests and gates.
Reusable implementations take explicit target/project/toolchain inputs and honor
cache and job budgets. Reuse a compatible build tree; do not clone a full source
and build tree per trial. Isolate only configurations that require it (such as
Debug CRT, architecture or static linkage), and record disk headroom before
large builds. Preserve dependency originals and referenced raw evidence.

Generated test applications/fixtures live under ignored build directories;
unit-test source remains under tests/. Captures and reports are ignored output.
Do not open test windows unless necessary; GPU validations use hidden/nonfocused
windows and must not steal input or foreground other applications. Formal timing
runs exclude concurrent builds, gates, plotting and other benchmarks.

Owned source and comments use readable US English and ASCII where practical;
localized product text belongs to i18n resources. Follow the repository format
configuration and readable Linux/Luanti-style conventions. Referenced local
style PDFs and architecture images are optional inputs, not clone prerequisites.
LF/CRLF and untouched upstream-byte rules in §11 remain authoritative.

## §3 Layering

OS primitives and compatibility are the bottom layer; renderer/resource build
on them; middleware composes reusable subsystems; runtime/application own
lifecycle; editor, scripting and examples consume those public interfaces.
Use the corresponding API umbrella before internal code. Public UI strings
use the i18n system; numeric paths have deterministic fallback semantics.
