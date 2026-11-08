# Volumetric fog shaders

Follow the repository contract and renderer shader rules. This raster pass owns
the scene-depth-bounded interval, static march jitter, authored density,
sample positions/count and ONE/SRC_ALPHA composition. GPU cost and visual parity need
actual native observations; CPU helper controls do not establish either.

`volfog_shadow_clip` selects the original matrix before performing one unchanged
matrix-vector product. Integer selectors 0/1/2 select their matching matrices;
all others, including negative selectors, retain the cascade-3 fallback. The
current uniform-only DXBC captured in reports/es-validation/20261018-uniform-only
still computed all four matrix products before selecting the result. This new
data-selection candidate has no measured GPU qualification yet: fresh compiled
code must establish whether dynamic constant-buffer indexing actually reduces
work, followed by native pixel and whole-frame comparisons. Distinct-matrix CPU
cases validate projection values and fallback, not GPU instruction selection or
reduced work. Keep their independent expected coordinates and non-unit w cases.
The separate HLSL matrix-branch candidate has not qualified for a whole-frame
performance benefit; its hints remain removed. This is not a finding that
matrix branching necessarily regresses performance. Do not combine these
experiments with mip selection, projection reassociation, sampling or quality
changes.

The shadow-distance shortcut in fs_volfog.sc applies only where the existing
last-15-percent shadow fade has fully reached lit. It preserves the fade band,
cascade selection/blending/fallthrough and the complete medium integration.
Valid CSM inputs have finite ordered/padded splits, count 1..4, finite matrices,
positions and bias/blend with nonoverflowing projection arithmetic and defined
texture reads. The CPU fixture compares the complete actual helper with its
old path and counts typed texture/transform ports. It does not prove equivalence
for undefined sampler reads or invalid projection math. NaN view depth/reach,
nonpositive reach, infinite reach, counts outside 1..4 and a float-rounded zero
fade span do not take the shortcut. The actual owner publishes an integer
cascade count; fractional in-range values are not part of that producer
contract. Keep unsupported state on its previous path rather than replacing it
with a success claim.

The CPU normalizes the step count to an integer in [1, 64] to match the
shader loop. A positive authored volume radius intersects the near-to-scene
ray segment with a world-space sphere before placing samples. Empty
intersections return identity composition. Radius zero uses the global
medium. Keep the shadow lookup depth camera-relative even when the fog
interval starts beyond the near plane.

An optional periodic world-space density field modulates extinction and
in-scattering at the same march sample. Cell contrast fades when the march
step stops resolving the lattice. Zero noise preserves the homogeneous
shortcut. The default forward phase remains unchanged, and the
optional forward and mirrored backward lobes affect direct scattering only.

For finite march inputs, exactly zero height_falloff and zero density noise
select a uniform medium.
Compute its segment transmittance and bulk in-scattering once per ray, retaining
the original multiplication order, IGN samples, 64-iteration bound, authored
step count, CSM calls, accumulated recurrence and early break. Positive falloff
retains the prior per-sample path. The complete march through gl_FragColor is
extracted by tests/renderer/volfog_density_march_controls.cmake; counted typed
math/sunlight ports compare final RGB/transmittance and sample positions with
the same source's old per-sample block. CPU equality and reduced exp counts do
not establish shader compiler behavior, image parity or measured GPU savings.

The per-sample `!uniform_medium` branch has a HLSL-only `[branch]` hint. Retained
density and matrix-branch DXBC had flattened this draw-uniform condition into
`movc`, executing both per-sample exponentials even when height falloff was zero.
The hint changes only conditional execution intent; arithmetic order, samples,
CSM work and output recurrence remain unchanged. The complete-march CPU adapter
strips that one exact hint as syntax, preserving every equality/count assertion
and the old repeated-exp negative. It cannot prove HLSL execution: fresh DXBC
must show conditional exponential work, followed by actual image and whole-frame
comparison before reporting a GPU improvement.

The D matrix-data selection experiment retained pixel equality in the actual
2.5K capture, but the short A/D/D/A timing window drifted and established no
whole-frame improvement. Its source was reverted to the ordinary four-way
matrix projection selection; the uniform-medium branch remains. Retained D
EXE/SDK/DXBC evidence under reports is experimental evidence, not qualification
of the reverted candidate or a claim that matrix selection caused regression.
