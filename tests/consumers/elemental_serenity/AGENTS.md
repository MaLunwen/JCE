# elemental_serenity - JCE dogfood application

## Identity

- L7 consumer and reference application for single-executable deployment.
- Portable Lua is the default ES single-executable product and performance
  baseline. Ordinary `jce_project.json` selects the existing portable scene and
  Lua-only closure. `jce_project.multilang.json` preserves the original
  Lua/Python/Java/C++/C SDK fixture and is selected only by explicit
  `ES_MULTILANG=ON`; retain its required bindings. Catalog Bundle bytes, individual platform applications and
  measured performance each require their own actual acceptance.
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
7. Build through `scripts/jce.py build-project tests/consumers/elemental_serenity`; do not add project
   build scripts outside `scripts/`.

## Default and explicit SDK fixture closure

Ordinary project-open and `build-project` read `jce_project.json`, whose startup
is `scenes/elemental_serenity.bundle.scene.json`, required language is Lua and
native module/factory fields are absent. Keep the actual six portable Script
bindings, scene values and authored RenderPipeline settings shared with the
normal Bundle input. A manifest correction does not requalify a historical EXE.

The preserved original bytes in `jce_project.multilang.json` retain the separate
Lua/Python/Java/C++/C startup, canonical native addresses and two factories/module
paths. The shared CMake owner defaults `ES_MULTILANG=OFF`; explicit ON selects
that fixture manifest and required backend/factory closure. A compatible installed
SDK with unavailable Python/Java must refuse it. The supported `build-project`
driver has no alternate-manifest selector; README documents the explicit public
installed-SDK CMake route. Do not replace or mutate the ordinary manifest to run
that fixture, copy source trees, or relabel Lua success as five-language proof.

The default `src/main_portable.c` is the instantiated public empty-project
entry and consumes only the shared default app callbacks over serialized Lua.
Its source list excludes the custom native main, Mood plan provider, N shortcut,
probe loop and extra PerfCanvas. These remain intact and explicitly selected
only by ES_MULTILANG. There is no second game loop, asset loader or registry.
The selected portable Director has its own byte-identical dependency sidecar so
the generic public packer includes all runtime season textures and audio during
ordinary source-project builds. Native graph and relocated runtime checks remain
necessary; metadata tests do not certify the final asset count or visible states.

The explicit fixture's generated SDK shim remains its sole static native publisher. Default mode
builds no fixture native modules; ON compiles the same source owners and links
module call-down interfaces without a second registry. The in-tree legacy branch
also gates all optional language/runtime/module staging behind ES_MULTILANG and
refuses missing required language targets for that fixture. Selected runtime
metadata comes from the corresponding manifest; removing an optional module key
must tolerate the Lua manifest's absent key.

`es_script_probe.c` keeps the shared probe entities and selects actual script
paths for the chosen entry. Portable probes count Lua script owners, not five
languages. The Mood request/plan/ack protocol retains its separate acceptance.
The narrow existing source controls consume actual selected scene bindings and
real shipment admission with Lua-only availability; the preserved old manifest
is an ordinary rejected negative. They do not compile or run a native SDK app.

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

Portable staging retains a selected script's exact <script>.deps.json declaration. The ES director's explicit portable-to-original alias readdresses existing dependency bytes when no portable-specific declaration exists; missing required metadata fails before output. Staging records original source paths and identical SHA256/bytes, excludes orphan declarations, and includes declarations in its byte cap. The public Bundle packer remains the only dependency-graph authority; staging CPU controls, including an executed old drop-wire negative, do not certify exported Bundle/runtime/rendering parity.

## Serialized Mood and default font consumer

`tools/es_mood_scene.py` appends the same MoodRequest/Plan/Ack/Provider/Status
and nine Canvas buttons to authoring and portable scenes. The shared
`scripts/es_mood_portable.lua` handles request, validated plan, actual director
message and application acknowledgement. APPLIED requires Ack after the
existing director sets its authored season/time state; send_message alone is
not success. Next preset is authored selection, not generated content.
`es_aid_mood.*` supplies optional standalone plans through the public offline
scene director. Editor/portable entries without that provider explicitly show
native UNAVAILABLE and their actual portable preset fallback. An equivalent
Editor native director call-down port is not shipped; do not link another
engine into a ScriptCModule to fake it. Missing nodes, rejected plans and
missing acknowledgement remain observable failures.

Portable Next sends the existing apply message with selector `next`; the
Director resolves it from its committed season/time state before applying and
acknowledging. Top season, Rain and day/night buttons therefore remain the same
authoritative state owner. Mood's last acknowledged state is not a second
selector source. Only this portable Next request accepts any valid state in
`0..7` from its matching Ack; explicit presets and native plans require the exact
requested state, and native negative plan states remain invalid. Busy handling,
request sequencing and timeout behavior are unchanged. The optional native
planner retains its own existing plan path; this portable fix does not certify
interleaved top-button synchronization for native or custom plan providers.

Blank UIText/main defaults use shared automatic host/stock selection. Optional
LXGW bytes remain intact for explicit authoring. Its local provenance records
actual SFNT metadata and the matching pinned upstream OFL notice, while upstream
release binary hash comparison remains NOT_VERIFIED. The default Bundle roots
exclude the font. Original assets and published catalog Bundles stay unchanged;
new native/Bundle payload membership still requires the public packer's actual
BOM and each asset's rights record.

`tests/mood_protocol.c` executes the current authoring and portable Lua through
an installed public SDK with counted host ports. It covers eight state handlers,
actual apply/Ack, six seeded top-button/Next interleavings, missing receiver
timeout, native plan wait, busy, rejected plans (including -1), mismatched explicit
Ack and missing nodes. Removed actual Ack-wire, restored old stale-Next decision
and removed receiver-selector variants must fail ordinary assertions at runtime
after successful compilation. CPU protocol and staging checks do not certify rendered
Editor, relocated standalone or reopened Bundle parity. Normal ES reopen checks
use `--content-policy normal --content-record <BUNDLE_PROVENANCE.json>`; the
existing default qa-muted policy and negative controls remain distinct.

The Director owns only BeautyCam's six VirtualCamera pose fields. Each update
checks public has_component before submitting a reused six-key partial patch;
a missing camera/component stays absent, and current fov/priority/active values
remain with their external owner. Keep the generator and both Lua owners aligned.
The installed-SDK mood_protocol fixture runs both complete Directors through the
actual Script VM, compares pose values with the old transport in the same Lua
math, and checks absent/reappearing components and zero component JSON reads.
Its ordinary old-read and missing-guard controls must fail assertions. Counted
host destinations do not certify the native generic merger, rendered parity or
whole-frame performance; revised assets need separate three-host acceptance.

BeautyCam authors one primary Camera output beside its VirtualCamera shot controller, with matching 25-degree FOV and the same look-at pose in authoring and portable entries. A VirtualCamera alone does not replace the runtime primary output. Keep gen_scene.py synchronized; a warning-free actual reopen remains required.

BeautyCam's actual primary Camera uses SKYBOX clearMode 0 so the authored sky
can be submitted by the common runtime render path. COLOR clearMode 1 suppresses
that content even when the scene sky itself is enabled. The generator's actual
BeautyCam append and both serialized entries are source-controlled together;
VirtualCamera shot updates do not substitute for this Camera policy. Source
controls do not certify cooked Bundle consumption, visible sky or performance.

## Full reopened lifecycle witness

`tools/bundle_reopen_suite.py` defaults to `--mode full-lifecycle`; explicit
`--mode policy` retains the previous 360-frame policy-only witness. Both fixed
frame modes set `JCE_MAX_SECONDS=0` so the performance helper's inherited timed
budget cannot terminate the requested frame count. Full mode records all 900
host frames with fixed simulation dt, requests the Game image at 12 seconds,
then stops after 800 PLAYING updates. This is synthetic lifecycle evidence,
not normal-clock throughput or physical input. The 50/220 observations retain
early liveness for the original five Lua owners. Full mode sends explicit virtual Game motion,
down and up at visible Play frames 398/400/402, after the current 1.2+4.5 second
reveal. Separate 400/600 host observations retain every original Lua owner, rain,
flower/firefly, extinguish and default-font check; moving this weather window is
not relaxing those checks. Require all three original synthetic admissions rather
than inferring input delivery solely from a scene change. This virtual producer
feeds the normal runtime commit without changing native hover/focus policy.
The 10/800/880 observations use the maintained canonical scene restoration
validator. The pre-Stop scene requires the director-owned IntroTitle to be
cleared and one authored primary Camera; these serialized observations do not
prove the screenshot contains the 3D diorama.

Full mode additionally clicks the existing MoodState6 rainy/day button at visible
Play frames 648/650/652. It derives the point from the serialized HudCanvas and
UIImage rectangle, checks its UIButton handler and retains the source layout
hash. The saved actual scene600/800 layouts must agree with those event bytes.
Require all six virtual admissions, actual portable Mood Script binding, READY
to APPLIED status, request `(0,-1,0)->(1,6,0)`, director Ack `(0,1,0)->(1,6,1)`
and ground rainy_night to rainy_day. Provider and plan observations must remain
zero for this portable Editor case; the runner never makes a native provider
available or injects expected scene state. The additional click occurs after the
unchanged 400/600 rain oracle. It keeps the rainy season, and both the original
weather check and new Mood check require both campfire lights extinguished.
Policy mode retains its original three events and has no Mood requirement.
The sixth owner is proven through its actual request/apply/Ack/status path, not
a fabricated lifecycle probe. This is serialized application evidence; the
ordinary Game screenshot still does not certify Mood text pixels or physical UI.

Full mode enables the existing CPU/resource trace and consumes the maintained
bounded streaming lifecycle parser. Require one create/destroy and Play/Stop,
positive runtime ticks, complete UTF-8/EOF and stable input identity, zero loss,
closed captured scopes and registered-owner termination. Resource samples retain
known/unknown flags and owner-thread identities; they are not a global allocator,
all-OS-thread, leak or GPU-completion certificate. A parser failure retains the
trace summary and the outer failure receipt. Nonflat pixels remain only readable
Game-image evidence; a UI/intro can satisfy that oracle. There is no calibrated
ES diorama ROI contract here, so image parity must remain separately inspected.

Full mode also explicitly requires the canonical typed renderer submission
consumer and retains its bounded packet result in the lifecycle receipt. Missing
packets, incorrect scope/thread ownership or incomplete four-field packets fail;
legacy policy mode is unchanged. Unknown context generation and uint32 wrap stay
explicit observations, never GPU completion or a join to latched GPU telemetry.

The adjacent CPU controls feed the actual CSV, canonical scene and full streaming
trace/resource consumers, including invalid restore, intro, camera, frame count,
loss, EOF, unclosed scopes, missing/duplicate runtime owners and absent resources.
Process/image ports are explicitly controlled and make no native-run claim.
Failure receipts retain the observed process and backend before policy checks,
including when a healthy native process fails a weather or lifecycle oracle.
Same-source old-intro-schedule, removed full-validation and submission-wire controls
must terminate with ordinary assertions, rather than syntax/import failures.

## Discrete flower visibility ownership

The Director owns visibility of its resolved flower roster for one Lua owner
lifetime. Startup reads each MeshRenderer once, retains its complete entity
identity, original prefix index and actual visible bit, and skips missing
components. Discrete season/day changes retain the existing index hash and
fractions, patch only changed visible bits through public comp_set, and update
the cache only after an accepted write. A rejected write raises an observable
error. A fresh on_start clears the roster cache; do not retain entity rows
across Play owner recreation or replace visible with component-enable state.
The continuous color tween still updates all foliage/grass, lights, Water and
render settings; flower component JSON is absent from that hot path.

`tests/director_visibility.lua` executes complete current portable/authored
Directors against an explicitly supplied frozen pre-change portable owner.
Counted public host ports cover the102-flower loop, cold/changed-only writes,
all eight states, day/night, reverse/repeated/retargeted selection, mixed dt,
missing components and fresh generation-bearing owners. Every sampled visible
pattern, unrelated authored MeshRenderer field and40+4 dynamic color result
must agree with the actual frozen owner. The reference must demonstrably run
its repeated loop; do not replace behavioral checks with source-pattern tests.
This is a consumer CPU contract, not Engine serializer timing, actual native
Play/Stop, GPU lifecycle or whole-frame improvement evidence. The authored
change belongs in tools/gen_director.py and its generated script; the maintained
portable owner carries the equivalent policy. Preserve wind trajectory math.

## Portable wind transport contract

The portable director owns its three authored WindLine components for its
lifetime. It resolves component presence once at startup, reuses one flat
42-number point buffer and one style table per line, and requires the public
`line_set_points` call to store all 14 points. Style remains a small component
JSON patch; the generic engine merge can still serialize/reparse existing
points internally. This is not a claim that all engine-side JSON work is gone.
The hidden transition explicitly patches `positionCount=0` once; an empty point
table does not clear the public component. Missing components or rejected style,
point and hide writes fail visibly instead of taking a JSON fallback path.

Run `lua resources/assets/scripts/test_portable_policies.lua resources/assets/scripts`
from this project with an available Lua 5.4 interpreter. All earlier policy
assertions remain, then `tests/director_wind.lua` executes the complete current
portable and generated authored owners with counted public host ports. Its
6000-frame steady, mixed-dt and exact-boundary schedules cover all eight states,
respawn/gap timing, random consumption, fixed style and buffer reuse. These are
consumer CPU controls, not the native SDK adapter or rendered parity. The
authored owner remains the actual legacy JSON trajectory reference; do not copy
its wind equations into the fixture or alter that owner to satisfy the test.

Random consumption, position counts and style are bit-exact. Coordinates may
differ by at most one adjacent finite float32 value: the authored JSON encoder's
`%.9g` introduces an intermediate decimal rounding before float storage, while
the public batch stores the computed number directly. The retained `exact` mode
exposes this difference; `old-json`, `unchecked-return` and `shifted-points` modes
must fail ordinary runtime assertions for the old hot path, missing return check
and a larger trajectory change. No coordinate formatting is added to production
to reproduce the obsolete decimal transport. New Bundle/EXE staging, native
three-host lifecycle, actual images and paired normal-clock timing remain
separate acceptance steps; these CPU controls establish no millisecond benefit.

The scene currently has six Script owners. The original reopened five-policy
table covers Director, Fireflies, Campfire, FlowerSway and PropSurface. Full mode
also requires MoodRequest's serialized request/apply/Ack path described above;
actual native-run acceptance requires a fresh fullsuite receipt. Its separate
public-SDK `tests/mood_protocol.c` checks remain CPU protocol evidence rather than
rendered Mood or standalone/provider parity. The adjacent fullsuite CPU controls
retain actual main-loop validator wiring with explicitly authored process/image
ports, plus ordinary omitted-wire assertions for missing Ack, wrong sequence,
unexpected provider, unchanged ground and missing Mood input admissions.

## Optional portable capture seeds

`tools/stage_portable_bundle.py --capture-seeds A B` opts the staged Director
into two existing public NUMBER Script.params, `capture_seed_a` and
`capture_seed_b`. Both must be integers in `0..16777215`; the public parameter
stores float32, so larger integers would not retain every distinct seed. The
stager rejects missing/ambiguous Directors, malformed/duplicate parameter rows
and insufficient slots before creating resource output, preserves unrelated
parameters, and explicitly replaces any existing capture pair. Seed selection
and scope are recorded beside source/staged hashes. No flag means no scene
rewrite or implicit seed; do not add @export defaults for these optional rows.

The actual portable Director reads both values through `jce.get_param` using
`self.entity` and calls Lua 5.4 `math.randomseed(a,b)` at startup before random
use. A partial or invalid pair raises a visible error; successful application
logs `ES_CAPTURE_SEED` once. The stream is shared within the Lua VM. Fresh
runtime/Bundle, fixed dt, matching inputs and frame selection are still
required; native particle/GPU RNG and rendering determinism remain separate.
Other scripts consuming Lua randomness would also share this seeded stream.

The existing `tests/director_wind.lua` executes the actual complete Director
for two identical 800-frame seeded schedules after different prior RNG states,
requires identical point/style/count bytes, and observes a changed trajectory
when only the second seed changes. It retains the 6000-frame normal unseeded
comparison, checks zero/max bounds and invalid rows, and exposes `missing-seed`
mode as an ordinary assertion failure after removing only the real seed call.
`tools/test_stage_portable_bundle.py` also retains an executed omitted-parameter
wire negative. These are CPU consumer controls with counted public ports;
actual SDK parameter loading, staged Bundle reopen and image parity require
their own runtime evidence. Do not claim native/GPU replay from these controls.

## Authored render-pipeline closure

Portable staging requires explicit --project in addition to --resource-root.
The common public Bundle packer owns collection of exactly the standard
Settings/RenderPipeline.rp.json as settings/render_pipeline.rp.json; this tool
does not copy the Settings tree or rewrite feature toggles. Count its bytes in
the staging bound, retain source SHA-256, require one correctly sized canonical
actual-BOM entry and retain that entry's content identity. Source changes during
export fail. Missing source retains the packer's documented default policy;
present invalid/unreadable input is an error. Python controls validate argument,
provenance and omission detection, not the native archive payload or runtime
application. Reopen and inspect actual effective settings for each host before
claiming scene fidelity or attributing performance changes to one render pass.

## Default authored antialiasing

ES authoring and portable scene rendering enable spatial FXAA by default.
Keep tools/gen_scene.py and both serialized scene postfx.fxaa values aligned.
Settings/RenderPipeline.rp.json disables TAA by default; temporal AA remains an
explicit author opt-in through that standard policy or the existing Editor
Render Pipeline controls. This is application policy, not a change to Engine
HIGH/ULTRA presets or a claim that the temporal algorithm is repaired.

Use the existing tools/test_mood_scene.py default-policy control to reject
generator/scene/Settings drift. Repacking, actual shader consumption, dynamic
quality, lifecycle and performance require separate artifact-bound validation.
