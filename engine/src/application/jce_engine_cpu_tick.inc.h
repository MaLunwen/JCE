/* The one complete CPU update owner, factored from the normal iteration.
 * INITIALIZATION retains its existing per-iteration contract. Render phases,
 * GPU asset finalization and screenshots remain with admitted render frames. */
static void engine_cpu_tick(JceEngine *e, float dt)
{
    if (!engine_owner_ready(e)) return;
    /* ── PlayerLoop: INITIALIZATION / EARLY_UPDATE ──────────────────
     * Initialization runs before any per-frame work; EarlyUpdate is
     * for input poll and event-drain style hooks that need to see a
     * fresh frame.  Engine-owned input update remains at END_OF_FRAME
     * below (it drives swap-edge / just-pressed detection). */
    {   uint64_t _t0_pl_init = jce_time_perf_counter();
        jce_player_loop_run_phase(JCE_PHASE_INITIALIZATION, dt);
        jce_perf_phase_add("pl_init",
            jce_time_perf_to_ms(_t0_pl_init, jce_time_perf_counter())); }
    if (!engine_owner_ready(e)) return;
    {   uint64_t _t0_pl_early = jce_time_perf_counter();
        jce_player_loop_run_phase(JCE_PHASE_EARLY_UPDATE, dt);
        jce_perf_phase_add("pl_early",
            jce_time_perf_to_ms(_t0_pl_early, jce_time_perf_counter())); }
    if (!engine_owner_ready(e)) return;

    /* DEBUG TOGGLE: input record / replay (JCE_INPUT_RECORD /
     * JCE_INPUT_REPLAY, opened in jce_engine_create).  This sits right
     * before the action-map update on purpose: replay injects the recorded
     * snapshot BEFORE anything reads input this frame, so raw-input
     * queries AND actions observe the replayed state; record captures the
     * exact snapshot the game is about to read.  The END_OF_FRAME
     * jce_input_update below rolls cur→prev as usual, so pressed/released
     * edges reconstruct identically on replay.  At replay EOF: log once,
     * close, and keep running on live input. */
    if (e->input_replayer && e->input) {
        if (!jce_input_replay_tick(e->input_replayer, e->input)) {
            LOG_WARN(LOG_TAG,
                "input replay finished after %llu frames — back to live input",
                (unsigned long long)
                    jce_input_record_frame_count(e->input_replayer));
            jce_input_record_close(e->input_replayer);
            e->input_replayer = NULL;
        }
    } else if (e->input_recorder && e->input) {
        if (!jce_input_record_tick(e->input_recorder, e->input)) {
            LOG_WARN(LOG_TAG, "input record write failed — recording stopped");
            jce_input_record_close(e->input_recorder);
            e->input_recorder = NULL;
        }
    }

#if defined(JCE_ENABLE_AI_DISPATCH) && JCE_ENABLE_AI_DISPATCH
    /* ai_dispatch: constraint records enter the frame at input parity —
     * this sits beside the input record/replay hook on purpose.  Replay
     * pumps the .jarc stream gated by the canonical tick; live results
     * pump within the frame budget (spec H/J).  No-op until the host
     * calls jce_aid_init(). */
    if (jce_aid_initialised())
        jce_aid_engine_tick(jce_fixed_clock_default()->tick_count, 0);
#endif
    if (!engine_owner_ready(e)) return;

    /* QW-input-actions — evaluate the action map against the current
     * input snapshot so FIXED_UPDATE / UPDATE consumers (games, camera
     * controller) read fresh action values this frame.  Raw-input queries
     * remain available in parallel; this only resurrects the action layer.
     * prev_value bookkeeping (for jce_action_pressed/_released) advances
     * once per call here, matching the once-per-frame edge contract. */
    if (e->actions && e->input)
        jce_actions_update(e->actions, e->input);

    /* ── PlayerLoop: FIXED_UPDATE (P3-B.2) ──────────────────────────
     * Glenn Fiedler accumulator: drives 0..N fixed steps per frame so
     * physics + future deterministic netcode see a stable cadence
     * regardless of render rate.  Spiral-of-death clamp lives inside
     * jce_fixed_clock_advance.  This is the SAME clock the per-runtime
     * physics step adopts its cadence from (P1-fixed-clock-unify), so a
     * jce_engine_set_fixed_hz() change reaches physics too.  Configurable
     * via jce_engine_set_fixed_hz (default 60 Hz, == physics step). */
    {
        JceFixedClock *fc = jce_fixed_clock_default();
        const uint32_t steps = jce_fixed_clock_advance(fc, (double)dt);
        const float    fdt   = (float)fc->fixed_dt;
        for (uint32_t i = 0; i < steps; ++i) {
            jce_player_loop_run_phase(JCE_PHASE_FIXED_UPDATE, fdt);
            jce_fixed_clock_tick(fc);
            if (!engine_owner_ready(e)) return;
        }
    }

    /* Tick registered subsystems. */
    if (e->subsystems) {
        JCE_PROFILE_ZONE_N("Subsystems::Update");
        uint64_t _t0_sub = jce_time_perf_counter();
        jce_subsystem_update_all(e->subsystems, dt);
        jce_perf_phase_add("subsystems",
                           jce_time_perf_to_ms(_t0_sub, jce_time_perf_counter()));
        JCE_PROFILE_ZONE_END;
    }
    if (!engine_owner_ready(e)) return;

    /* ── PlayerLoop: UPDATE ─────────────────────────────────────────
     * Gameplay / ECS world tick.  Runs after legacy subsystem update
     * so phase consumers observe the same world state the app does. */
    {   uint64_t _t0_pl_update = jce_time_perf_counter();
        jce_player_loop_run_phase(JCE_PHASE_UPDATE, dt);
        jce_perf_phase_add("pl_update",
            jce_time_perf_to_ms(_t0_pl_update, jce_time_perf_counter())); }
    if (!engine_owner_ready(e)) return;

    const uint64_t app_started = jce_time_perf_counter();
    engine_app_update_only(&g_app_desc, dt);
    jce_perf_phase_add("app_update",
        jce_time_perf_to_ms(app_started, jce_time_perf_counter()));
    if (!engine_owner_ready(e)) return;

    /* ── PlayerLoop: LATE_UPDATE ────────────────────────────────────
     * Post-gameplay: cameras, IK, anim post-processing. */
    {   uint64_t _t0_pl_late = jce_time_perf_counter();
        jce_player_loop_run_phase(JCE_PHASE_LATE_UPDATE, dt);
        jce_perf_phase_add("pl_late",
            jce_time_perf_to_ms(_t0_pl_late, jce_time_perf_counter())); }

}
