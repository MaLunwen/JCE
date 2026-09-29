# elemental_serenity - JCE dogfood application

## Identity

- L7 consumer and reference application for single-executable deployment.
- C99 application code; assets and UI are data.
- The only engine include surface is `<jce/api.h>` or focused public
  `<jce/api_*.h>` umbrellas. Never include `engine/src/**` or third-party
  headers.

## File Map

| Path | Role |
|---|---|
| `src/main.c` | Application lifecycle, scene/runtime setup, input, rendering, and autoshoot verification path. |
| `src/es_scene_orchestrator.*` | Product policy over the generic AI director/coordinator: user-triggered next state, bundled recipe selection, root/primary role dispatch, status, and fallback. |
| `resources/assets/scenes/` | Authoring source scene consumed by editor/cook. |
| `resources/assets/scene_ai/` | Closed capability catalog and bundled-AI SceneRecipe v2 fixtures. |
| `resources/assets/scripts/` | Scene-bound runtime behavior; paths must be covered by bundle dependency metadata. |
| `ui/` | RmlUI HUD source. |

## Rules

1. Keep game-specific season, weather, camera, and progression policy here;
   never move it into engine scene/runtime modules.
2. AI output selects semantic capabilities only. It must not inject paths,
   scripts, components, shaders, or commands.
3. Every bootstrap recipe uses the exact public schema/compiler version and
   preserves the `environment.root -> environment.primary` hierarchy contract.
4. The orchestrator consumes stable roles and FrozenPlan identities, not
   entity display names or operation array positions.
5. The runtime must work from an embedded PAK after the EXE is copied alone to
   an unrelated directory. Loose-file fallback is not release correctness.
6. Assets remain redistributable under their recorded licenses; do not add an
   asset without provenance and bundle closure.
7. Build through `scripts/jce.py app examples/elemental_serenity`; do not add project
   build scripts outside `scripts/`.

## Verification

- Focused director/coordinator/transaction tests must pass.
- Build against a clean installed SDK.
- Copy only the final EXE into an empty directory and run the autoshoot probe.
- Confirm logs report `pak=yes loose=no`, a verified activated plan, clean
  shutdown, and expected screenshots with grass, water, lighting, and weather.

## Source and ownership

See SOURCE_AND_TERMS.md. Project-specific authoring tools and palette inputs
live in tools/. Build output stays in build/. External upstream source remains
read only. Runtime orchestration consumes public SDK APIs; retired internal
transport experiments are preserved only in the ignored local archive.
