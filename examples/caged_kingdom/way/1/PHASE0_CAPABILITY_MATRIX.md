# JCE Environment & World Authoring — Phase 0 Deliverable: Capability Matrix & Build Order

**Plan:** `examples/caged_kingdom/way/1/JCE_ENVIRONMENT_WORLD_AUTHORING_COMPLETE_PLAN.md`
**Repo head at audit:** `9b96578e` "world: one environment state, because there were three winds" (branch `track-space-project`)
**Status vocabulary:** `COMPLETE` = ships and meets the plan's clause · `PARTIAL` = ships but does not meet the clause · `BUILT_BUT_UNWIRED` = producer exists, compiles, is tested, has zero consumers · `MISSING` = no code

**Totals: 211 capabilities — 57 COMPLETE, 70 PARTIAL, 84 MISSING.**

*Recounted 2026-08-15, and the count is the point: the four BUILT_BUT_UNWIRED are
closed (U1-U4), B4/B5/B23/B26 landed, and B24/B25/C13/C23/F27 moved because
measuring them disagreed with the row. One moved DOWN — B25 from COMPLETE to
PARTIAL, because a path that exists is not a path that runs. The rows are
counted from the table itself; `F6-F11` is one row worth six capabilities, which
is why a naive row count reads 206 and not 211.*

*Every row touched here carries the number that moved it. A row that says
COMPLETE without one is a row nobody has run.*

---

## 0. READ THIS FIRST — a regression landed between the audits and this document

The §2 commit `9b96578e` moved the weather overlay onto the new `JceEnvironmentState`:

```
engine/src/middleware/scene/jce_sr_environment.c:4017-4019
    st.wind_dir      = sr->env.wind_direction_ws;
    st.wind_strength = jce_environment_wind_speed_now(&sr->env);
    st.wetness       = sr->env.global_wetness;
```

**Nothing anywhere writes `sr->env.wind_speed_mps` or `sr->env.wind_gust`.** Verified: the only writes to `sr->env` in the whole repo are `jce_scene_renderer.c:1460` (`= jce_environment_default()`) and `jce_sr_environment.c:3980-3983` (weather_type, precipitation_rate, cloud_coverage, cloud_density). `jce_environment_default()` sets `wind_speed_mps = 0.0f` (`jce_environment.c:70`), and `jce_environment_wind_speed_now` returns `base * (1 + gust)` = **0.0** (`jce_environment.c:228-236`).

Consequence, in every shipping frame: `jce_weather.c:126` computes `wind = wind_dir.x * wind_strength * 0.05f` = 0, `fs_weather.sc:68` adds nothing, and **rain and snow now fall perfectly vertically.** Before the commit the value was `3.0f * intensity` for rain / `1.5f * intensity` for snow (`jce_weather.c:149-158`). The vegetation-pcg audit's "Snow wind drift → PARTIAL, works but wrong source" was true when it ran; the fix for it must now also restore a non-zero source. `sr_drive_weather` is the sole setter (`jce_weather_set_state` has exactly three call sites, all inside it: `:3993`, `:4020`).

This is the archetype of what this whole plan is about: **a carrier landed before a writer, and the visible behaviour got worse, silently.** Every wiring task below must be paired with a writer, or it repeats this.

**Closed 2026-08-15 by U4's writer** (`21e38944`), and verified rather than assumed: `hidden_cove_rain` reports `wind_speed_mps` 3.00 with gust 0, so `jce_weather.c:126` computes `1.0 * 3.0 * 0.05` = **0.1500** — the same number the pre-regression `3.0f * intensity` produced. The check that mattered was not the speed but `wind_dir.x`: U4 deliberately did **not** migrate wind direction, and had `jce_environment_default` left x at 0 the drift would still be exactly zero with a perfectly healthy speed beside it. It leaves x = 1.0 (`jce_environment.c:70`), so the product survives. That is luck, not design, and it is why C13 stays PARTIAL: **nothing writes `wind_direction_ws`.**

---

## 1. BUILT_BUT_UNWIRED — do these first

Four entries in 211. All four are fields of **one struct**, `JceEnvironmentState` (`engine/include/jce/middleware/world/jce_environment.h:52-96`), landed in `9b96578e` together with its unit tests (`tests/middleware/world/test_jce_environment.c`, registered `tests/middleware/world/CMakeLists.txt:75-76`). The module is complete, sanitized, integrated and framerate-independent. Its consumers are what is missing.

That concentration is the single most useful fact in this document: **the entire sunk-cost recovery in this plan is "finish wiring the struct that just landed", and it is also §2, the plan's own mandated foundation.** Four wiring tasks, four different subsystems, one producer.

### U1 — `humidity` / `precipitation_rate` → fog extinction (§4.2)
*Built:* `jce_environment.h:83-85` (`humidity`, `fog_density`, `precipitation_rate`), `temperature_c`; integrated by `jce_environment.c:175-230`.
*Missing wiring:* (a) nothing writes `humidity` or `fog_density` — they hold their defaults (0.4, 0.0) forever; (b) there is **no mapping function** from humidity/precip to an extinction coefficient anywhere; (c) `jce_volumetric_fog_set_params` is fed from `JceSceneRenderingSettings` only (`jce_scene_renderer.c:4928`), never from `sr->env`.
*Work:* write `jce_environment_fog_extinction(const JceEnvironmentState*)` next to the other derived views (`jce_environment.h:118-131`), make the weather preset write humidity, and change `jce_scene_renderer.c:4928` to take `max(authored, derived)`. Small — but it is three pieces, not one.

### U2 — `moon_direction_ws` / `moon_illuminance_lux` → fog lighting (§4.3)
*Built:* `jce_environment.h:68-69` + the key-light accessors `jce_environment_key_direction` / `jce_environment_key_illuminance` (`jce_environment.c:245-261`), whose entire stated purpose is that "callers that pick a key light must agree on where the switch happens or shadows swap direction mid-frame".
*Missing wiring:* `jce_volumetric_fog.c` holds a single `u_sun` / `u_sun_color` uniform pair (`jce_volumetric_fog.c:38-39`) fed from the directional light; nothing selects the moon below the horizon. Nothing writes the moon direction either — it stays at the default `(0,-0.9,-0.436)`.
*Work:* drive `sun_direction_ws`/`moon_direction_ws` from `jce_time_of_day.c`, then feed the fog uniform from `jce_environment_key_direction`.

### U3 — key-light accessors → sun/moon shadow cascades (§8)
*Built:* same two accessors.
*Missing wiring:* `jce_csm.c` is called with the sun direction only; there is no night cascade. Night currently gets `jce_time_of_day.c:191`'s "moonlight tint" — a colour, not a light with a shadow.
*Work:* one call-site change in the CSM setup, once U2 has given the state a real moon direction. Do U2 and U3 together; they share the writer.

### U4 — `wind_direction_ws` / `wind_speed_mps` / `wind_gust` → vegetation, ocean, weather overlay (§36, §23, §7.2)
*Built:* `jce_environment.h:71-76` under the comment *"ONE wind. The ocean spectrum, the vegetation shader and the weather overlay all read this; none of them keeps its own any more"* — plus `jce_environment_wind_speed_now` (`jce_environment.c:228-236`).
*Missing wiring, and it is the largest of the four:*
- **No writer at all.** Speed and gust are 0 forever. This is the §0 regression.
- Grass still uploads its own authored wind: `jce_sr_environment.c:1910-1911` reads `g->wind_dir` / `g->wind_speed` from `JceGrassFieldComponent` (`jce_scene.h:1136-1138`).
- Ocean still uses its own: `JceWaterComponent.fft_wind_dir_x/z`, `fft_wind_speed` (`jce_scene.h:1226-1229`), consumed at `jce_sr_environment.c:2414-2415`.
- The commit message for `9b96578e` says this explicitly: *"Not yet migrated: the grass and ocean winds still read their authored fields."*
*Work:* (1) a writer — weather preset or ToD — that sets speed/gust/direction; (2) `jce_sr_environment.c:1910-1911` reads `sr->env` with the authored component field demoted to a per-field *multiplier*; (3) same for the FFT desc build. Item (2)/(3) change every existing grass/ocean look, so they need a golden re-baseline, which is a §53 dependency — see §5.

**Everything else the earlier audits labelled BUILT_BUT_UNWIRED was corrected to PARTIAL with proof and is not in this section:** the unified EnvironmentState (it *is* ticked, `jce_sr_environment.c:3980-3984`), async/cancellable derived rebuilds (the IBL bake already does it, `jce_sr_environment.c:3777-3784`, `:3766`), the source-hashed derived cache (`jce_ibl.c:584-585`, `:599-657`), terrain tiling authorability (`jce_terrain.c:578-586`), the Lua weather path (`jce_script.c:901` → `jce_scene_components_json.c:3364-3379`), snow wind drift, and material wetness response (which is MISSING, not unwired — there is no wetness/clearcoat parameter in `engine/include/jce/renderer` at all, and zero `wet` hits in any lit shader).

---

## 2. Capability matrix

### Area A — Atmosphere / Sky / Fog / Sky-lighting (§3, §4, §9) — 27 caps · 5C / 6P / 2U / 14M

| # | Capability | § | Status | Evidence |
|---|---|---|---|---|
| A1 | Rayleigh + Mie + ozone transmittance model | 3.1 | COMPLETE | `jce_atmosphere.h:44-68`, CPU, headless-safe |
| A2 | Transmittance LUT baked + sampled | 3.1 | COMPLETE | `jce_atmosphere.h:107-108` (64×256); `fs_sky.sc:62,150-166` |
| A3 | Multi-scattering LUT | 3.1 | MISSING | no symbol |
| A4 | Sky-view LUT | 3.1 | MISSING | no symbol |
| A5 | Aerial-perspective LUT / 3D volume | 3.1 | MISSING | "aerial perspective" is height fog: `jce_scene.h:400-401` *"AERIAL PERSPECTIVE reuses the existing fog_* fields"*; `fog_apply.sh:5-27` |
| A6 | Ground-to-space parameterization | 3.1 | PARTIAL | planet-curvature term `fs_sky.sc:356`; LUT is not space-parameterized |
| A7 | Editor Atmosphere params (14) + presets | 3.2 | MISSING | `grep -rn atmosphere editor/src` = **0 hits** |
| A8 | Sun in lux | 3.3 | PARTIAL | `jce_atmosphere.h:59-61` (128000 lx), `jce_atmosphere_sun_illuminance`; consumed by ToD only |
| A9 | Camera EV100 | 3.3 | MISSING | `jce_postfx.h:56` exposure is a bare multiplier |
| A10 | One radiometric convention across sky/cloud/fog/water | 3.3 | MISSING | cloud uses `u_sky_params.y` scale, water uses `sun_specular`, fog uses its own colour |
| A11 | Froxel grid volume | 4.1 | MISSING | `jce_volumetric_fog.c:2-7` *"Single full-screen pass… march N samples"*; the only froxel grid is the light cluster (`jce_light_cluster.h:4`) |
| A12 | Per-froxel extinction/albedo/emissive/phase | 4.1 | MISSING | — |
| A13 | Non-linear Z slicing | 4.1 | MISSING | — |
| A14 | Global height fog | 4.2 | COMPLETE | `jce_volumetric_fog.c:70-80`; wired `jce_scene_renderer.c:4908-5004` |
| A15 | Local box/sphere fog volumes | 4.2 | MISSING | `grep fog_volume\|FogVolume\|local_fog` = 0 hits |
| A16 | Mist / dust / smoke / waterfall mist media | 4.2 | MISSING | — |
| A17 | Weather humidity → fog density | 4.2 | **COMPLETE** | U1 landed `6834668d`. Koschmieder-calibrated: clear 30 km, saturated 500 m, heavy precipitation 1 km. End to end, clear uses the authored 0.001300 (V = 3009 m), rain derives **0.009902** (V = 395 m) |
| A18 | VFX particle volumes into fog | 4.2 | MISSING | — |
| A19 | Fog lighting: sun | 4.3 | COMPLETE | `jce_volumetric_fog.c:38-39` |
| A20 | Fog lighting: sun shadow (CSM) | 4.3 | COMPLETE | `jce_volumetric_fog.c:40-44` `u_csm_vp`, `s_csm[4]` |
| A21 | Fog lighting: moon | 4.3 | **COMPLETE** | U2 needed no change: the fog's sun comes from `shadow_sun_hold.dir`, the quantised direction the cascades actually rendered for, so pointing the cascades at the key light carries the fog with them. Feeding the fog separately would put the light axis off the shadows it passes through |
| A22 | Fog lighting: point / spot | 4.3 | MISSING | — |
| A23 | Fog temporal reprojection / cut reset / ghost rejection | 4.4 | MISSING | — |
| A24 | Sky radiance cubemap | 9 | **COMPLETE** | 2026-08-15. Was HDR-path only. `sr_refresh_sky_ibl` evaluates the analytic sky into a 128x64 equirect with `jce_sky_radiance` (pure math, headless-safe) and feeds the same `jce_ibl_bake_cpu` the HDR path uses |
| A25 | Diffuse irradiance from sky | 9 | PARTIAL | `jce_sr_draw.c:3373-3393` Preetham SH9 → `light_env`; magnitude stays authored `:3405-3420`; single-source gate `tools/lint/check_env_light_authority.py:20-23` |
| A26 | Specular prefilter | 9 | **COMPLETE** | 2026-08-15. The whole IBL path was keyed on a Skybox component's `hdr_path` changing, so **every scene lit by the analytic sky had no environment specular at all** — metal, water and wet ground reflected nothing. Now baked from the sky. Isolated by zeroing `specularIBL` in **both** lit shaders: dry `slope` +0.177 mean (0.02% of pixels), rain at wetness 0.9 **0.43%** — twenty times the area, because the wet response multiplies roughness by 0.35 and that is the regime an environment specular shows up in. Small in a scene of sand and rock, which is correct; the point is that a scene with polished content now gets one |
| A27 | Amortized probe update | 9 | PARTIAL | cos(0.5°) for the SH9 ambient; cos(2°) for the sky IBL, because that one is a cubemap convolution on a worker rather than nine multiply-adds. Cost measured on `hidden_cove_daynight` (4 h/s, so the sun crosses 60°/s): convolution **5400 ms at irr32/pf128 → 1350 at 16/64 → 395 at 16/32**, and 16/32 is **indistinguishable** from 32/128 in the picture (+0.004 mean, 0.03% of pixels, at the run-to-run noise floor) — the large sizes were spending 13x the work on detail an analytic sky does not contain. Bounded by one bake in flight, so the worst case is one LOW-priority worker busy; a realistic cycle triggers one bake per ~8 s. Still **no** per-face staggering |

### Area B — Volumetric Clouds (§5) — 30 caps · 6C / 9P / 0U / 15M

| # | Capability | § | Status | Evidence |
|---|---|---|---|---|
| B1 | `coverage × profile × shape − erosion` | 5.1 | PARTIAL | implemented `jce_cloud_noise.c:542-563`; the sky march reaches it only through a 2.5D atlas |
| B2 | 2D weather map | 5.1 | MISSING | coverage is one scalar (`jce_scene.h:342`) |
| B3 | Cloud type | 5.1 | PARTIAL | `ctype` drives the gradient `jce_cloud_noise.c:542`; not authorable per region |
| B4 | Height profile + anvil shapes a rendered result | 5.1 | **COMPLETE** | 2026-08-15. The axis swap is fixed, and so is a second defect found beside it. The bake writes slice = world Z, column = world X, row = normalised altitude (`jce_cloud_noise.c:655-667`); the march read altitude into the slice and world Z into the row, so a cloud's vertical structure was laid out along Z and its Z extent along the height gradient. Separately the world-to-tile factor was `0.05` per kilometre — a declared 20 km period over an atlas that covers exactly one 4096 m period — stretching the sky's clouds **4.88x** while the ground sampled the same field at its true size. Measured on `view=sky`, cloud contribution 0.82 vs 0.0: **21.278 → 25.912** grey levels, affected pixels 84.89% → 99.03%, peak 65.3 → 176.3, and the detail the field actually contains arrived with it — mean horizontal gradient of the cloud term **1.24 → 3.87** |
| B5 | Low-frequency 3D shape noise | 5.1 | **COMPLETE** | same swap, fixed with B4 |
| B6 | High-frequency erosion | 5.1 | PARTIAL | continuous in the bake (`jce_cloud_noise.c:555-563`), aliased through the atlas in the sky |
| B7 | Curl / distortion | 5.1 | MISSING | — |
| B8 | Storm / precipitation field | 5.1 | MISSING | `cloud_precipitation` exists (`jce_environment.h:78`), zero readers |
| B9 | Ray ↔ layer intersection | 5.2 | PARTIAL | flat-slab only; camera pinned to ground level (`fs_sky.sc:211-215`), gated `dir.y > 0.02` (`:205`) ⇒ the camera can never be inside cloud |
| B10 | Empty-space skipping | 5.2 | PARTIAL | whole-march skip at `fs_sky.sc:205`; no per-step skip |
| B11 | Early-out on low transmittance | 5.2 | COMPLETE | `fs_sky.sc:303` |
| B12 | Beer-Lambert extinction | 5.3 | COMPLETE | `fs_sky.sc:299` |
| B13 | Henyey-Greenstein phase | 5.3 | COMPLETE | `fs_sky.sc:289-293` |
| B14 | Dual-lobe phase | 5.3 | MISSING | one HG lobe per octave |
| B15 | **Short light march toward the sun (self-shadow)** | 5.3 | **COMPLETE** | 2026-08-15. `li = exp(-sigma*a*dt*4.0)` used the local sample's own density times a 4 that stood in for the distance to the cloud top without measuring it, so a cumulus was as bright underneath as on its sunlit crown. Now six geometrically-growing steps toward the sun accumulate real optical depth, sharing one `cloud_density_at` with the view march so the two cannot address the atlas differently (which is how the axis swap survived). Measured on `view=sky`, cloud term vs no cloud: the change correlates **-0.553** with cloud thickness — thinnest quartile **+7.549** grey levels, thickest 5% **+0.000**. It stops over-darkening thin wisps and leaves thick cloud exactly as it was, which is the whole of a self-shadow. GPU cost measured with `JCE_PERF_LOG`'s per-view breakdown over 35 windows: `Scene/Color` median **4.38 → 4.71 ms (+0.33, +7.5%)** |
| B16 | Multiple-scattering approximation | 5.3 | COMPLETE | Wrenninge 3-octave contrast approx, `fs_sky.sc:226-238, 283-298` |
| B17 | Powder / silver lining, physically constrained | 5.3 | PARTIAL | `cloud_rim` (`fs_sky.sc:311-315`) feeds only the artistic grade `sky_stylise` (`:326-331`), never the radiance integral |
| B18 | Half / quarter resolution | 5.4 | MISSING | full-res sky pass |
| B19 | Jitter / blue noise | 5.4 | PARTIAL | 2026-08-15. The march start is now offset by interleaved gradient noise (Jimenez) — one dot product and a `fract`, no texture, no state. A function of `gl_FragCoord` ALONE, so the dither is identical every frame and the image stays deterministic: a temporal jitter with no TAA to resolve it trades banding for crawling, and TAA is not on in every tier this runs on. Measured on the cloud term, `view=sky`: neighbouring pixels resolving to an identical step **34.38% → 29.17%**, mean local gradient 0.8893 → 0.9372. PARTIAL: this is a fixed dither, not blue noise, and there is no temporal component |
| B20 | Temporal reprojection + history clamp | 5.4 | MISSING | — |
| B21 | Adaptive step count | 5.4 | PARTIAL | `u_cloud_quality.x` budget, compile-time bound 24 (`fs_sky.sc:224-226`) |
| B22 | Depth-aware upscale | 5.4 | MISSING | — |
| B23 | **Clouds move with wind** | 5.1/6.2 | **COMPLETE** 2026-08-15 | the march sampled `wp = dir * t` with **no time term**, so the cloud layer was nailed to the world. A world-space offset now accumulates in `sr_advance_environment_state` from the one wind authority (U4) at `JCE_CLOUD_WIND_FACTOR` = 3.0 (cloud-base wind over surface wind), and **both** consumers subtract it: the sky march through `u_cloud_quality.yz`, the shadow bake through its window centre. Verified against a control that removes the only moving thing: `view=sky`, cloudCoverage 0.82 vs 0.0, five shots 3000 frames apart. Cloud run **0.1082 / 0.2089 / 0.3017** grey levels as the offset reaches 86 / 171 / 256 m; the no-cloud control is **0.0000** at every shot. The B4/B5 scale mismatch was fixed in the same pass, which is why the drift is 2.1-2.7x what it measured before |
| B24 | Cloud shadow texture | 5.5 | COMPLETE | `jce_cloud_shadow.c`; live in `hidden_cove.scene.json`. **Was COMPLETE and never once ran** — the bake call sat four blocks deep inside `if (want_ssao)`, and `d.sun_dir` was negated so `jce_cloud_shadow_bake` refused every frame on its `ny < 0.05` guard. Fixed 2026-08-15; bake now runs from `sr_update_cloud_shadow_frame`, beside the shadow pass. Cost measured and amortised — see B26. **And then it was a binary mask**: the sky march integrates density per KILOMETRE while the bake integrates per METRE, and the bake multiplied by an undocumented `0.46` instead of dividing by 1000 — 460x. At the shipped `cloudDensity` 1.15 the optical depth was 132 at the thinnest density the field produces, so `exp(-tau)` was 0 everywhere there was any cloud at all. Fixed 2026-08-16; ground response mean **-54.10 → -18.24** and the term finally has a penumbra. Guarded by `test_shipping_parameters_do_not_saturate`, which mutation-fails at `only 0 of 1024 texels are partial` |
| B25 | Cloud shadow → meshes / buildings | 5.5 | PARTIAL | rides the SSAO target's blue channel (`fs_ssao.sc` → `fs_ssao_blur.sc` → `fs_pbr_body.sh`), so meshes have cloud shadow **only when SSAO is on** — and the viewport pipeline has it off even where the scene sets `ssaoEnabled: true`. **This is an architectural constraint, not an oversight, and it was briefly mis-marked COMPLETE on 2026-08-15**: `fs_pbr_body.sh` occupies all sixteen sampler stages and uses 3 for `s_aoMap`, so the CPU cloud map cannot be bound for meshes without overwriting every mesh's AO map with a top-down transmittance read by mesh UV. Lifting this needs a stage, i.e. the CSM cascades packed into an array or atlas. *(Correction: an earlier revision of this row blamed `.rp.json` not reaching the viewport. That is not the cause. `want_ssao` is `scene->ssao_enabled && feature_enabled("ssao")`, and it measured 0 with the feature FORCED on — because the envshot `--view` presets take the `JCE_DBG_VISTA` path, which bypasses the scene's render settings entirely. That is a property of the measurement harness, not of the engine; `JCE_FORCE_SSAO=1` exists precisely for it.)* |
| B26 | **Cloud shadow → terrain** | 5.5 | **COMPLETE** | 2026-08-15. The "no free slot" reading was wrong in a useful way: stage 3 held `s_aoMap`, *declared and never bound or read* on the terrain path, which is not an occupied slot but a hazard. It now carries `s_cloudShadow` (`fs_terrain.sc:57`), bound by `sr_bind_terrain_cloud_shadow` on **both** draw paths — the chunked one, which is the one that actually runs, had no bind at all. Measured on `hidden_cove`, cloudCoverage 0.82 vs 0.0, noise floor 0.0008: ground response **0.0020 → 19.6455** grey levels, 44.29% of ground pixels moving more than 8 |
| B27 | Cloud shadow → vegetation | 5.5 | **COMPLETE** | 2026-08-15, via the shared `cloud_shadow.sh`. Grass folds it into `csmSh` so it multiplies only the shadow-casting directional light; foliage multiplies the whole ramp, because that shader has **no separable direct-light term and samples no shadow map at all** — a cloud is the one occluder it can honestly carry. Attributed by neutralising the new lines to `* 1.0` and rebuilding: view `forest` **12.8800 → 39.6342** grey levels (3.08x), `canopy` 18.4977 → 26.3474 |
| B28 | Cloud shadow → water | 5.5 | **COMPLETE** | 2026-08-15. Folded into the same shadow scalar the sun contribution uses, so it dims glint and diffuse alike and leaves the sky reflection to the sky. `s_water_caustic_disp` moved off stage 3 to make room — terrain can free only stage 3, so 3 is the stage everything else must match. View `bay` **17.9717 → 23.2569** (1.29x), the mildest of the four, which is right: water is mostly reflecting a sky that is already darker under its own cloud |
| B29 | Nubis Evolved (camera in cloud, superstorm, internal lightning) | 5.6 | MISSING | — |
| B30 | Nubis Cubed (voxel / SDF) | 5.6 | MISSING | explicitly non-blocking |

### Area C — Weather / Rain / Snow / Lightning / Wetness / Shadows (§6, §7, §8) — 23 caps · 5C / 7P / 1U / 10M

| # | Capability | § | Status | Evidence |
|---|---|---|---|---|
| C1 | WeatherPreset asset (11 fields) | 6.1 | MISSING | only `weather_type` + `weather_intensity` (`jce_scene.h:318-319`) |
| C2 | 8 presets | 6.1 | MISSING | 3 types (`jce_weather.h:27-31`) |
| C3 | Transition: response time / curve / delay / seed per parameter | 6.2 | PARTIAL | env integrates wetness (τ 30 s) and snow (τ 240 s melt) `jce_environment.c:196-224`; every other parameter snaps |
| C4 | Storm cascade (13 coupled responses) | 6.2 | MISSING | — |
| C5 | Weather controller / timeline / script API | 6.2 | PARTIAL | working Lua path: `jce_script.c:901`/`:1209` → `jce_rt_script.c:1216`/`:944-948` → `jce_scene_components_json.c:3364-3379` (serialize→merge→`extract_rendering_settings_from_obj:772-774`) → `jce_scene.c:566-573`; renderer re-reads live at `jce_scene_renderer.c:3186`/`:4889`. Only **2 fields**; no controller object; sequencer has no property track (`jce_sequencer.h:28-31`) |
| C6 | Near-camera rain streaks | 7.1 | PARTIAL | screen-space overlay only (`jce_weather.c:117-140`, `fs_weather.sc`) |
| C7 | Mid-range GPU rain particles | 7.1 | MISSING | — |
| C8 | Far screen/volume approximation | 7.1 | PARTIAL | the overlay *is* the far approximation — applied at all ranges |
| C9 | Ground splashes | 7.1 | MISSING | only STYLIZED-water `splash_ratio` |
| C10 | Rain → water ripple events | 7.1 | MISSING | — |
| C11 | Wetness accumulation | 7.1 | COMPLETE | `jce_environment.c:196-206` (state side only) |
| C12 | Snow flakes | 7.2 | COMPLETE | `fs_weather.sc`, mode 1 |
| C13 | Snow / rain wind drift | 7.2 | **COMPLETE** | 2026-08-15. U4 restored the speed; the direction got its first writer today. `environment.wind.directionX/Z` is parsed, serialised and written into `sr->env.wind_direction_ws` before the state advances. On `hidden_cove_rain` the drift was **0.1500** and is now **0.0294** — and the drop is the fix, not a regression: 0.1500 came from `jce_environment_default`'s (1,0,0), which nothing had ever overwritten, while this cove's grass and ocean have both always said (0.2, 1.0). Overriding to (-1, 0) gives -0.1500 and turns the cloud drift with it, so both consumers follow one authority |
| C14 | Snow material mask / accumulation | 7.2 | PARTIAL | 2026-08-15. `snow_amount` had zero readers — and would have stayed at zero anyway, because the environment accumulates it only below `ENV_SNOW_SETTLE_C` = 1 C and melts it every frame above, while **nothing in the repository ever wrote `temperature_c`**: every scene sat at the 15 C struct default, so lying snow was unreachable by construction in an engine that renders snowfall. Both halves landed together — `environment.temperatureC` is now authored, parsed, serialised, editor-exposed and written, and `wetness.sh` gained `snow_coverage/snow_albedo/snow_roughness`, weighted by `smoothstep(0.35, 0.85, N.y)` (snow lies on what holds it; a wall gets none) toward albedo 0.90 rather than pure white, with roughness UP because snow is a rough dielectric. New test map `hidden_cove_snow` (type 2, -5 C) from the generator. Measured at `snow_amount` 0.320: view `slope` **38.02% of pixels brighter**, 0.05% darker, mean +2.602, peak +51. PARTIAL: no accumulation MASK (it is a global scalar, so snow appears on sheltered surfaces too), no depth, no displacement, and grass/foliage/water do not read it |
| C15 | Footprints | 7.2 | MISSING | plan defers |
| C16 | Lightning (bolt path, flash, cloud illumination, thunder) | 7.3 | MISSING | `grep -i lightning` over engine+editor = 2 unrelated comments |
| C17 | Unified wetness = weather + proximity + puddle mask | 7.4 | PARTIAL | only the weather term exists |
| C18 | **Material wetness response** | 7.4 | PARTIAL | 2026-08-15. Was MISSING with zero hits in any lit shader, while `global_wetness` had been integrated by the environment since it was written and read by nothing but the rain overlay's own tint. `engine/shaders/pbr/wetness.sh` now darkens albedo (x0.7 at full wet, the usual mid-porosity dielectric figure) and multiplies roughness (x0.35), weighted by `0.25 + 0.75 * saturate(N.y)` — rain falls down, and the floor is named in the header as a stand-in for wind-driven rain and runoff rather than passed off as physics. Wired into terrain and `fs_pbr_body.sh` from one include, so ground and the things on it cannot disagree. Measured on `hidden_cove_rain` at wetness 0.895, against the same scene with the two calls neutralised: `slope` **36.03% of pixels darker**, 0.83% brighter; `canopy` 55.67% / 0.78%; `bay` 27.24% / 27.63% (water reflects a tightened specular back). PARTIAL: no per-material porosity, no puddles, no clearcoat, no normal flattening, and grass/foliage/water still do not read it |
| C19 | 4-cascade CSM, stable, texel snap, PCF, bias | 8 | COMPLETE | `jce_csm.h:20-63` |
| C20 | Per-cascade culling | 8 | COMPLETE | `jce_csm.c` |
| C21 | Vegetation shadow distance | 8 | PARTIAL | grass is non-casting by design (`jce_scene.h` `cast_shadow` "v1: ignored") |
| C22 | Cached far cascade | 8 | COMPLETE | static shadow cache with per-cascade dynamic gate |
| C23 | Moon shadows / moon as key light | 8 | PARTIAL | U3 landed `4a039cfe`, gated on `tod_active`. **Visible effect is zero, and that is the finding**: night 0.004% → 0.000%, day 4.690%. There is 506x less occludable directional energy at night because `jce_time_of_day.c` gives the night a moon *tint* on the sun term and never a moon with radiance of its own. Shadows need a light |

### Area D — Terrain (§10-18) — 31 caps · 8C / 10P / 0U / 13M

| # | Capability | § | Status | Evidence |
|---|---|---|---|---|
| D1 | Tiled heightfield is the authority | 10 | PARTIAL | `jce_terrain_create_tiled` (`jce_terrain.h:81`); `"procedural": true` in an ordinary `.terrain.json` reaches it (`jce_terrain.c:578-586` → `:393`), and the editor Load button gets there (`jce_panel_terrain.cpp:248-252`). **No shipped asset sets it** |
| D2 | Streaming resident tiles + prefetch | 11 | COMPLETE | `jce_terrain.h:102,126`; live path `jce_sr_terrain.c:489-490, 516-525` inside `sr_draw_terrain_chunks:445` |
| D3 | Chunk / quadtree LOD | 11 | COMPLETE | `jce_terrain_chunk_build_mesh(..., lod, ...)` `jce_terrain.h:309` |
| D4 | Geometry clipmap | 11 | MISSING | plan permits deferral |
| D5 | LOD morph transition | 11 | MISSING | ⇒ visible pop |
| D6 | GPU height-texture sampling | 11 | MISSING | CPU-built chunk meshes |
| D7 | LOD cracks / tile border / seams | 11 | PARTIAL | skirt-style handling; §11 says "High 不接受明显 skirt 伪影" |
| D8 | Normal / slope derived texture | 11 | PARTIAL | normals per vertex; no derived slope map |
| D9 | Sculpt: raise / lower / smooth / flatten | 12 | COMPLETE | `jce_terrain.h:47-52` |
| D10 | Sculpt: terrace / ramp / noise / stamp / set-height / copy-paste | 12 | MISSING | 4 of 10 modes exist |
| D11 | Brush: hardness / alpha / rotation / spacing / jitter | 12 | PARTIAL | radius/strength/falloff only |
| D12 | Sculpt dirty cascade | 12 | PARTIAL | `jce_scene.h:3052-3057` invalidates renderer + pick + physics; **nav, foliage, biome not in the chain** |
| D13 | **Undo: affected tiles / dirty rect / compressed delta** | 13 | MISSING | `jce_panel_terrain.cpp:124-137` snapshots the **whole** heights+splat arrays, 32 deep (`kTerrainUndoLimit:119`) — literally what §13 forbids |
| D14 | Undo as an editor Command | 13/46 | MISSING | panel-local `std::vector` stacks (`:120-122`), outside the scene-snapshot undo — documented at `:106-112` |
| D15 | Erosion async / preview / cancel / one command | 13 | MISSING | runs inline in the `ImGui::Button` body, `jce_panel_terrain.cpp:479-546` |
| D16 | Procedural generation node set (11 ops) | 14 | PARTIAL | `jce_terrain_create_procedural` takes seed + frequency (`jce_terrain.h:94`); no node graph |
| D17 | **Hydraulic erosion** (water/sediment/velocity grid; rain→flow→capacity→transport→deposition→evaporation) | 15 | MISSING | what ships is a **pointwise gully filter**: `jce_phacelle.h:49-56` — *"THERE IS NO HYDROLOGICAL CONNECTIVITY… No river network, flow accumulation, watershed, drainage-based biome or moisture map may be derived from this output."* Params are octaves/frequency/strength/detail (`jce_terrain.h:179-188`) |
| D18 | Derived maps: flow / flow dir / sediment / deposition / wear / water accumulation | 15 | MISSING | the only output is `out_ridge` (`jce_terrain.h:190-192`) |
| D19 | Derived maps feed rivers / materials / mud / rock / vegetation / debris | 15 | MISSING | consequence of D18 |
| D20 | Thermal / talus erosion | 16 | COMPLETE | `jce_terrain.h:217-224`, mass-conserving and tested |
| D21 | Hydraulic + thermal chainable | 16 | PARTIAL | chainable, but the "hydraulic" half is not hydraulic |
| D22 | Material inputs: height/slope/curvature/flow/sediment/wetness/biome/weights | 17 | PARTIAL | height + splat weights only |
| D23 | Layers: rock/dirt/mud/grass/sand/snow/ruins | 17 | PARTIAL | 3 layer textures + splat; **all 16 sampler stages already occupied** (`fs_terrain.sc:166-168`) |
| D24 | Triplanar steep / cheap flat / macro colour / detail normal / distance LOD | 17 | PARTIAL | — |
| D25 | Sky-occlusion bake (visibility + bent normals) | 9/17 | COMPLETE | `jce_terrain.h:252-261` |
| D26 | Spline modifiers: flatten/carve/bank/shoulder/paint/exclusion | 18 | MISSING | — |
| D27 | Auto vegetation exclusion from roads/buildings | 18 | MISSING | — |
| D28 | Terrain collision mesh | 10 | COMPLETE | `jce_terrain.h:274` |
| D29 | Heightmap import / export (R16 + file) | 41 | COMPLETE | `jce_terrain.h:360-388` |
| D30 | Terrain holes | — | COMPLETE | `jce_terrain.h:338-345` |
| D31 | **Erosion works on tiled / streamed terrain** | 15 | MISSING | `apply_erosion`, `apply_thermal`, `bake_sky_occlusion` all *"Return false, touching nothing, for a terrain with no resident height grid (tiled/procedural)"* (`jce_terrain.h:176-177, 214-215, 250-251`); editor refuses at `jce_panel_terrain.cpp:499-506` |

**OPEN, 2026-08-16 — terrain may not reach the shadow caster set at all.**
`sr_try_submit_terrain_shadow` is the ONLY thing that puts terrain into a shadow view
(`sr_draw_terrain_chunks` has one caller, the colour pass). Instrumented with a counter at
the function's first line, it is entered **0 times per frame** on `hidden_cove` — on every
frame including frame 0, so it is not the static shadow cache. A second counter at the
cascade caster ladder (`jce_sr_shadow.c:2017`) also reads 0, so that loop is not the one
this scene runs; the terrain entity is filtered out before the probe on whichever path is.
Terrain classifies as `SR_RK_OTHER` (so `kc_fast` is false and the probe is not skipped for
that reason) and `sr_entity_casts_shadow` returns true for it, so neither obvious filter
explains it. **This contradicts an earlier session's measurement that the terrain shadows
itself**, so either that path changed or the two scenes differ. Not resolved; recorded with
the counters rather than guessed at.

Separately, that submit loop had **no frustum test of any kind** — every chunk into every
cascade and every local-light view, 256 chunks a cascade on this scene's 16x16 grid. The
caller's per-caster cull cannot cover it: `sr_shadow_caster_aabb` returns false for terrain
by design, so `has_aabb` is false and the cascade reject never fires. A cull is now in place
(the caller passes the shadow view's matrix, or NULL for the paths that have none, because a
cull against the wrong frustum deletes shadows that should be there) — but with the submit
never reached in this scene, **it has not been exercised and no cost was measured.**

**Terrain had no ambient occlusion of any kind until 2026-08-15.** `fs_terrain.sc:318` read
`float ao = mix(1.0, 1.0, u_pbrParams.z)` — a constant 1.0 wearing the shape of a blend, for
every value of the `aoStrength` it appears to honour. Not a texture, not SSAO, nothing, in the
shader whose whole subject is ground that other things stand on. The stage it needed was there
all along: `s_metalRough` at 1 and `s_normalMap` at 2 are both **declared and never read** on
this path (terrain takes metallic and roughness from `u_pbrParams`), so "all sixteen stages are
occupied" was true only on paper. Stage 1 now carries the screen-space AO target, sampled by
screen UV exactly as `fs_pbr_body.sh` does it, so the ground and the things standing on it
darken by the same rule at the same contact.

Measured by neutralising the new read and rebuilding — what turning SSAO on and off changes:

| view | meshes only | + terrain |
|---|---|---|
| slope | 0.0046 (0.02% of pixels) | **0.1528 (1.88%)** |
| canopy | 0.0009 (0.00%) | **0.1762 (2.15%)** |
| bay | 0.0005 (0.00%) | **0.4129 (6.97%)** |

SSAO's entire visible contribution in these scenes was terrain, and terrain was the one surface
that could not receive it.

**D1 × D31 is a structural contradiction:** the plan's authoritative data format (tiled) is the one format none of the editing tools accept.

### Area E — Water (§19-29) — 40 caps · 11C / 13P / 0U / 16M

| # | Capability | § | Status | Evidence |
|---|---|---|---|---|
| E1 | Body types Ocean / Lake / River / Pond | 19 | MISSING | one `JceWaterComponent` = a rectangular plane (`jce_scene.h:1175-1179`); repo-wide grep for river/lake/water_body/body_id = 0 hits |
| E2 | Per-body shape / spline | 19-20 | MISSING | — |
| E3 | Base level / depth | 19 | PARTIAL | `base_height` only — and until 2026-08-15 the CPU field did not even have that right. `base_height` is ENTITY-LOCAL and the renderer published it to `JceWaterFieldDesc` unconverted, so on `hidden_cove` the field said **0.000** while the water was drawn at **14.992**, with `center_x/center_z` left at (0,0) against a body centred at (0,-40). Every CPU query — buoyancy, shoreline, gameplay — read that. Fixed by making the scene fill the desc from the entity, so no caller does the local-to-world mapping and no caller can get it wrong |
| E4 | Flow | 19 | MISSING | — |
| E5 | Wave asset | 19/22 | PARTIAL | waves inline in the component, max 4 (`jce_scene.h:1156-1179`); no shared asset |
| E6 | Underwater profile per body | 19/28 | PARTIAL | `jce_underwater.c` is global |
| E7 | Shoreline | 19/25 | PARTIAL | `jce_water_shoreline.c` + `shore_foam_m`/`shore_surge_s` (`jce_scene.h:1206-1208`) |
| E8 | Physics | 19/27 | COMPLETE | `JceBuoyancyComponent` (`jce_scene.h:1276-1281`) |
| E9 | Ocean world level + boundary + exclusion islands | 20 | MISSING | — |
| E10 | Lake polygon spline + shore falloff | 20 | MISSING | — |
| E11 | River spline / width / depth / bank / flow / carve | 20 | MISSING | — |
| E12 | Pond | 20 | PARTIAL | a small plane |
| E13 | Water debug (tiles/depth/flow arrows/shore distance/body id) | 20 | MISSING | — |
| E14 | Shared tiled water zone, camera-centred LOD | 21 | PARTIAL | ring mesh (`jce_water.h:154-166`) + `ocean` flag; still one mesh per component |
| E15 | Body mask / info map | 21 | MISSING | STYLIZED `data_tex` is per-body shore distance only |
| E16 | Gerstner wave param set | 22 | PARTIAL | per-wave fields present; no spread/seed generator |
| E17 | **CPU surface query returning height/normal/velocity/body id/depth/flow** | 22 | PARTIAL | `JceWaterSample` = position + normal + jacobian (`jce_water_field.h:56-62`). **velocity, body id, flow absent; `depth` was deliberately removed** with a note that it could never hold a value (`jce_water_field.h:65-72`). The height it does return was **wrong by 15 m** until 2026-08-15 — see E3 |
| E18 | One clock for render + physics | 22 | COMPLETE | `jce_water_field.h:1-33` — the module exists to kill the two-clock bug |
| E19 | FFT ocean (Tessendorf) | 23 | COMPLETE | `jce_water_fft.c`; `JCE_WATER_MODE_FFT` |
| E20 | JONSWAP fetch + swell | 23 | COMPLETE | `jce_ocean_spectrum.c`; `fft_fetch`/`fft_swell` |
| E21 | Multi-cascade non-commensurate patches | 23 | COMPLETE | `jce_water_field.h:118-140` |
| E22 | Crest / foam metric | 23/25 | COMPLETE | jacobian < 0 ⇒ whitecap (`jce_water_field.h:60-62`) |
| E23 | Fresnel | 24 | COMPLETE | `fs_water.sc:339-341` |
| E24 | Roughness | 24 | MISSING | no roughness input on water |
| E25 | Depth absorption (Beer-Lambert) | 24 | COMPLETE | `clarity` (`jce_scene.h:1185-1195`), `fs_water.sc:64` |
| E26 | Scattering | 24 | MISSING | — |
| E27 | Planar reflection | 24 | MISSING | `fs_water.sc:11` *"planar reflection … explicit FOLLOW-UPS and are intentionally NOT implemented here"* |
| E28 | SSR on water | 24 | PARTIAL | `jce_ssr.c` exists; water does not write depth by default (`jce_scene.h:1265-1274`) so SSR sees through it |
| E29 | Probe / sky reflection fallback | 24 | COMPLETE | `s_prefilter` cube stage 7, `fs_water.sc:33-34` |
| E30 | Refraction | 24 | MISSING | — |
| E31 | Multi-scale normal | 24 | PARTIAL | — |
| E32 | Foam sources (crest/shore/obstacle/wake/waterfall/authored) | 25 | PARTIAL | crest + shore only |
| E33 | Caustics | 25 | COMPLETE | `jce_water_caustics.c` + `fs_water.sc:86-141`, same Jacobian as the foam |
| E34 | Shore: attenuation / wet bank / debris / vegetation exclusion | 25 | MISSING | foam + surge only |
| E35 | Sun sparkle | 24 | PARTIAL | one specular lobe |
| E36 | Local shallow-water sim (h, vx, vz; propagation/damping/boundary/impulse) | 26 | MISSING | `grep -i shallow` finds only colour names and the TMA depth factor |
| E37 | Footstep ripple / wake / explosion impulses | 26 | MISSING | — |
| E38 | Buoyancy probes → submersion → force + drag | 27 | COMPLETE | `jce_scene.h:1276-1281` |
| E39 | Underwater: absorption/scattering/fog/caustics/underside/bubbles/audio LPF/hysteresis | 28 | PARTIAL | `jce_underwater.c/.h` cover the optical set |
| E40 | HFW offline breaking-wave → compact wavefront | 29 | MISSING | explicitly long-term |

**Water is the strongest area in the engine and the weakest against this plan.** The *surface physics* is genuinely production-grade (E18-E22 are things most engines do not have). The *authoring model* — bodies, splines, rivers, flow, masks — does not exist at all.

### Area F — Vegetation / PCG / Biome (§30-38) — 30 caps · 8C / 7P / 1U / 14M

| # | Capability | § | Status | Evidence |
|---|---|---|---|---|
| F1 | Foliage Type asset (15 fields) | 30 | PARTIAL | `JceVegetationScatterComponent` (`jce_scene.h:1081-1108`) covers mesh/density/seed/slope/scale/align/tint. No variants, spacing, height range, wetness range, biome mask, per-type wind, interaction, collision policy. It is a **component**, not an asset |
| F2 | Scatter inputs: position / height / normal / slope | 31 | COMPLETE | `jce_foliage.c` over the bound terrain |
| F3 | Scatter inputs: curvature / layer / flow / sediment / wetness / dist-to-water / dist-to-road / dist-to-building / biome | 31 | MISSING | none exist; several depend on D18 |
| F4 | Artist mask | 31 | COMPLETE | `density_mask_path` + 64² `density_paint` (`jce_scene.h:1085-1108`) |
| F5 | Noise input | 31 | PARTIAL | — |
| F6-F11 | PCG graph: sources / sampling / attribute / filters / transform / outputs | 32 | MISSING ×6 | there is **no PCG anywhere**: `grep -rni "\bpcg\b"` hits only PCG32 RNG comments (`jce_rand.c:4`, `jce_water_fft.c:106`, `jce_cloud_noise.c:99`) |
| F12 | PCG determinism (graph version + input + seed + cell id) | 33 | PARTIAL | the scatter is seed-deterministic and re-entry stable; there is no graph and no cell id |
| F13 | Bake path (hero trees / ruins / composition) | 34 | MISSING | — |
| F14 | Runtime path (grass / flowers / rocks) | 34 | COMPLETE | grass field + scatter rebuild on param change |
| F15 | Hybrid | 34 | MISSING | — |
| F16 | Instancing + compact per-instance data | 35 | COMPLETE | one instanced submit per grass field |
| F17 | GPU frustum culling for foliage | 35 | PARTIAL | `jce_gpu_scene.c` serves the general scene; foliage uses the CPU cell path |
| F18 | Hi-Z occlusion | 35 | COMPLETE | foliage Hi-Z default-on |
| F19 | Draw indirect | 35 | PARTIAL | `jce_gpu_scene.c:596-638` with a documented 1:1 fallback; not on the foliage path |
| F20 | Foliage LOD | 35 | PARTIAL | distance fade (`fade_start`/`fade_end`), not mesh LOD |
| F21 | Impostor far distance | 35 | COMPLETE | `jce_impostor.h:91-124` |
| F22 | CPU fallback (cell cull, CPU LOD, instancing, lower density) | 35 | COMPLETE | — |
| F23 | Wind: global trunk bend | 36 | COMPLETE | `vs_grass.sc:73-94` travelling wave + gust envelope |
| F24 | Wind: branch motion + leaf flutter | 36 | MISSING | one bend angle (`vs_grass.sc:92`) |
| F25 | Vertex branch / leaf masks | 36 | MISSING | — |
| F26 | Per-instance phase | 36 | PARTIAL | spatial phase jitter from world position (`vs_grass.sc:88`), not per instance |
| F27 | Environment wind + gust drive vegetation | 36 | PARTIAL | U4 landed `21e38944`. Grass and ocean now read `jce_environment_wind_speed_now` as a multiplier over the authored value; before it there were **three** winds (ocean 7.5, grass 1.6 ignoring the weather, ENV 0 that nobody could read) with contradictory directions. Clear reads grass 1.600 / ocean 7.500, identical to before the migration; rain 4.800 / 22.500. PARTIAL because grass and ocean still carry their own authored direction rather than reading the environment's. The environment now HAS an authored direction (see C13), which is what makes that migration legitimate — before today it would have meant replacing an authored value with a struct default |
| F28 | Local interaction (bend field, impulse, recovery) | 37 | MISSING | — |
| F29 | Biome asset | 38 | MISSING | zero hits |
| F30 | Biome paint / procedural / hybrid | 38 | MISSING | — |

### Area G — Editor / Pipeline / Delivery (§2, §39-54) — 30 caps · 3C / 17P / 0U / 10M

| # | Capability | § | Status | Evidence |
|---|---|---|---|---|
| G1 | **Single authoritative `JceEnvironmentState`** | 2 | PARTIAL | owned at `jce_sr_internal.h:890`, built `jce_scene_renderer.c:1460`, advanced once/frame `jce_sr_environment.c:3980-3984`, read back `:4017-4019`, on the live path via `jce_scene_renderer.c:4893`. **But**: it lives on `JceSceneRenderer` not `JceScene`; absent from `api_world.h:21-29`; never ticked headless; grass and ocean winds do not read it |
| G2 | Editor edits the same data model | 2 | MISSING | no environment panel; the only writers are `rs->weather_type/intensity` and `sr->cloud_*` |
| G3 | Controller / timeline / script drive `EnvironmentState` **only** | 2 | PARTIAL | `jce.render_set` writes scene *rendering settings*, which the renderer then copies into env — the inverse of the mandated direction |
| G4 | Headless keeps env, loads no GPU env resources | 2/51 | PARTIAL | the advance is deliberately above the `!sr->pak` guard (`jce_sr_environment.c:3986`), but `sr_drive_weather` is renderer-only ⇒ a build with no scene renderer never ticks it |
| G5 | World workspace (Environment/Terrain/Water/Foliage/PCG/Biome/Debug) | 39 | MISSING | 99 panels; only `jce_panel_terrain.cpp` and `jce_panel_world_streaming.cpp` are world tools |
| G6 | Environment panel (12 controls) | 40 | PARTIAL | cloud coverage/density/layer at `jce_panel_lighting_settings.cpp:221-239`; fog at `:541-585`; time of day in `jce_panel_time_of_day.cpp`. **Zero** weather / wind / wetness / lightning / atmosphere controls — `grep -rln weather editor/src` returns exactly one file, and it is the fog section |
| G7 | Terrain Mode tabs (Manage/Sculpt/Paint/Erosion/Spline/Debug) | 41 | PARTIAL | 4 of 6; no spline |
| G8 | Water Mode | 42 | MISSING | — |
| G9 | Foliage / PCG Mode | 43 | PARTIAL | inspector density brush only |
| G10 | Editor + Runtime share one PCG core | 43 | MISSING | no PCG |
| G11 | Source asset layer (10 formats) | 44/47 | PARTIAL | only `.terrain.json` exists; the other nine return zero hits repo-wide |
| G12 | Derived cache: deletable / rebuildable / source-hashed / non-authoritative | 44 | PARTIAL | it exists and ships: `jce_ibl.c:584-585` FNV-1a-64 over source pixels + bake params, `:599-657` `.jce/cache/ibl_%016llx.bin` load/store, `:689-705` hit-or-recompute, design stated `:569-579`; second instance `:207-235`; reached from `jce_sr_environment.c:3697` and `jce_scene_renderer.c:2089`. **No shared manager, no terrain/PCG/HLOD registrant** |
| G13 | Cooked env formats (`.jce_env` …) | 44/47 | MISSING | — |
| G14 | Dirty dependency graph | 45 | PARTIAL | `jce_scene.h:3052-3057` covers renderer/pick/physics; nav, water shore, foliage cells, biome, HLOD are in no graph |
| G15 | Derived rebuilds async / prioritized / cancellable | 45 | PARTIAL | already true for the IBL bake: `jce_sr_environment.c:3777-3784` (`JCE_ASYNC_PRIORITY_LOW`, `"scene.ibl.bake"`), cancel honoured `:3691-3692`, `:3702-3706`, cancelled on rapid switch `:3766`, drained `:3714-3745`. Terrain erosion/thermal/sky-occlusion still run inline on the UI thread (`jce_panel_terrain.cpp:479-546`) |
| G16 | World tools as editor Commands (6 types) | 46 | MISSING | terrain uses panel-local stacks (`jce_panel_terrain.cpp:106-122`); the other five do not exist |
| G17 | RenderGraph order per §48 | 48 | PARTIAL | `jce_render_graph.c` + `jce_scene_renderer_view_order.c` exist; steps 3, 4, 9, 10, 11 have no nodes |
| G18 | Runtime caps branching, never "no compute ⇒ no environment" | 49 | COMPLETE | `jce_gpu_caps.c:32`, `jce_gpu_scene.c:596-638`, `jce_renderer_caps.c:96` |
| G19 | LOW tier | 50 | PARTIAL | — |
| G20 | MEDIUM tier (froxel fog + low-res volumetric cloud) | 50 | MISSING | neither exists (A11, B18) |
| G21 | HIGH tier | 50 | PARTIAL | — |
| G22 | CINEMATIC tier | 50 | MISSING | — |
| G23 | Headless terrain height query | 51 | COMPLETE | `jce_terrain.h:283` |
| G24 | Headless water surface query | 51 | COMPLETE | CPU-canonical, zero readback (`jce_water_field.h:26-33`) |
| G25 | Headless deterministic weather | 51 | PARTIAL | the advance is pure and tested; only the renderer calls it (G4) |
| G26 | `samples/world_environment_lab/` (9 sub-scenes) | 52 | MISSING | **there is no `samples/` directory at all** |
| G27 | Visual regression harness (fixed camera/res/weather/seed/quality/exposure/backend + golden + RMS/SSIM) | 53 | PARTIAL | `tools/visual_diff.py`, `tools/render_parity.py` exist; no environment goldens, no fixed-weather harness |
| G28 | Logical determinism (env state, PCG placement, terrain source, weather seed) | 53 | PARTIAL | env advance deterministic + tested; `weather_seed` (`jce_environment.h:95`) has zero readers; no PCG |
| G29 | Per-subsystem budget capture (P50/P95/P99/stall/GPU mem/tiles/instances/uploads) | 54 | PARTIAL | `JCE_PERF_LOG`/`perf_phase` exist engine-wide; no per-env budget table or counters |
| G30 | **Environment state serialization (save / reopen)** | 57 | MISSING | `JceEnvironmentState` is never written to or read from any `.scene.json`. `humidity`, `fog_density`, `temperature_c`, `wind_*`, `weather_seed`, `moon_*` have **no serializer**. Under §57 ("Editor 不能保存" = not done) this alone blocks any §2 completion claim |

---

## 3. The critical path: how far is §2 from being real?

**Distance: one third of the way, and the remaining two thirds are the hard two thirds.**

What §2 mandates: one authoritative state; sky, cloud, fog, water, vegetation *read* it; none keeps its own weather; the editor edits the same model; controllers/timelines/scripts drive *only* it; headless keeps it without GPU resources.

What exists (`9b96578e`, verified above):
- The struct, complete, sanitized, integrated, framerate-independent, pure in `(state, dt)`, unit-tested — `jce_environment.h:52-96`, `jce_environment.c`, `tests/middleware/world/test_jce_environment.c`.
- **One** owner, ticked once per frame, on the live render path, deliberately above both draw gates — `jce_sr_internal.h:890`, `jce_scene_renderer.c:1460`, `jce_sr_environment.c:3980-3984`, `jce_scene_renderer.c:4893`.
- **One** consumer: the weather overlay (`:4017-4019`).

What is not there, in order of how much it blocks:

1. **It has no writer and no serializer (G30, G2).** Eleven of its twenty-three fields hold their construction defaults forever. A state nobody can author and nobody can save is not an authority; it is a cache. This is also what produced the §0 regression. **Nothing else in §2 is worth doing before this** — every consumer wired to a field that is permanently 0 makes the picture worse, not better, and does so silently.
2. **The data flows backwards (G3).** The mandate is `script/timeline → EnvironmentState → subsystems`. Today it is `script → JceSceneRenderingSettings → (renderer copies 4 fields into) env → overlay`. The proven Lua path (`jce_script.c:901` → … → `jce_scene.c:566-573`) is real and is the right *transport*; its *destination* is wrong.
3. **It lives on the wrong object (G1, G4).** `JceSceneRenderer` is a renderer. §51 requires the state to run headless with no GPU environment resources; `sr_drive_weather` is renderer-only, so today a dedicated-server build never advances the environment at all. It has to move to `JceScene` (or the runtime) and be published through `api_world.h`.
4. **Four subsystems still keep their own weather** — the exact thing §2 forbids: grass wind (`jce_sr_environment.c:1910-1911`), ocean wind (`:2414-2415`), cloud coverage/density (scene settings, copied *into* env at `:3982-3983` rather than read *from* it), fog (`jce_scene_renderer.c:4928`).

**Ordering verdict — §2 must land before Phases 1-4, and cannot fully land before Phase 6.**
- Before: §3 atmosphere, §4 fog, §5 clouds and §6-7 weather each need sun/moon/humidity/wind/coverage *from somewhere*. If §2 is not the somewhere, each will grow its own field — and then §2 becomes a four-way migration instead of a four-way wiring. The evidence that this is not hypothetical: the commit message for `9b96578e` names three winds that already existed, and the §2 header (`jce_environment.h:10-18`) names six partial descriptions.
- After: the *editor* half of §2 (G2) needs a panel, and the panel needs the Environment mode of the World workspace (§39, G5), which is Phase 6 work. So §2 splits: **§2a (writer + serializer + move off the renderer + wire the four consumers) is Phase 1's prerequisite; §2b (editor authoring surface) rides with Phase 6.**

---

## 4. Adapted Phase order

The plan's Phase 1-12 is a correct *dependency* order for a greenfield engine. JCE is not greenfield: some phases are 80% done, two are near-zero, and two of the plan's orderings are actively wrong against the evidence. Below, `[plan Pn]` marks where each item sits in the original.

**Phase A — §2a: make the environment state authoritative and saveable.** `[plan P0/P2 — promoted]`
Writer (weather preset → env), serializer in `.scene.json`, move `env` from `JceSceneRenderer` to `JceScene`, publish in `api_world.h`, tick from the runtime so headless works, then wire U1/U2/U3/U4 and fix the §0 wind regression. **Why first, against the plan's implicit "atmosphere first":** the struct already exists and is already ticking, so this is wiring, not building; and every later phase needs a place to put its inputs. Blocks: everything.

**Phase B — cloud correctness (no new features).** `[plan P3 — promoted, halved]`
Four defects, all with exact locations, all cheap relative to their visual weight: the atlas axis swap (B4/B5/B6 — bake `jce_cloud_noise.c:635, 645-648` vs shader `fs_sky.sc:252, 257, 263`); wind advection (B23 — the march has no time term, `fs_sky.sc:246-252`); the missing sun light march (B15 — `fs_sky.sc:294-295`); cloud shadows reaching terrain and vegetation (B26/B27 — needs a freed sampler stage or a per-vertex term, `fs_terrain.sc:166-168`). **Why before atmosphere, against plan P1 < P3:** the cloud *machinery* is already built and already on screen in a shipped scene (`hidden_cove.scene.json:71999`); the profile code is already correct in the shadow bake. This is the highest visible-quality-per-hour work in the plan, and it needs nothing from atmosphere. The plan puts it third because it assumes clouds don't exist yet. They do.

**Phase C — atmosphere completion.** `[plan P1]`
Multi-scattering LUT, sky-view LUT, aerial-perspective volume, EV100 exposure, one radiometric convention. The transmittance LUT is already CPU-baked, uploaded and sampled (A1/A2), so this extends a working chain rather than starting one. Note A5: "aerial perspective" currently means height fog reusing `fog_*` (`jce_scene.h:400-401`) — replacing it changes every existing scene's distance look, so it needs Phase J's goldens or it will be un-reviewable.

**Phase D — froxel fog + local media.** `[plan P2]`
Nothing exists (A11-A13, A15-A16, A23); the current fog is a single homogeneous full-screen march (`jce_volumetric_fog.c:2-7`). Full build. Do it *after* Phase C because froxel injection wants the aerial-perspective convention, and after Phase A because U1 (humidity → extinction) is one of its media sources.

**Phase E — weather system + wetness response.** `[plan P4]`
WeatherPreset asset and 8 presets (C1/C2), the storm transition cascade (C4), lightning (C16), rain/snow beyond a screen overlay (C7/C9/C10), and — the big one — **material wetness response (C18)**, which needs a new PBR material parameter before any shader can respond. §2a's writer is this phase's front end, so scope them together.

**Phase F — terrain editing correctness.** `[plan P6 — promoted above P5]`
Delta undo (D13), editor Commands (D14/G16), async+cancellable erosion (D15 — copy the pattern already proven at `jce_sr_environment.c:3777-3784, 3766`), and **erosion on tiled terrain (D31)**. **Why before the terrain renderer, against plan P5 < P6:** the renderer half is largely done (D2/D3 stream and LOD today, `jce_sr_terrain.c:489-525`), while the editor half violates §13 and §46 outright. The plan's order assumes the renderer is missing; it isn't.

**Phase G — real hydraulic erosion + derived maps.** `[plan P7]`
D17/D18/D19. This is a from-scratch grid simulation: `jce_phacelle.h:49-56` states in the source that no flow, watershed or moisture map may be derived from what ships. It is also the unlock for F3 (scatter inputs), D22 (material inputs) and E11 (river placement), so it gates two later phases.

**Phase H — terrain runtime polish.** `[plan P5 — demoted]`
LOD morph (D5), seam/crack quality (D7), GPU height sampling (D6), the sampler-stage budget that currently blocks both cloud shadows and a 7-layer material (B26, D23). Deferred because it is quality on a working system, not capability.

**Phase I — water authoring.** `[plan P8, scope inverted]`
The plan lists "Gerstner + shading + shore + underwater + buoyancy" — **all of that is already COMPLETE or PARTIAL-good** (E18-E23, E25, E29, E33, E38; FFT/JONSWAP/cascades are beyond what the plan asked for). What is missing is the entire authoring model: body types, splines, rivers, flow, masks, debug (E1/E2/E4/E9-E13/E15), plus the surface-query fields physics and gameplay actually need — velocity, body id, depth, flow (E17, `jce_water_field.h:56-72`). **Re-scope this phase from "build water" to "build water bodies".**

**Phase J — environment lab + visual regression.** `[plan P0 deliverable / P12 — split and promoted]`
`samples/world_environment_lab/` does not exist and neither does `samples/` (G26). Every phase from B onward changes how existing scenes look; without a fixed-camera/seed/weather golden harness (G27) none of those changes is reviewable and none is defensible under §57. **Build the lab and the harness as soon as Phase A gives it a saveable state to fix** — i.e. start it in parallel with Phase B, not at the end. The plan puts the lab in Phase 0 and verification in Phase 12; the evidence says the harness belongs with the lab, early.

**Phase K — vegetation: PCG + biome.** `[plan P9]`
F6-F11 and F29-F30 are absolute zeros; F3 depends on Phase G's derived maps; F17/F19 (GPU-driven foliage) can reuse `jce_gpu_scene.c`. The rendering half (F16/F18/F21/F22) is already done, so this phase is a scatter-rules and asset-model phase, not a rendering phase.

**Phase L — integrated coupling.** `[plan P10]`
Only meaningful once the four §2 consumers actually read env: cloud shadow → water, wetness → materials, wind → grass + ocean + rain, humidity → fog.

**Phase M — tiers + backend fallbacks.** `[plan P11]`
G19-G22. MEDIUM is currently undefinable (G20) because froxel fog and a low-res cloud path don't exist; it becomes definable after Phases B and D.

**Phase N — independent verification.** `[plan P12]`
Per §58, using the Phase J harness, and explicitly not a re-run of development unit tests.

---

## 5. The four things most likely to be underestimated

**1. "Hydraulic erosion" is a rename away from being reported as done — and it is not started.**
`jce_terrain_apply_erosion` exists, has a params struct, is in the public ABI, and has an editor button. Every surface signal says the capability is there. The source says otherwise, in its own words: `jce_phacelle.h:49-56` — *"THERE IS NO HYDROLOGICAL CONNECTIVITY. The gullies are a local, per-point illusion… No river network, flow accumulation, watershed, drainage-based biome or moisture map may be derived from this output. If a design needs those, it needs an actual hydraulic-erosion simulation over a bounded grid, which this is not."* Its parameters are `octaves/frequency/strength/detail`; §15's are `rainfall/evaporation/erosion rate/deposition/sediment capacity/flow force/erodability/iterations/feature size`. **Zero overlap.** The plan's §15 derived outputs (flow, flow direction, sediment, deposition, wear, water accumulation) are the inputs to §17 terrain materials, §31 scatter, and §20 river placement — so estimating this as "tune the existing erosion" silently under-scopes three other phases. It is a new bounded-grid simulation, plus its async/preview/cancel harness (D15), plus making it work on tiled terrain (D31, which today returns `false` and touches nothing).

**2. Cloud shadows cannot reach terrain or vegetation without a sampler-budget redesign.**
§5.5 is categorical: *"不能只有天空中'看见云',地面光照完全不变"* — and the ground it names is terrain, buildings, vegetation, water. Today only buildings/meshes get it (`fs_pbr_body.sh:919-923`). The terrain shader documents exactly why not, and it is not a wiring oversight: `fs_terrain.sc:155-168` — *"terrain has all 16 sampler stages occupied (splat + 3 layers at 13-15), so there is no free slot to bind it to."* A previous attempt bound nothing and **turned the whole ground black**. So B26 is not one line; it is either a texture-array consolidation of the splat layers, or a per-vertex cloud term, or a channel stolen from an existing bound target — and the same 16-stage ceiling is what caps D23 at 3 material layers when §17 asks for 7. **These two are the same problem and should be scoped as one task.** Anyone estimating B26 and D23 separately, each as small, will be wrong twice.

**3. Every wind/wetness/environment wiring task rewrites how existing scenes look, and there is no harness to review that with.**
U4 alone changes grass motion in every scene with a grass field and the ocean spectrum in every scene with FFT water — both currently driven by *authored per-component* values (`jce_scene.h:1136-1138`, `:1226-1229`) that artists tuned. C18 (material wetness) adds a new PBR parameter that changes every lit surface under rain. A5 (real aerial perspective) replaces the height fog that every scene's distance look was tuned against. There are no environment goldens, no `samples/` directory at all (G26), and `tools/visual_diff.py` has nothing to compare. The precedent is in this repo's own history: switching the FFT spectrum was called out in the source as *"a deliberate re-baseline of any golden hash over the water field, never a side effect of a build"* (`jce_scene.h:1240-1246`). **Budget the golden harness as a Phase B-parallel dependency, not as Phase 12 cleanup**, and budget a re-baseline pass into every wiring task — U4, C18, A5, and the Phase C atmosphere swap each need one.

**4. §2 is four capabilities of work and thirty capabilities of *reach*, and the reach is where it breaks.**
The struct landed in one commit and looks finished. The matrix says otherwise: `JceEnvironmentState` has no writer, no serializer (G30 — it cannot survive a save/reopen, which §57 alone makes disqualifying), lives on the renderer so headless never ticks it (G4), receives data *from* the scene settings rather than feeding them (G3), and is read by exactly one consumer while four subsystems still keep their own weather. And the one wiring that *did* land — `st.wind_strength = jce_environment_wind_speed_now(&sr->env)` — made the shipping product visibly worse, because the field it reads is permanently zero and nothing caught it (§0). That is the pattern to plan against: **in this codebase a carrier can land, pass 301 tests and 25 lints, and still be a regression, because "the value is 0" is not a test failure.** Every §2 wiring task in Phase A must ship with (a) its writer, (b) a serialization round-trip test, and (c) an assertion or golden that the wired value is non-default in at least one shipped scene. Without (c), the next four wirings fail the same way and each one will look green.