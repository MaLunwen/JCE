#!/usr/bin/env python3
# THIS RUNNER IS NOT ALL THE GATES.  tools/audit/run_architecture_audit.py
# runs sixteen more -- the ABI snapshot against contracts/abi-snapshot.txt,
# dependency boundaries and licences, view reservations, binding parity, the
# script-VM and language-catalog contracts, SDK scripting export, the patented
# codec gate, singleton ownership, single-decoder -- plus the two script
# generators in check mode.  "run_all.py is green" and "a gate is red" are
# both true at once when that suite has not been run.
#
# The comment that stood here until 2026-08-31 said four of those gates "were
# removed with tools/audit/ itself (owner decision, 2026-08-17)".  That is not
# true today: tools/audit/ exists and all sixteen checkers are tracked in git.
# It was found by a public-header change that drifted contracts/abi-snapshot.txt
# -- a TRACKED contract -- with nothing in the documented closing procedure
# noticing.  A runner that tells you the other gates do not exist is worse than
# one that says nothing.
#
#     python tools/lint/run_all.py                     # this file
#     python tools/audit/run_architecture_audit.py       # the other sixteen
"""
run_all.py — Run every JCE lint in sequence and aggregate results.

Each lint script is invoked as a child process so failures in one do not
short-circuit the others; the aggregate exit code is non-zero if any
individual lint fails. CI calls this as the single entry point.

Usage:
  python tools/lint/run_all.py
  # exits 0 if all lints pass, 1 if any fail.
"""

from __future__ import annotations

import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path

LINT_DIR = Path(__file__).resolve().parent
REPO_ROOT = LINT_DIR.parents[1]

# Order: cheap structural checks first, then content scans.
#
# An entry is either a bare filename (resolved under tools/lint/) or a
# (repo-relative path, argv) pair for a checker that lives elsewhere.
LINTS = [
    "check_release_version.py",
    ("tools/lint/check_release_version.py", ["--self-check"]),
    "check_repository_hygiene.py",
    ("tools/tests/test_shader_lint.py", []),
    ("tools/tests/test_build_driver.py", []),
    ("tools/tests/test_standalone.py", []),
    ("tools/tests/test_conan_configuration.py", []),
    ("tools/tests/test_llm_transport.py", []),
    ("tools/tests/test_skills_paths.py", []),
    ("tools/tests/test_pack_skill.py", []),
    ("tools/lint/check_skill_locales.py", ["--self-check"]),
    ("tools/lint/check_repository_hygiene.py", ["--self-check"]),
    # AGENTS.md §11's 3000-line cap.  It was the one invariant in that list
    # with no checker at all -- restated five times across the charters and
    # enforced zero times, while the 2026-06-23 renderer split silently
    # regrew 14,058 lines in 68 days with this runner green throughout.
    "check_file_size.py",
    "check_agents_md.py",
    "check_determinism_contract.py",
    # 三处（引擎默认 / 编辑器发射器 / CLI 发射器）必须对同两个符号与
    # 64 字节达成一致；任何一处漂了都不会报错，只会让加密的 dist
    # 产物在客户机上解不开自己的档案。
    "check_pak_key_shares_contract.py",
    # 一个被 #ifdef 守着、却哪里都没有 #define 的宏，是一条
    # 永远编译不进任何 program 的 permutation 轴。而它不只是死代码：
    # JCE_RENDER_COOKIE_2D_ARRAY 死着的时候，C 侧却照样把一张
    # 永远没人写入的 array 纹理绑到 sampler 13 上，于是灯光 cookie
    # 在每个桌面后端上静默失效。
    "check_shader_feature_axes.py",
    # 构建脚本里写死的本机绝对路径，在写它的那台机器上是隐形的，
    # 在别的机器上会把「你没装 emsdk」变成「工具链在一个不存在的
    # 路径上」。2026-09-18 实测：四个入库脚本共八条，最糟的一条
    # 不是「默认值」而是 **无条件覆盖** JAVA_HOME。
    "check_no_machine_paths.py",
    # 阴影投影体必须在每一条顶点路径上被压平到近平面。级联的正交盒
    # 朝太阳的推移量是个启发式（jce_csm.c 的 near_extend，按世界 XZ
    # 的 ±2*radius 取），而一个投影体要够到某级联所需的水平距离是
    # 高度 / tan(太阳仰角)——随仰角下降无界增长，与 radius 无关。
    # 漏掉一条路径不会编译失败，只会让那条路径继续丢阴影，于是缺陷
    # 变成「跟网格类型相关」的间歇现象，报告的人说不出条件。
    "check_shadow_pancake.py",
    # 阴影深度偏置必须是一个距离。它此前是「归一化深度」里的常数，
    # 于是同一个数在同一帧里 cascade 0 是 89 mm、cascade 3 是 8969 mm
    # （10.4 倍，没人选过这个比例），还会随工程改 Shadow Distance 漂。
    # 两处站点各自恢复旧式写法不会编译失败，只会让地形和网格在同一帧
    # 的同一块地上对「偏置是多少」给出两个答案。
    "check_shadow_bias_units.py",
    "check_shader_variants_generated.py",
    "check_public_api_purity.py",
    # AGENTS.md §4's promise -- "#include <jce/api.h> gives you the
    # whole engine" -- which check_editor_consumption.py measured and
    # PRINTED for a month without ever failing on it: 111 of 299 public
    # headers were unreachable on 2026-08-28 and still exactly 111 on
    # 08-31.  Enforcement now lives in its own gate.
    "check_api_closure.py",
    "check_editor_consumer_purity.py",
    # The Inspector edits engine memory through raw pointers and bolts undo on
    # beside the widget with insp_track_edit(), which only works if it runs on
    # EVERY frame.  Call sites sitting inside a one-frame ImGui predicate ran on
    # neither the activation nor the deactivation frame -- a combo change was
    # neither undoable nor marked the scene dirty, and nothing said so.  The
    # backlog is burned down and the ratchet's baseline is empty, so both of its
    # rules are absolute now.
    "check_inspector_undo_scope.py",
    # Bullet's built-in collision-filter constants ARE the low bits of JCE's
    # 32-slot layer matrix (CharacterFilter == 32 == layer 5), so using one
    # silently pins an object to a layer nobody authored.  The character capsule
    # did exactly that: it behaved as layer 5, could only ever touch layers 0
    # and 1, and probed the ground through a filter it did not collide through.
    "check_physics_layer_filters.py",
    # JceEnvironmentState is scene-owned so physics and gameplay read the same
    # world the renderer draws.  The state moved; the DRIVER did not --
    # jce_environment_advance had one caller and it was inside the renderer, so
    # a headless dedicated server's clock, wetness, snow and sun never moved and
    # jce_environment_is_daytime() answered TRUE forever.  Invisible in every
    # configuration a developer looks at, because those all have a renderer.
    "check_environment_authority.py",
    # A prefab instance is stored as a REFERENCE plus a diff: the editor writes
    # "overrides":[...] with only the differing rows and an EMPTY children
    # array, because "an instance root's children come ENTIRELY from the source
    # on load".  The engine could not load that -- `overrides` appeared zero
    # times in it -- so a shipped level rebuilt every placed prefab as one bare
    # entity with no components and no children the source provided.
    "check_prefab_override_parity.py",
    # JceScriptHost is a table of function pointers and jce_script.c falls back
    # to a plausible default for every NULL slot -- which is what keeps a short
    # host safe AND what makes an unfilled slot invisible.  The runtime is the
    # only writer, so a member it never assigns is a binding that is
    # registered, callable and permanently inert in all seven languages.
    # is_key_down had been exactly that since it was written.
    "check_script_host_writers.py",
    # jce_default_main.inc.h IS the shipped game -- a project's main.c is a
    # ten-line shim around it -- and NOTHING in this repository compiles it.
    # Not a target, not a test, not a sample.  So a call to a function no
    # included header declares is silent here (MSVC needs /W3 for C4013) and a
    # hard error in a customer's clang 15+ build, pointing inside an SDK header
    # they never wrote.  Two such calls were live when this landed.
    "check_shipped_main_declarations.py",
    # Play is a transient sandbox, but two things outlive it -- the saved
    # scene file and the undo stack -- and both leaked.  Ctrl+S during Play
    # wrote the SIMULATED scene over the authored level (while the autosave
    # timer beside it was already guarded), and history PRODUCTION never
    # checked play state though undo/redo consumption did, so live tweaks
    # evicted the real history and became undoable over the real scene.
    # Both were one missing condition next to code that already had it.
    "check_play_mode_isolation.py",
    # jce_world_streamer_create had exactly two callers, both under editor/src/,
    # so a shipped game streamed none of the chunks its scene authored -- for a
    # large world, none of the world.  Rule 2 asks whether the update is
    # REACHED, not merely present: a streamer created and never ticked loads the
    # ring around the origin once and then freezes, which reads as working for
    # as long as the player stands still.
    "check_world_streaming_shipped.py",
    # The same shape, found 2026-09-20: jce_physics_debug_set_line_sink and
    # jce_physics_debug_flush had exactly one caller each and both were under
    # editor/, so a packaged game could not draw a collider -- in the one
    # configuration where a developer cannot write the wiring themselves.
    # --self-check runs six fixtures, two of which exist because this gate's
    # own first version counted the implementation file as a caller.
    "check_physics_debug_shipped.py",
    # A TRACKED FILE MUST NOT SAY IT IS NOT IN THE TREE.  79 files under
    # tests/ ended their header with "NOT IN THE TREE: tests/ is gitignored on
    # this branch"; 73 were TRACKED, each asserting from inside the repository
    # that it was not in the repository.  The sentence is true of `main` and
    # was copied onto a branch that tracks tests/.  The six that were NOT
    # tracked were written by the one agent that BELIEVED it and therefore did
    # not commit them -- left on a worktree eleven branches share, which is
    # where an untracked file quietly disappears.  Self-refuting in 73 places,
    # load-bearing in 6, and invisible to every other gate.
    "check_provenance_claims.py",
    "check_authored_path_consumed.py",
    "check_component_field_consumed.py",
    "check_component_serializer_roundtrip.py",
    "check_shader_uniform_bound.py",
    "check_exported_symbol_declared.py",
    # The editor's Play path and the shipped game each hand-build a
    # JceRuntimeDesc, and nothing compared them: eleven of eighteen fields were
    # set on exactly one side.  A shipped game had no navmesh, nowhere to write
    # a save, and ignored every physics/time value the designer tuned -- while
    # the same scene behaved correctly the moment somebody pressed Play.
    "check_runtime_desc_parity.py",
    "check_layer_dependencies.py",
    "check_engine_native_io.py",
    # Invariants have to hold in the configuration that SHIPS.  Bare
    # assert() is compiled out by /DNDEBUG, and worse, five self-tests
    # had their whole bodies inside `#ifndef NDEBUG` -- so in Release the
    # functions did not exist and the tests that ran them could only be
    # registered for a Debug build.  Measured with the control both ways:
    # three deliberately-false assertions gave 452/452 GREEN before the
    # repair and three RED after.  Run this checker with --self-check to
    # see both of its rules fire on a planted defect.
    "check_engine_assertions.py",
    # §11's FIRST invariant ("engine内不出现 C++"), whose one-line
    # exception for third-party bridges had quietly become the rule.
    "check_engine_cxx_bridges.py",
    # The GL version floor and the shader profile live in two files, in two
    # languages, and neither fails loudly when wrong: a bad profile fails the
    # build with a GLSL message that never mentions bgfx, and a bad floor fails
    # nothing at all -- it renders, just on the 2010 paths.
    "check_gfx_api_tiers.py",
    # One character -- a trailing '_' on a shader local -- breaks that shader on
    # OpenGL and nowhere else, because only the GL path runs glsl-optimizer and
    # only glsl-optimizer turns `li_` into the reserved name `li__37`.  The sky
    # shader shipped that way and it took launching the editor on GL to see it.
    "check_shader_reserved_identifiers.py",
    # conan runs the hooks in ~/.conan2, not the ones committed here, and only
    # jce.py copies them across.  A direct `conan install` after editing a hook
    # builds with the old one and says nothing -- which cost a full bgfx
    # rebuild at the wrong OpenGL floor on 2026-09-01.
    "check_conan_hooks_synced.py",
    ("tools/lint/check_conan_source_policy.py", ["--self-check"]),
    # A log line naming an env var nothing reads asserts something false
    # about the machine it runs on.  Both editor viewports said
    # "JCE_DISABLE_OCCLUSION set" on every default run; no getenv has ever
    # read that name.
    "check_logged_env_vars.py",
    "check_async_teardown.py",
    "check_caps_bit_misread.py",
    "check_config_key_roundtrip.py",
    "check_culling_mask_single_source.py",
    "check_camera_clear_single_source.py",
    "check_morph_clip_wired.py",
    # A filter that lost its guides is still a filter: it lights every pixel
    # it used to, just wrongly, so no screenshot guard sees it.  SSGI's
    # denoiser lives inside the composite (two view ids, both spent), which
    # makes "simplify the composite" the edit that removes it.
    "check_ssgi_denoise_guided.py",
    # Shaping a string that has not changed produces the identical picture,
    # so a renderer that went back to doing it twice per frame per label would
    # report nothing at all.  The cache is the only shaper; this says so.
    "check_text_shape_cached.py",
    "check_entity_tag_layer_single_writer.py",
    "check_sprite_sorting.py",
    # A self-test with no caller is a claim nobody checked.  This repo had
    # seven and zero callers; wiring the five runnable ones found three
    # failures on their first ever run, two of them real engine bugs.
    "check_self_tests_run.py",
    # §11's remaining two ungated invariants. Both HOLD today -- these
    # gate a clean state rather than fix a broken one, because a second
    # math library and a second build entry point both arrive by drift,
    # never by decision.
    "check_math_singleton.py",
    "check_build_script_layout.py",
    "check_platform_macros.py",
    "check_input_seam.py",
    "check_mesh_enable_consumers.py",
    "check_cull_gen_consumers.py",
    "check_instance_sort_depth.py",
    "check_raw_allocator.py",
    "check_sdk_asset_embed_shape.py",
    "check_project_build_layout.py",
    "check_jce_tests_closure.py",
    "check_env_light_authority.py",
    "check_water_field_authority.py",
    "check_terrain_single_owner.py",
    "check_shader_sampler_slots.py",
    "check_shader_branch_order.py",
    "check_shader_varying_pairs.py",
    "check_format_has_producer.py",
    "check_material_texture_sampler.py",
    # One light, four parsers: only the unified "Light" row has a
    # serializer, so the three compat rows are read-only paths nothing
    # in the editor round trip exercises -- and two of them silently
    # dropped castsShadow/shadowBias.
    "check_light_parser_parity.py",
    # A component the engine SERIALISES must be addable through the generic
    # jce_scene_set_comp -- otherwise a script, an SDK consumer or a CLI can
    # read one out of a scene file and never write one, and the first sign is
    # a designer opening the scene and finding it empty.
    "check_component_authoring_surface.py",
    # A Project Settings knob that stops at the editor window.  Six
    # defects of exactly this shape shipped in one week -- quality tier,
    # texture quality, anisotropic, Target Framerate, Pixel Light Count,
    # per-level AA -- every one of them right in the viewport and wrong
    # in the exe, with no symptom.
    "check_project_settings_consumed.py",
    # The AI tooling's safety invariants: no dry run may print a
    # credential, and nothing leaves the machine without --send.  Both
    # are asserted against EVERY provider by iterating the table, which
    # is how the allow-list redaction that leaked a Gemini key got in.
    # Every parity claim carries its evidence.  The audit that produced
    # the original split published totals and stored no rows, so it
    # could never be re-run -- a number that quietly stops being true
    # while still being printed.  contracts/engine-parity.json is that
    # measurement made re-runnable, and this refuses a row without the
    # file, command or landed item that settles it.
    "check_bundle_deps_keys.py",
    "check_parity_ledger.py",
    # README.md listed FIFTEEN public umbrellas while the tree had twenty-three.
    # Nobody typed it wrongly -- the tree moved and the front page did not, for
    # a month, and then the repository was made public.  The volatile half of
    # the README is generated now; this is what keeps it that way.
    "check_readme_facts.py",
    # `git add -A` is one keystroke and a pushed commit is permanent. The
    # unpublished AI authoring work lives under private/; this fails if any of
    # it ever becomes tracked, AND if the .gitignore rule that keeps it out is
    # ever removed -- checking only the effect would pass on a tree where the
    # directory happens to be absent.
    "check_private_tech_untracked.py",
    ("tools/lint/check_source_layout.py", ["--self-check"]),
    "check_source_layout.py",
    # The Automation API's tool surface: the tracked contract in
    # contracts/automation-tools.json is what an external agent reads to learn
    # what exists, so a tool added or renamed without regenerating it publishes
    # a surface that is no longer there -- and nothing fails, because the code
    # still works.  This also RUNS the suite rather than checking that it
    # exists: this repository has shipped self-tests that were written and
    # never called.
    # The same question one level up: the spec in .docs/way/ states 42
    # requirements AND states how each is to be verified, and until 2026-09-20
    # nothing connected the two -- 37 of the 42 were named by no acceptance and
    # 32 by no test, so "is REQ-X met?" could only be answered by re-deriving
    # it by hand, differently each time.  The contract stores the JUDGEMENT and
    # reads the STATEMENT from the spec on every run, so a requirement whose
    # wording changes fails instead of being silently re-interpreted.  A row
    # without evidence fails too: this file exists so the claims and the
    # operations are the same list.  SKIPS when .docs/ is absent (it is
    # gitignored), and says so rather than passing quietly.
    # The chain that makes "no module bypasses the Automation API" enforceable
    # rather than merely stated: Changeset.record_write is the only way a tool
    # puts bytes in the project, and api.call requires a changeset for any
    # tool declaring writes=True.  The missing link is a tool that records a
    # write and forgets the flag -- it still works, it still writes, it just
    # does it outside a changeset, so rollback leaves those bytes behind.  A
    # write that happens is indistinguishable from a write that happens
    # correctly until somebody rolls back.
    # REQ-ARCH-02's enforceable half.  The other half -- editor and CLI on one
    # API -- is a design question about who owns undo and is recorded as open.
    # This one is a prohibition, and a prohibition nobody checks is a sentence:
    # the engine's input replay facility would let an agent "click" anything in
    # the editor without writing a single tool, and a replayed click has no
    # return value, no error code and no changeset.
    # REQ-ARCH-02's ownership, decided rather than left open: the editor's
    # undo history owns in-memory scene edits -- ALL of them, including any
    # that arrive through the Automation API while the editor runs -- and the
    # changeset owns files on disk. That is Unity's Undo.RecordObject rule and
    # Unreal's FScopedTransaction rule; in neither engine is Ctrl+Z a
    # version-control revert. Zero such call sites exist today, which is why
    # this gate is for the FIRST one.
    # Every JceLifecycleEvent must have somewhere that emits it.
    # JCE_LIFECYCLE_DEVICE_LOST shipped in the public ABI with ZERO emit
    # sites: registering for it returned a valid handle and the callback was
    # dead code, and the one place that mentioned the gap was a platform
    # AGENTS.md table whose stated reason had gone stale.  The checker says
    # out loud what it cannot see -- DEVICE_RESET has an emitter that is
    # UNREACHABLE on every real backend, and passes.
    "check_lifecycle_events_emitted.py",
    # AND ITS OWN FIXTURES, as a separate entry.  This rule was rewritten
    # repeatedly by being USED, and more than once the rewrite made it LOOSER
    # -- counting a header declaration as a compliant call; letting the scope
    # lookback read a comment; a hard-coded entry-point list that made a new
    # writing call site invisible.  None of those reports anything.
    #
    # Most of its fixtures must stay GREEN, and those are the ones that stop
    # this being a rule against the name jce_editor_automation rather than
    # against the defect -- its first real finding would otherwise be a false
    # positive, which is how a gate stops being run.  A green fixture cannot
    # be verified by loosening the rule (it was already green), so each is
    # checked by making the rule STRICTER until it goes red; the checker's own
    # docstring carries the table.
    #
    # NO COUNTS HERE ON PURPOSE.  The line this replaces said "seven fixtures,
    # two of which must stay green" and by then it was ten and six -- and the
    # summary inside the checker had the same defect, twice, which is what
    # made it count instead of assert.
    "i18n_audit.py",
    "i18n_hardcoded.py",
    "check_i18n_dup_values.py",
    # Gates whose subject may legitimately be absent from a worktree.  They
    # exit 2 = SKIPPED (see EXIT_SKIPPED below) rather than inventing a verdict.
    "check_editor_consumption.py",
    "check_skills.py",
    "check_test_window_discipline.py",
    # The engine describes its components and so does tools/jce_scene_kit.py;
    # this asserts they describe the SAME ones.  Asking the engine needs a
    # built SDK, so on a checkout without one it reports SKIPPED rather than
    # inventing a verdict -- "I could not ask" and "they agree" are different
    # facts, and only one of them is safe to author scenes against.
]

# A gate whose subject is missing must be able to SAY SO.  Without this the
# aggregator has only pass/fail, so the doctrine every checker here already
# follows -- "report SKIPPED and state why; never fail, never silently pass"
# (check_agents_md.py) -- was stated but not representable: a correct SKIPPED
# came out as FAIL, and the cheapest way to get green was to delete the gate.
EXIT_SKIPPED = 2


def resolve(entry) -> tuple[Path, list[str], str]:
    if isinstance(entry, tuple):
        rel, argv = entry
        return REPO_ROOT / rel, list(argv), " ".join([rel, *argv])
    return LINT_DIR / entry, [], entry


# CPython's own message for a corrupt compiled-pattern state inside the sre
# engine.  No pattern in this repository can request it and no checker's source
# can raise it -- it is the environment fault recorded below wearing a
# traceback instead of dying silently.  Deliberately the ONLY loud signature
# here: a TypeError or an AssertionError could be a real defect, and a retry
# rule that swallowed those would be the thing this machinery exists to catch.
# Each entry is a signature: a string that must appear, or a TUPLE of strings
# that must ALL appear.  The tuple form exists so a signature can be pinned to
# the stdlib frame that raised it -- see the ast one below, whose exception
# TYPE is ordinary and whose LOCATION is not.
_INTERPRETER_FAULTS = (
    "RuntimeError: internal error in regular expression engine",
    # CPython walking an AST it has just built and finding a LIST where a node
    # type's _fields must hold strings.  _fields is a tuple of str on the node
    # CLASS; a checker can choose which tree to walk and nothing else, so this
    # says the node object is not what the interpreter put there.  Pinned to
    # ast.py on purpose: `TypeError: attribute name must be string` raised from
    # a checker's own getattr WOULD be a real defect, and this rule must not
    # cover that.
    ("ast.py", "TypeError: attribute name must be string, not"),
)


# Private suites are local additions, never prerequisites of the public core.
_PRIVATE_DIR = REPO_ROOT / "private/tools/lint"
_PRIVATE_CHECKS = ['check_ai_tooling.py', 'check_automation_contract.py', 'check_requirement_traceability.py', 'check_automation_write_declared.py', 'check_editor_automation_undo.py', 'check_no_ui_automation.py', 'check_introspection_parity.py']
for _name in _PRIVATE_CHECKS:
    if (_PRIVATE_DIR / _name).is_file():
        LINTS.append(("private/tools/lint/" + _name, []))
    else:
        print("Private suite unavailable (not a PASS): " + _name)

# ---------------------------------------------------------------------------
#  A SECOND FAULT FAMILY, AND IT MUST NOT BE RETRIED.
#
#  These are the machine running out of Windows COMMIT CHARGE -- the promise
#  Windows bills for, not the memory anyone used.  A checker whose peak working
#  set is 69 MB still dies this way, and every one of these signatures reaches
#  the runner as `exit 1` with a traceback, which is indistinguishable from a
#  finding by exit code alone.  That is why they are matched by TEXT.
#
#  WHY THEY GET NO RETRY, measured 2026-09-20.  The retry below was written for
#  interpreter faults, where re-running is free and usually works.  Under
#  commit exhaustion it does the opposite:
#
#    1. check_automation_contract.py dies -- but the SUITE it spawned is a
#       GRANDCHILD.  subprocess.run kills the child it started; nothing kills
#       the grandchild, which keeps running and keeps its 1.59 GB.
#    2. the retry starts a second one.
#    3. several lint runs in an afternoon stacked six of them, ~9.5 GB held by
#       processes whose parents were all dead.
#
#  So the retry ran into the exact condition that caused the failure while the
#  previous attempt still held the memory.  The intent was right ("a checker
#  that says nothing has found nothing"); the machine inverted the cost.
#
#  And the verdict is different too: a checker that died of this did NOT find a
#  violation, and calling it FAIL is how a reader spends twenty minutes looking
#  for a defect in a checker that never ran.  It is NOT MEASURED -- which still
#  leaves the run un-clean, because an unmeasured gate is not a passed one.
# ---------------------------------------------------------------------------
_RESOURCE_FAULTS = (
    "MemoryError",
    "OSError: [WinError 1450]",     # insufficient system resources
    "OSError: [WinError 1455]",     # the paging file is too small
    "Insufficient system resources",
    "The paging file is too small",
)


def _resource_matches(text):
    """True when `text` shows the MACHINE gave out, not the tree.

    BOTH HALVES ARE LOAD-BEARING AND NEITHER IS SUFFICIENT.

    The traceback is required because `_RESOURCE_FAULTS` above now CONTAINS
    the string "MemoryError", so matching the word alone would let any checker
    that scans tools/lint and echoes a line have its REAL finding silently
    re-labelled "not measured".  This repository has already had one gate
    permanently disarmed by the act of committing its own text.

    The signature is required because a traceback alone means only "something
    went wrong".  A checker that crashes on ITS OWN defect -- an IndexError in
    a parser, a None where a path was expected -- also arrives as exit 1 with a
    traceback, and that is a genuine red that must not be swallowed.  Do not
    simplify this to "has a traceback, do not retry": the question is not
    whether the failure was abnormal, it is whether it was the MACHINE.
    Answering the first question instead of the second turns this from a
    correction into a way to lose defects."""
    t = text or ""
    if "Traceback (most recent call last)" not in t:
        return False
    return any(sig in t for sig in _RESOURCE_FAULTS)


def _commit_free_gb():
    """Free Windows commit charge in GB, or None where it cannot be read.

    Read before a retry, because the retry's whole cost depends on it.
    """
    try:
        import ctypes

        class _MS(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong),
                        ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong),
                        ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong),
                        ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong),
                        ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

        st = _MS()
        st.dwLength = ctypes.sizeof(_MS)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)):
            return None
        return st.ullAvailPageFile / (1024.0 ** 3)
    except Exception:
        return None


# Below this much free commit, a retry is more likely to add a stuck process
# than to produce an answer.  2 GB because one automation suite reserves 1.59.
_RETRY_MIN_FREE_GB = 2.5


def _fault_matches(text):
    """True when `text` carries one of the signatures above."""
    for sig in _INTERPRETER_FAULTS:
        if isinstance(sig, tuple):
            if all(s in text for s in sig):
                return True
        elif sig in text:
            return True
    return False


def _crash_shaped(rc, out, skip_code):
    """A failure with nothing to say -- or nothing its source could say -- is
    the process dying, not a finding.

    KEEP IN SYNC with the identical copy in tools/audit/run_architecture_audit.py and tools/audit/run_dedup_audit.py."""
    if rc in (0, skip_code):
        return False
    text = (out or "").strip()
    if not text:
        return True
    return _fault_matches(text)


# THE CHILDREN DO NOT WRITE BYTECODE, AND A CRASHED CHILD IS RE-RUN ONCE.
#
# Six gate runs in one session failed with results that cannot be true:
# "[FAIL] editor-consumer-purity" with NO output at all (three times), a
# TypeError from check_raw_allocator.py claiming `list.append` was "a generator
# object [that] is not callable", and i18n_audit.py exiting 3221225477 --
# 0xC0000005, an ACCESS VIOLATION in the interpreter.  Every one passed when
# re-run alone, and none is a bug the source can have.
#
# PYTHONDONTWRITEBYTECODE WAS THE FIRST GUESS AND IT WAS WRONG: the access
# violation happened with it already set.  It is kept anyway (a runner whose
# ~20 short children race on one __pycache__ is worse for no gain), but the
# cause is upstream of this repository -- something in this Windows environment
# kills a freshly spawned CPython often enough to be seen several times an hour
# under a burst of ~20 spawns.
#
# SO A CRASH-SHAPED FAILURE IS RE-RUN ONCE, AND THE RETRY IS ANNOUNCED.
# Crash-shaped means EMPTY OUTPUT with an exit code that is neither success nor
# the SKIPPED convention: every gate here prints before it fails, so a silent
# failure carries no finding to lose.  A checker that prints anything is never
# retried, and a second crash is reported as a failure -- this makes a flaky
# environment legible, it does not make a red gate green.
#
# Five gate runs in one session failed with results that cannot be true:
# "[FAIL] editor-consumer-purity" with NO output at all (three times), and a
# TypeError from check_raw_allocator.py claiming `list.append` was "a generator
# object [that] is not callable".  Every one passed when re-run alone, and the
# last one is not a bug that source can have.
#
# THIS IS NOT A PROVEN DIAGNOSIS.  What is known: these runners spawn ~20 short
# python children, often while another python is running in the same tree, and
# the one piece of shared MUTABLE state those children all touch is
# __pycache__.  A torn .pyc explains both shapes -- an interpreter that dies
# before printing, and one that runs something that is not the source.  So the
# shared thing is removed from the picture rather than reasoned about; the cost
# is re-compiling a handful of small scripts per run, which is noise next to
# what they do.
#
# If a spurious failure recurs after this, the cache was not it, and that is
# worth more than a guess that happened to be followed by quiet.
def run_one(entry) -> tuple[str, int, str]:
    path, argv, label = resolve(entry)
    if not path.is_file():
        return label, 127, f"(skipped: {label} not found)"
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"

    def _spawn():
        return subprocess.run(
            [sys.executable, str(path), *argv],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=env,
        )

    proc = _spawn()
    _out = (proc.stdout or "") + (proc.stderr or "")

    if _resource_matches(_out):
        # The machine gave out.  Do NOT re-run: see _RESOURCE_FAULTS.
        free = _commit_free_gb()
        where = f"{free:.2f} GB" if free is not None else "unknown"
        print(f"  [not measured] {path.name} died of resource exhaustion "
              f"(commit free: {where}) -- NOT re-run, because the retry would "
              f"walk into the same wall while the previous attempt's "
              f"grandchildren still hold their memory", flush=True)
    elif _crash_shaped(proc.returncode, _out, EXIT_SKIPPED):
        free = _commit_free_gb()
        if free is not None and free < _RETRY_MIN_FREE_GB:
            print(f"  [not measured] {path.name} exited {proc.returncode} with "
                  f"no output, and commit free is {free:.2f} GB -- under "
                  f"{_RETRY_MIN_FREE_GB} GB a retry adds a stuck process more "
                  f"often than an answer, so it is not attempted", flush=True)
        else:
            print(f"  [retry] {path.name} exited {proc.returncode} with no "
                  f"output -- crash-shaped, re-running once (a checker that "
                  f"says nothing has found nothing)", flush=True)
            proc = _spawn()

    out = (proc.stdout or "") + (proc.stderr or "")
    return label, proc.returncode, out.rstrip()


DEFAULT_LOG = REPO_ROOT / "reports" / "lint-failure.txt"


def write_transcript(results, failed):
    """Persist every lint's exit code and output when the suite goes red.

    A run of this suite is not reproducible: twenty-seven checkers read a
    working tree that other processes are also writing.  On 2026-08-27 this
    aggregator went red twice in ~23 runs with

        TypeError: 'generator' object is not callable

    as the LAST line -- and both times the caller had kept only the summary,
    so there was nothing left to say WHICH checker had raised it.  The failing
    line names this file so that even `... | tail -1` carries the pointer.
    """
    if not failed:
        return None
    try:
        DEFAULT_LOG.parent.mkdir(parents=True, exist_ok=True)
        parts = [
            "# JCE lint transcript (%s)"
            % datetime.now().isoformat(timespec="seconds"),
            "# %d lint(s); python %s" % (len(results), sys.version.split()[0]),
            "",
        ]
        for name, code, out in results:
            parts.append("=" * 72)
            parts.append("%s   exit=%d" % (name, code))
            parts.append("-" * 72)
            parts.append(out.rstrip() or "(no output)")
            parts.append("")
        DEFAULT_LOG.write_text("\n".join(parts) + "\n",
                               encoding="utf-8", newline="\n")
        return DEFAULT_LOG
    except OSError as exc:
        print("(could not write transcript to %s: %s)" % (DEFAULT_LOG, exc))
        return None


def main() -> int:
    print(f"running {len(LINTS)} lint(s)...")
    print("=" * 72)
    results: list[tuple[str, int, str]] = []
    for s in LINTS:
        results.append(run_one(s))

    skipped = [(s, c, o) for (s, c, o) in results if c == EXIT_SKIPPED]
    failures = [(s, c, o) for (s, c, o) in results
                if c != 0 and c != EXIT_SKIPPED]

    for s, c, o in results:
        if c == 0:
            status = "PASS"
        elif c == EXIT_SKIPPED:
            status = "SKIP"
        elif c == 1 and _resource_matches(o):
            # exit 1 with a traceback is what an UNCAUGHT exception looks like,
            # which is byte-identical to a checker that found something and
            # exited 1 on purpose.  The text is the only thing that tells them
            # apart, and getting it wrong sends a reader hunting for a defect
            # in a gate that never ran.
            status = "NOT MEASURED -- the machine ran out, this is not a finding"
        elif c == 1:
            status = "FAIL (1)"
        else:
            # A checker exits 1 when it finds a violation.  Anything else means
            # it did not get to finish: an unhandled exception (2), a Windows
            # access violation (3221225477 / 0xC0000005), a POSIX signal
            # (128+N).  Those are NOT findings, and reporting them with the
            # same word taught this session's reader that a checker had found
            # something when it had in fact crashed before checking anything.
            status = f"CRASH ({c}) -- did not complete, this is not a finding"
        print(f"[{status}] {s}")

    if skipped:
        print("=" * 72)
        print(f"{len(skipped)} lint(s) SKIPPED (subject absent -- NOT a pass):")
        for s, c, o in skipped:
            first = next((ln for ln in o.splitlines() if ln.strip()), "")
            print(f"  {s}: {first}")

    unmeasured = [(s_, c, o) for (s_, c, o) in failures if _resource_matches(o)]
    if unmeasured:
        print("=" * 72)
        print(f"{len(unmeasured)} lint(s) NOT MEASURED -- the machine ran out of "
              f"commit charge, not the tree.  These are NOT findings, and they "
              f"are NOT passes either: re-run them when the machine is idle.")
        for s_, c, o in unmeasured:
            last = next((ln for ln in reversed(o.splitlines()) if ln.strip()), "")
            print(f"  {s_}: {last.strip()[:110]}")

    if not failures:
        # Clear the transcript from the last RED run.  It is written only on
        # failure and was never removed on success, so after a red run followed
        # by a green one the file sat there describing a state that no longer
        # exists -- with its own timestamp and lint count, which is exactly what
        # makes a stale artefact readable as a current one.  (Found 2026-08-31:
        # the leftover said "31 lint(s)" while the suite had 33.)
        try:
            DEFAULT_LOG.unlink(missing_ok=True)
        except OSError:
            pass
        print("=" * 72)
        print(f"{len(LINTS) - len(skipped)} of {len(LINTS)} lint(s) passed, "
              f"{len(skipped)} skipped.")
        return 0

    log_path = write_transcript(results, True)
    print("=" * 72)
    tail = f"  Full transcript: {log_path}" if log_path else ""
    print(f"{len(failures)} lint(s) failed. details:{tail}")
    print()
    for s, c, o in failures:
        print(f"--- {s} (exit {c}) ---")
        print(o)
        print()
    return 1


if __name__ == "__main__":
    sys.exit(main())
