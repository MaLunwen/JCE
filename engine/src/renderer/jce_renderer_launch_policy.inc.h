/* Included after the opaque Renderer definition. No application dependency. */
#include <stdlib.h>
void jce_renderer_launch_vsync_observe_internal(JceRenderer *r, int phase)
{
    if (!r || !r->launch_vsync.bound) return;
    JceRendererLaunchVsync *policy = &r->launch_vsync;
    const bool effective = jce_renderer_get_vsync(r);
    if (phase == 2 && policy->observed && policy->observed_requested == policy->requested &&
        policy->observed_effective == effective) return;
    policy->observed = true; policy->observed_requested = policy->requested;
    policy->observed_effective = effective;
    jce_log_write(JCE_LOG_LEVEL_INFO, "launch-policy", __FILE__, __LINE__,
        "launch-vsync: phase=%d requested=%d override_present=%d override=%d owner_flag_effective=%d presentation_available=%d window_flags_valid=%d window_flags=%u hidden_d3d12=%d backend_ack=unknown",
        phase, policy->requested ? 1 : 0, policy->present ? 1 : 0, policy->value ? 1 : 0,
        effective ? 1 : 0, policy->available ? 1 : 0, policy->window_flags_valid ? 1 : 0,
        (unsigned)policy->window_flags, policy->hidden_d3d12 ? 1 : 0);
    JceTraceScope scope = jce_trace_scope_begin("renderer.launch_vsync");
    if (scope.id) {
#define VSYNC_DETAIL(name_, value_) jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, \
    "launch.vsync." name_, (int64_t)(value_), scope.id, scope.id)
        VSYNC_DETAIL("phase", phase);
        VSYNC_DETAIL("requested", policy->requested);
        VSYNC_DETAIL("override_present", policy->present);
        VSYNC_DETAIL("override", policy->value);
        VSYNC_DETAIL("owner_flag_effective", effective);
        VSYNC_DETAIL("presentation_available", policy->available);
        VSYNC_DETAIL("window_flags_valid", policy->window_flags_valid);
        VSYNC_DETAIL("window_flags", policy->window_flags);
        VSYNC_DETAIL("hidden_d3d12", policy->hidden_d3d12);
        VSYNC_DETAIL("backend_ack_known", 0);
        VSYNC_DETAIL("gpu_completion_known", 0);
#undef VSYNC_DETAIL
    }
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}

void jce_renderer_launch_vsync_bind_internal(JceRenderer *r, bool present,
    bool value, bool requested, bool available)
{
    if (!r || r->launch_vsync.bound) return;
    r->launch_vsync.bound = true;
    r->launch_vsync.present = present; r->launch_vsync.value = value;
    r->launch_vsync.requested = requested;
    r->launch_vsync.available = available && !r->is_fallback && !r->headless;
    r->launch_vsync.observed = false;
    jce_renderer_launch_vsync_observe_internal(r, 1);
}

static bool renderer_launch_vsync_resolve(JceRenderer *r, bool requested)
{
    r->launch_vsync.requested = requested;
    if (!r->launch_vsync.present) return requested;
    return r->launch_vsync.value;
}

static bool renderer_presentation_vsync_resolve(const JceRendererLaunchVsync *policy,
                                               bool requested)
{
    return requested && !policy->hidden_d3d12;
}

/* An unmapped D3D12 swapchain cannot supply display synchronization. Keep
 * requested/launch intent separate from this observed presentation constraint.
 * Observe only at backend creation and explicit platform rebind, not per frame. */
static uint32_t renderer_presentation_vsync_refresh(JceRendererLaunchVsync *policy,
    bgfx_renderer_type_t type, JceWindow *window, uint32_t reset_flags)
{
    const bool was_hidden_d3d12 = policy->hidden_d3d12;
    policy->window_flags_valid = false;
    policy->window_flags = 0;
    policy->hidden_d3d12 = false;
#if JCE_PLATFORM_WINDOWS
    if (type == BGFX_RENDERER_TYPE_DIRECT3D12) {
        policy->window_flags_valid = jce_window_get_flags(window, &policy->window_flags);
        policy->hidden_d3d12 = policy->window_flags_valid &&
            (policy->window_flags & JCE_WINDOW_HIDDEN) != 0;
    }
#else
    (void)type; (void)window;
#endif
    const uint32_t effective = renderer_presentation_vsync_resolve(policy,
        (reset_flags & BGFX_RESET_VSYNC) != 0) ? reset_flags : reset_flags & ~BGFX_RESET_VSYNC;
    if (was_hidden_d3d12 != policy->hidden_d3d12) {
        jce_log_write(JCE_LOG_LEVEL_INFO, "presentation-policy", __FILE__, __LINE__,
            "presentation-vsync: hidden_d3d12=%d window_flags_valid=%d window_flags=%u input_reset_flags=%u effective_reset_flags=%u backend_ack=unknown",
            policy->hidden_d3d12 ? 1 : 0, policy->window_flags_valid ? 1 : 0,
            (unsigned)policy->window_flags, (unsigned)reset_flags, (unsigned)effective);
    }
    return effective;
}

static void renderer_presentation_vsync_rebind(JceRenderer *r, JceWindow *window)
{
    const bool requested = renderer_launch_vsync_resolve(r, r->launch_vsync.requested);
    const uint32_t flags = (r->reset_flags & ~BGFX_RESET_VSYNC) |
        (requested ? BGFX_RESET_VSYNC : 0);
    r->reset_flags = renderer_presentation_vsync_refresh(&r->launch_vsync,
        bgfx_get_renderer_type(), window, flags);
}

/* These are startup experiments, not saved graphics settings or DXGI flip-model.
 * Bound each parse to two bytes: no whitespace, signs, prefixes or coercion. */
static bool renderer_launch_diagnostic_digit(const char *text, unsigned maximum,
    bool *present, uint8_t *value)
{
    *present = text && text[0];
    if (!*present) return true;
    if (text[0] < '0' || text[0] > (char)('0' + maximum) || text[1]) return false;
    *value = (uint8_t)(text[0] - '0');
    return true;
}

static JceRendererLaunchDiagnostics renderer_launch_diagnostics_snapshot(void)
{
    JceRendererLaunchDiagnostics request = {0};
    if (!renderer_launch_diagnostic_digit(getenv("JCE_GFX_FLIP_AFTER_RENDER"), 1,
            &request.flip_present, &request.flip)) request.invalid |= 1;
    if (!renderer_launch_diagnostic_digit(getenv("JCE_GFX_MAX_FRAME_LATENCY"), 3,
            &request.latency_present, &request.latency)) request.invalid |= 2;
    return request;
}

/* Pinned bgfx 1.146.9306-550: D3D12 passes its command queue to the shared
 * IDXGIDevice1 query, and GL has no consumer. Do not label these effective. */
static unsigned renderer_launch_latency_support(bgfx_renderer_type_t type)
{
    if (type == BGFX_RENDERER_TYPE_DIRECT3D11 || type == BGFX_RENDERER_TYPE_VULKAN) return 1;
    if (type == BGFX_RENDERER_TYPE_DIRECT3D12) return 2;
    if (type == BGFX_RENDERER_TYPE_OPENGL || type == BGFX_RENDERER_TYPE_OPENGLES) return 3;
    if (type == BGFX_RENDERER_TYPE_NOOP) return 4;
    return 5;
}

static bool renderer_launch_flip_applicable(bgfx_renderer_type_t type)
{
    return type != BGFX_RENDERER_TYPE_NOOP && type < BGFX_RENDERER_TYPE_COUNT;
}

static void renderer_launch_diagnostics_apply(const JceRendererLaunchDiagnostics *request,
    bgfx_init_t *init)
{
    if (!request || request->invalid) return;
    if (request->flip_present && renderer_launch_flip_applicable(init->type)) {
        init->resolution.reset &= ~BGFX_RESET_FLIP_AFTER_RENDER;
        if (request->flip) init->resolution.reset |= BGFX_RESET_FLIP_AFTER_RENDER;
    }
    if (request->latency_present && renderer_launch_latency_support(init->type) == 1)
        init->resolution.maxFrameLatency = request->latency;
}

/* phase: 0 invalid/refused, 1 init false, 2 admitted, 3 API refused,
 * 4 headless/Noop-only entry, 5 software fallback. No per-frame observer. */
static void renderer_launch_diagnostics_observe(const JceRendererLaunchDiagnostics *request,
    const bgfx_init_t *init, unsigned phase)
{
    if (!request || (!request->flip_present && !request->latency_present)) return;
    const bgfx_renderer_type_t type = init ? init->type : BGFX_RENDERER_TYPE_COUNT;
    const unsigned support = renderer_launch_latency_support(type);
    const bool attempted = init && phase > 0 && phase < 4;
    const bool flip_forwarded = attempted && !request->invalid && request->flip_present &&
        renderer_launch_flip_applicable(type);
    const bool latency_forwarded = attempted && !request->invalid && request->latency_present && support == 1;
    const int flip = request->invalid & 1 ? -1 : request->flip;
    const int latency = request->invalid & 2 ? -1 : request->latency;
    const int cpu_init = attempted ? (phase == 1 ? 0 : 1) : -1;
    const uint32_t flags = init ? init->resolution.reset : 0;
    const unsigned forwarded_latency = init ? init->resolution.maxFrameLatency : 0;
    /* Keep this below the existing 512-byte logger record, including maximal
     * numeric fields. Unknown backend ACK/GPU completion are never inferred. */
    jce_log_write(request->invalid ? JCE_LOG_LEVEL_WARN : JCE_LOG_LEVEL_INFO,
        "launch-policy", __FILE__, __LINE__,
        "launch-gfx: phase=%u backend=%u invalid=%u flip_present=%d flip_request=%d flip_forwarded=%d latency_present=%d latency_request=%d latency_forwarded=%d latency_support=%u reset=%u init_latency=%u cpu_init=%d backend_ack=unknown gpu_completion=unknown",
        phase, (unsigned)type, (unsigned)request->invalid, request->flip_present ? 1 : 0,
        flip, flip_forwarded ? 1 : 0, request->latency_present ? 1 : 0, latency,
        latency_forwarded ? 1 : 0, support, (unsigned)flags, forwarded_latency, cpu_init);
    JceTraceScope scope = jce_trace_scope_begin("renderer.launch_gfx");
    if (scope.id) {
#define LAUNCH_GFX_DETAIL(name_, value_) jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_RENDER, \
    "launch.gfx." name_, (int64_t)(value_), scope.id, scope.id)
        LAUNCH_GFX_DETAIL("phase", phase);
        LAUNCH_GFX_DETAIL("backend", type);
        LAUNCH_GFX_DETAIL("invalid", request->invalid);
        LAUNCH_GFX_DETAIL("flip_present", request->flip_present);
        LAUNCH_GFX_DETAIL("flip_request", flip);
        LAUNCH_GFX_DETAIL("flip_forwarded", flip_forwarded);
        LAUNCH_GFX_DETAIL("latency_present", request->latency_present);
        LAUNCH_GFX_DETAIL("latency_request", latency);
        LAUNCH_GFX_DETAIL("latency_forwarded", latency_forwarded);
        LAUNCH_GFX_DETAIL("latency_support", support);
        LAUNCH_GFX_DETAIL("reset_flags", flags);
        LAUNCH_GFX_DETAIL("init_latency", forwarded_latency);
        LAUNCH_GFX_DETAIL("cpu_init", cpu_init);
        LAUNCH_GFX_DETAIL("backend_ack_known", 0);
        LAUNCH_GFX_DETAIL("gpu_completion_known", 0);
#undef LAUNCH_GFX_DETAIL
    }
    jce_trace_scope_end(scope, phase == 2 ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED);
}
