# Contextual Bundle provenance

Published file: [tower-defense.jbundle](tower-defense.jbundle).
Size: **19,910,914 bytes**. SHA-256:
`1866453161b34296fea5611421b83cc158c837ba5c85037099db1d1f14466b59`.
The catalog copy is byte-identical to the verified content archive; no compiled native
application module, SDK, DLL kit, private prompts or private workflow is added.
GPU shader payloads remain content assets; they are not a native host program.

The archive declares minimum engine version **0.13.4**, `jce-content-1`,
`jce.bundle` contract 1.1 and scene entry `scenes/td_lab.scene.json`. Its manifest
contains **111 payload assets**. The delivery verifier records **79 reference
source files**, **28 original raster assets** and **6 validated GLB models**;
these are different inventory scopes and must not be summed into a source count.
The payload includes a `c.class-ref`, not a compiled native module.

The producer was the public SDK `jce_bundle_pack` executable, SHA-256
`ce5b6032d60176dd723634fb350bed1329b503dd94013f8a5a384f79fbfe2004`.
Local reproduction evidence is retained under
`tests/consumers/td_lab/build/verification/perf27/`
`native15-editor22-correctness-preparation/runs/final-native15/delivery/`:
`content-bundle/tower-defense.jbundle.json`, `content-bundle/bundle_graph.json`,
`delivery-suite.json`, `bundle-parity-0.json` and `bundle-parity-1.json`.
Those generated records are not published as catalog payloads.

Both reopen/Play/Stop controls passed in a Windows/D3D11 native authoring context,
with the C class `TdController` and factory `td_module`. They observed mutation
and restoration of authored fields after bijective ID/parent-ID renaming, and
matching Game captures; their result is `PASS_WITH_NATIVE_AUTHORING_CONTEXT`.
They do not establish stock-host portable gameplay, every gameplay path, every
platform or an independent executable. SDK-built single-EXE/HTML applications
have separate artifact identities and execution receipts.

The current controls used Editor22, SHA-256
`158e17459d402356e155e1b97ca3311d6ca9b0d62d5e26793ab1e5cb52e3a5b1`,
and native authoring DLL14, SHA-256
`27280742fa91858b935a760eb2759807c841e04fd41b04132e57ec1d53afbb42`.
The independent authoring context had no loose resources. Both cycles restored
the 104 authored entities; actual C-controller logs separately identify gameplay
and destruction. Scene mutation alone remains insufficient script evidence.

Original reference fidelity has a retained raw touch-rotate failure. Finite
shader/reference derivatives are distinct from untouched upstream-source
comparisons; contextual content acceptance does not erase that boundary.
