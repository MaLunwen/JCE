# Benchmark sources and redistribution

The consumer host, analytical joint chain, contact boxes, streaming scenes and
primitive stress arrangement are first-party JCE fixtures under the repository
licence. Primitive topology is not a matched cross-engine source asset.

Canonical external inputs are separate from the SDK and remain in an explicit
ignored asset cache. Use the same immutable asset bytes in each engine; record
their hashes and each engine's import/conversion settings. Original upstream
files must remain unchanged. Test reports and generated bundles are local output.

## Animated Fox

Source: [Khronos Fox](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/Fox).
Revision: `edc7c9e67c639d230715049ee31f9a96a6babbbe`.
GLB SHA-256: `d97044e701822bac5a62696459b27d7b375aada5de8574ed4362edbba94771f7`.

Credit PixelMannen (model, CC0-1.0), tomkranis (rigging and animation,
CC-BY-4.0), and AsoboStudio/scurest (glTF conversion, CC-BY-4.0).
Preserve the pinned upstream README and LICENSE with any distributed fixture
bundle, provide attribution and indicate changes to derived scenes. This is not
an exclusively CC0 asset. The original GLB has 1,728 vertices, 24 joints and
Survey/Walk/Run clips. These are input counts, not a successful import verdict.

## Static Box

Source: [Khronos Box](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/Box).
Revision: `edc7c9e67c639d230715049ee31f9a96a6babbbe`.
GLB SHA-256: `ed52f7192b8311d700ac0ce80644e3852cd01537e4d62241b9acba023da3d54e`.
Copyright 2017 Cesium; CC-BY-4.0. Preserve the pinned source notice and attribution
with any distributed fixture bundle and identify derived-scene changes. The
unchanged 1,664-byte model is a canonical static-mesh input for GPUScene tests;
acquisition alone is not proof that the GPU-driven path ran.

## Complex static PBR chess set

Source: [Khronos ABeautifulGame](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/ABeautifulGame).
Revision: `edc7c9e67c639d230715049ee31f9a96a6babbbe`.
GLB SHA-256: `bd7133b4b322aae97c589b8839dae8155ad2546acb35ae32a127e722a959d007`.
Copyright 2020 Academy Software Foundation (original model), 2022 Ed Mackey
(glTF conversion); CC-BY-4.0. Preserve the pinned upstream README, licence and
credits in distributed fixture bundles and identify derived-scene changes.
The unchanged asset contains 49 nodes, 15 meshes/materials and 33 images.
Its 49 node-referenced primitives contain 944,100 position vertex references.
Optional transmission/volume extensions are present. The base PBR liveness
control does not certify those extensions or independent material fidelity.

## Restricted reference input

[DamagedHelmet](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/edc7c9e67c639d230715049ee31f9a96a6babbbe/Models/DamagedHelmet)
at the same revision declares both CC-BY-4.0 and CC-BY-NC-4.0 credits.
It is retained only in the local evaluation cache. Do not include its bytes in
the public SDK, unrestricted sample bundle or commercial output. Choose a
licence-compatible PBR fixture for distributable regression bundles.

## Fonts and external engine scenarios

Fonts are explicit host-file inputs, hashed during text validation. They are not
copied into the project, SDK or bundles; ownership and redistribution terms remain
with the font provider. A measured font on one host is not a portable font asset.

Unity ECS/Megacity, Epic City Sample and Godot's official benchmark repository
provide separate scenario and instrumentation references. Their licences and
hardware/workload scopes apply independently. References are not measurements
of matched engine speed; missing local timings must remain unavailable.
