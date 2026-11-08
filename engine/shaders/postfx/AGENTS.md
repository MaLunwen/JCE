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
