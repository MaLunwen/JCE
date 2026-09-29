# JCE repository contract

This is the single engineering rule source. User instructions take precedence.
Read this file, README.md, the affected module's AGENTS.md and the relevant
public API before editing. The maintained module map is
[contracts/module-memory-index.md](contracts/module-memory-index.md).
Use the repository [JCE skill](skills/jce/SKILL.md) for task-specific procedures.
Local scratch documents and private extensions are not required by a clone.

## §0 Architecture and language

- Engine: C99. Editor: C++20. C++ dependencies stay behind sanctioned C bridges.
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
- scripts/: manual entry points, native launchers and shell bootstrap helpers.
  scripts/jce.py forwards to the build implementation without a second recipe.
- examples/<project>/: consumer code, scenes, balance, assets and dedicated tools.
  A project-only generator or packaging default belongs to that project.
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

## §11 Change and verification invariants

1. Search public headers, implementation and adjacent modules before adding code.
2. Keep public headers self-contained C99; add APIs to their proper umbrella.
3. Preserve LF for owned text; BAT/CMD use CRLF on disk and LF in Git. Preserve
   binary and original upstream bytes. .editorconfig and .gitattributes agree.
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

## §2 Platform goals

Compatibility comes first: desktop Windows 7+, macOS 10.13+, glibc 2.28+;
Android 21+, iOS 12+ and wasm32 are capability targets. A particular build recipe
may have a higher toolchain/runtime floor; report that limitation accurately.
Design the default path for one core, 512 MiB RAM and integrated graphics.

## §3 Layering

OS primitives and compatibility are the bottom layer; renderer/resource build
on them; middleware composes reusable subsystems; runtime/application own
lifecycle; editor, scripting and examples consume those public interfaces.
Use the corresponding API umbrella before internal code. Public UI strings
use the i18n system; numeric paths have deterministic fallback semantics.
