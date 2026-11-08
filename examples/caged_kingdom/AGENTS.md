# Caged Kingdom consumer contract

CK is the production game consumer at examples/caged_kingdom. This checkout
contains build metadata; its implementation sources are currently absent by
owner choice. Do not invent a working build or restore obsolete code silently.

## Design authority

The owner's local frozen design baseline is
way/demo_complete/CK_DESIGN_FREEZE_v1.1_FINAL/README_BILINGUAL.md.
Its declared order is decision log, vision and pillars, domain authority,
story/content architecture, then production slice or implementation note.
The bilingual editions are semantic mirrors. Frozen design, production targets
and validation-required ideas are distinct from implemented gameplay evidence.

The lifecycle execution pack under way/demo_complete is a consumer workflow
reference, not authorization to change generic engine internals. Its source
snapshots are references, not another editable design authority. Other concept,
audio and devlog materials retain their declared reference status.

The owner requested that all way/ frozen material remain local and untracked
for this release. Preserve original files and manifests. A public clone must
not depend on those local materials for configuring the engine or its tests.
The historical root design filenames and removed src/resources layout are no
longer the current authority or an available implementation.

## Implementation and delivery

The optional JNI shared-library metadata force-loads its Application host on
macOS so JVM-discovered exports survive static archive selection; Windows/Linux
already retain that archive. The Core config TU is not duplicated in Application.
This metadata is not proof of a build for the absent consumer implementation.

- Use public installed JCE SDK headers and the shared JceAppDesc/runtime loop.
- Keep game rules, scripts, assets, generators and project-only tools here.
- Never include engine/src, third-party, SDL, bgfx or editor implementation.
- Use engine allocation, filesystem, input and rendering services.
- The final application is one architecture-matched executable with embedded
  resources. A transferable content Bundle is one .jbundle file. Neither may
  require a loose asset package, external language VM or source checkout.
- Validate editor simulation, packaged Bundle reopening and relocated platform
  execution with actual gameplay/scene evidence. Build metadata alone is not
  proof of any platform, resource budget or game feature.
- Report a generic SDK capability gap separately from consumer implementation.
  Frozen CANON changes require explicit owner decisions; do not infer them from
  a prototype failure or implementation shortcut.
