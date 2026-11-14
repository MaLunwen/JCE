# Shared post-processing shader ownership

`fs_composite.sc` owns the shared composite algorithm and effect order.
`JCE_SHADER_COMPATIBILITY` defaults to zero; compatibility GL/ES sets it to
one and removes the 3D LUT sampler and the complete LUT grade block. This
is an explicit temporary unsupported 3D-LUT effect, not a 2D fallback or
full grading certificate. Tonemap, other colour coefficients, motion/depth
effects and dither retain their algorithms. The runtime shares the same
profile predicate and skips stage-2 LUT resources/binds while retaining
authored settings and diagnosing a requested unsupported LUT once.
An actual CPU-compiled payload without sampler3D avoids that bgfx GL2.1
wrapper trigger; driver compilation, rendered UI/pixels and timings remain
separate acceptance evidence.


AgX's sigmoid and outset produce a display-encoded value. The active composite
converts it back to display-linear with the 2.2 transfer before returning to
`combineTonemap`, which owns the shared final output gamma. Preserve the other
operators, exposure, bloom, grading order and compatibility LUT exclusion.
`jce_postfx.c` loads/submits the composite; the unused standalone tonemap copy
is not an alternate active path. The complete-source CPU controls under
`tests/renderer/postfx_agx_controls.cmake` verify that contract and an ordinary
missing-transfer failure. CPU numbers do not certify shader compilation,
active GPU bindings, native pixels or nighttime readability.
