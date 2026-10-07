# Source, terms and project contract

Verified 2026-09-28.

| Item | Source / terms |
| --- | --- |
| Original diorama | [SahilK-027/Elemental-Serenity](https://github.com/SahilK-027/Elemental-Serenity) |
| Original demonstration | [elemental-serenity.vercel.app](https://elemental-serenity.vercel.app/) |
| Upstream license | [MIT, copyright 2026 Sahil K](https://github.com/SahilK-027/Elemental-Serenity/blob/main/LICENSE); [preserved notice](UPSTREAM_NOTICE.md) |
| JCE adaptation | [MaLunwen/JCE](https://github.com/MaLunwen/JCE), this examples directory; repository source terms: [MPL-2.0](../../LICENSE) |
| Previous local consumer | Preserved original consumer; moved into this examples project |
| Supplied reference snapshot | Original revision and Git metadata unavailable; byte-level asset matches are recorded separately |

This directory contains JCE scene data, lifecycle code and product policy. It
is not the original Three.js source tree. A build does not establish visual
parity. External source and dependencies are neither edited nor tracked here.

Keep the MIT copyright and permission notice with distributions containing
upstream material. Asset-specific notices take precedence. Do not infer rights
for music, models, textures or fonts solely from a repository license. Existing
imported assets and fonts remain locally preserved under Git ignore; publishing
them requires per-asset provenance. JCE scene/scripts, extracted palette inputs
and procedural content remain with this consumer.

Active runtime AI selects bounded semantic recipes through the public scene
orchestrator. The former private transport/mock experiment is retired, preserved
in the ignored local archive, and is not an SDK dependency.

Acceptance requires a clean SDK build, scene/bundle closure and current rendered
captures. MULTILANG.md contains historical measurements. Report reproducible
engine/editor gaps without inserting project names or rules into their behavior.

## Byte-level asset provenance and portable delivery

The official repository HEAD observed on 2026-10-06 was `6b8cebefa0ee10e1bdd081dd342a01b3fe753e09`. This identifies the current public reference, not the supplied local snapshot. [Official MIT notice](https://github.com/SahilK-027/Elemental-Serenity/blob/6b8cebefa0ee10e1bdd081dd342a01b3fe753e09/LICENSE) names Sahil K 2026. The preserved snapshot notice has its own hash.

[ASSET_PROVENANCE.json](ASSET_PROVENANCE.json) records every selected model/texture/audio file hash, exact upstream matches, model adaptations and project procedural outputs. Upstream-exact bytes and derived models remain upstream material; decompres-sion or format conversion does not make them original JCE art. Project-generated ground/card geometry and baked reference-palette textures retain their generator and source references. No separate credits or asset license files were found in the bounded public/src text audit; repository MIT coverage is the available source declaration, not independently attested third-party authorship. Unidentified assets must be resolved or excluded before cloud publication.

Portable staging excludes all font bytes and unused external-driver files. It does not copy or edit the original source snapshot. Normal portable and explicitly muted QA bundles are separate artifacts with staged input and output hashes; QA mute is not proof of normal audio parity.

The heightmap conversion is verified across all 65,536 samples: the JCE 8-bit value equals the supplied upstream 16-bit value shifted right 8 bits. Eight same-named adapted models identify the first-party indexed-GLB writer; they remain upstream-derived art, not independent creations. Ten unused legacy assets (old ground-plane, old baked texture set and water-data texture) are excluded from portable staging. All selected non-font model/texture/audio inputs now have either an exact upstream byte mapping, a documented derived mapping, or a first-party generator/reference-palette provenance category. Asset authorship outside the upstream declaration remains unverified; preserve MIT notice rather than deleting attribution.

## Catalog layout

The Git example directory now contains the normal single-file `.jbundle` and
notices only. Authoring inputs and project regression tools moved to
`tests/consumers/elemental_serenity`; ignored local archives/generated output did not move.
The original source snapshot revision and asset rights limitations stated above
remain unchanged. See BUNDLE_PROVENANCE.json for the exact normal artifact hash.
