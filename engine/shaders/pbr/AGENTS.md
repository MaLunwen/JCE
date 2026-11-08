# Shared PBR fragment ownership

`fs_pbr_main.sh` is the shared main for stock, generated graph and manifest
variants; entry files and the existing shader keyword/build owners select them.
Keep material sampling, checker fallback, factor/tint and alpha policy in this
one owner. Graph evaluation remains before fade-dither; the complete base-color,
view-mode and alpha-mask block runs after fade and before shadow dependencies.
View modes 2/3 retain their return before alpha mask. Preserve strict alpha below
cutoff rejection and all graph/array/tint formulas; shadow and lighting algorithms
remain with their existing complete blocks.

`tests/renderer/pbr_alpha_order_controls.cmake` consumes the actual main prefix
through the Normal boundary. Its typed CPU dependency observations do not prove
GPU early rejection, helper-lane/derivative behavior, texture LOD, image parity or
performance. Qualify actual shader compilation and backend image/timing evidence
separately; do not change sample counts or quality to satisfy this control.

The stock brute-force point-light loop skips shadow and BRDF dependencies only
when its existing squared radius attenuation is exactly zero. Preserve the
falloff formula, every positive attenuation, original packed light/shadow indices
and the complete nonzero contribution. This equivalence is scoped to finite,
well-defined PBR inputs: positive finite radius with finite squared-distance and
radius arithmetic, finite radiance/shadow/BRDF values and defined normalization.
It makes no NaN, infinity or invalid-material behavior promise. Do not add an
epsilon, color/intensity rejection, light compaction or a quality change; spot,
area and Forward+ loops retain their existing owners.

`tests/renderer/pbr_point_attenuation_controls.cmake` consumes the complete actual
point loop and direct-BRDF helper chain. Typed CPU sample ports and work counts
are dependency controls; they do not certify shader execution, GPU work elision,
image parity or a performance gain. Qualify those with actual compiled backends,
native captures and paired timing evidence.

Local shadow atlas fetches in `fs_pbr_decl.sh::sampleAtlasTile` use explicit
LOD0. The existing depth-target owner creates one-mip textures with both MIN
and MAG point sampling; stage15 inherits those flags. Preserve the normal
offset, depth comparison, tile clamp and hard/3x3 PCF quality branches. The
explicit level removes implicit-derivative requirements inside varying light
control flow; it does not reduce tap counts or establish a performance gain.
Compile actual backend payloads and qualify atlas/sampler state, pixels and
native whole-frame timings separately, including GL/GLES capability floors.

`JCE_SHADER_COMPATIBILITY` defaults to zero. The compatibility GL/ES stock
profile sets it to one and removes the stage-13 array sampler plus the complete
directional and spot cookie blocks. Default cookie formulas, IES and other
lighting math remain unchanged. Runtime policy must exclude cookie array
creation/binding on the same profile; substituting a 2D sampler is invalid.
Shader compilation and CPU ownership controls do not certify old-device pixels.

Water uses sampler 13 for vertex ripple, 2 for opaque depth, and 14 for optional filtered spectral gradients. Derivatives use undisplaced, unwrapped world coordinates; RG and BA carry separate cascade slopes. Normalize only after filtering and adding the independent ripple slope. GL 2.1 retains its existing CPU/vertex normals and omits the gradient sampler. Authoring absence keeps the old normal path; shader compilation and current-backend image controls are still required.

Optional temporal foam samples scene-owned RG cascade density on stage4,
mutually exclusive with stylized water data. `water_froth.sh` combines continuous
macro fragments with smooth warped fBm pores, each calibrated by monotone
piecewise-linear quantiles to sqrt(density) occupancy. Their product preserves
physical mean coverage; neither pattern creates foam from zero density. Filter
the two scales independently toward sqrt(density), so unresolved coverage tends
to the original density without a hard distance switch. Exact zero/one tails
remain continuous. Compute the caller's footprint before divergent early-outs;
these helpers evaluate no derivatives or implicit-LOD texture samples. The
nearshore layer uses real stage-13 velocity with two bounded half-offset flow
phases; zero flow remains stationary.
No analytic pattern creates foam without the existing physical density source.
Raw displacement Jacobian is unchanged. u_water_foam resets each modern draw;
unavailable resources and GL2.1 retain instantaneous foam. CPU tests do not prove
native pixels. Physical rain normal rings use 3.5 cells/metre and slope 0.08,
with the existing phase fade and derivative filter; stylized rain stays separate.

The optional crest term uses height above still water, existing sun visibility and a distance fade. Composite its transmitted light after planar reflection with one Fresnel transmission factor. GL120 retains original shading.

The RGB caustic producer compares projected triangle area with reference area. Its three channels sample the primary FFT at positive, neutral and negative offsets. The atlas has periodic aprons and a bounded gain.

The modern nearshore path uses the same stage 13 sampler for a padded
RGBA16F nearshore/wake atlas. It blends the solved surface and slopes back to
ambient through the buffer, applies depth-based dry coverage, and lights the
advected foam density. The old R32F wake remains the fallback. GL2.1 keeps the
legacy water program and never samples the atlas.

Modern FFT water excludes the legacy UV-radial shore rings; physical depth and
the nearshore field own its coast foam. Other water modes and GL120 keep their
existing radial option. `water_cloud_layer.sh` may borrow the stage-8 planar
sampler for the current physical cloud layer, with a negative explicit mode.
The shared `standard/sky_cloud_geometry.sh` proves the complete layer is in
front of a below-cloud receiver. Mode -3/-4 certifies that opaque backgrounds
received the current cloud pass; both sky and opaque backgrounds then retain
the water coverage. Without that submitted pass, an opaque background requires
composed coverage while a clear-depth sky already contains clouds. Both cases
use straight-alpha output and contribute in-scatter once.
No partial-cloud estimate is made from the representative history distance.
GPU binding, images and performance require separate native validation.

Accumulated terrain snow attenuates the underlying layer normal toward the
actual terrain base normal before the existing signed-track and controlled
snow-grain gradients. Zero snow preserves the original normal exactly. Only
existing negative SnowStamp height adds the bounded ambient-occlusion term;
zero stamps or nonnegative height leave that material factor unchanged. Keep
coverage, shared colour/depth displacement, stamp density and geometry budgets
with their existing owners. The R11 fixed DX11 A/B observes the same eight
circular troughs, exact camera/frame/snow state and shader create/link receipt.
It establishes that backend's material readability, not shoe-shaped geometry,
other backend pixels or a performance gain.

`water_surface_normal.sh` owns the modern colour and optical-trace normal:
filtered FFT cascades, retained shallow slopes, micro detail, rain rings and ice
snow/grain. `water_surface_noise.sh` owns their exact shared interpolant. Keep
unwrapped reference coordinates and normal derivatives before dry rejection.
Colour may flip the final normal toward the viewer; trace admission uses the
returned pre-flip facing so underwater/back-facing fragments retain no-hit
fallback. Ice lighting and optical ray traversal keep their existing owners.
Optional `water_normal_detail.sh` uses two independently filtered, rotated and
phase-advected gradients with weights 0.8/0.6 (squared weights sum to 1). Its vec4
transport is low/high inverse-metre frequency, slope strength, evaluated FFT
phase. Positive frequencies select the extension; strength zero yields no micro
slope, while absent/disabled transport keeps the exact old static 2.1/m path.
Evaluate both derivative footprints before the per-band early-outs. Stage14 is
reused; no sampler is added. The shared scene time still drives rain. CPU parity,
energy and phase controls do not replace actual shader identity, binding, native
motion/near/far images or whole-scene performance validation.


Modern water uses `water_lighting.sh` for ambient RGB times intensity, projected
dominant directional incident light and the homogeneous medium source shared by
colour and refraction. Authored water/absorption/foam/ice colours are material
response, not unconditional emission. The fallback sky follows actual ambient
hue and energy; a radiance cubemap, resolved SSR/opaque refraction and direct
glint are already lit and must not be multiplied by ambient again. GL2.1 keeps
its legacy shading. The medium source is a bounded homogeneous approximation,
not integration of shadowed participating media along each refracted ray.
The global PBR bind precedes both trace and colour submits, so their shared
light uniforms must agree at the actual draw; zero light counts ignore stale
directional slots. CPU controls, backend compilation and GPU binding/image
witnesses establish different parts of this contract.

Visible-sky water reflection is owned by `fs_water_optics_trace.sc` only after
geometry SSR has no valid hit and no uncertain opaque crossing. It reads the
completed linear opaque colour from this viewport, using the shared surface
normal and existing depth/colour samplers. Require the host sky-submit success,
upward direction, finite/front-facing projection, interior bilinear footprint
and all four exact-clear depth neighbours; retain horizon/edge confidence.
Do not relight the already-lit sky or change geometry-hit/refraction paths.
No new sampler or render target is required. This supplies visible screen sky,
not offscreen IBL; the finite geometry march (20 m in the current authoring)
cannot certify arbitrary farther occluders. Compiled controls and actual GPU
identity, source/output pixels and disabled-path witnesses remain distinct.
