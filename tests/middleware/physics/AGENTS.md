# Native physics debug query controls

`test_jce_physics_debug_range.cpp` compiles the complete production query with
real immutable Bullet collision worlds, shapes and BVHs. It covers the original
52-byte allocation, invalid descriptors, moved camera/body, exact retained
character priority, giant spanning meshes, candidate/node and triangle bounds,
crossing triangles, scaled/rotated compounds, and explicit unsupported BVHs.
The new query TU is compiled directly into this contract target; no renderer
or private exported SDK symbol is needed.

Preserve `test_jce_physics_debug_mesh.cpp` and the ordinary-failing old-quota
control after indexed-reader refactors. Source-variant radius-disabled and
character-disabled controls must compile before expected ordinary assertion
failures count as evidence. CPU ASan/UBSan old-header caller execution, textual
ABI append checking and real C99/C++ header inclusion are separate checks.
None certifies Windows graphics, full SDK consumers, runtime lifecycle, frame
latency or performance gains on a capture recorded before these changes.
