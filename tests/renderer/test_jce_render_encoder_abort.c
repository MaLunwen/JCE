/* Complete private encoder owner with typed CPU recording ports. No device. */
#include "unity.h"
#include <bgfx/c99/bgfx.h>
#include "renderer/jce_shader_deferred.h"
#include <string.h>

typedef struct PreparedState {
    bgfx_encoder_t *encoder;
    unsigned uniforms, commands, submits, discards;
    uint64_t state;
    uint8_t discard_flags, submit_flags;
    uint16_t submitted_program, indirect;
    bgfx_view_id_t view;
    uint32_t depth, start, num;
} PreparedState;
static PreparedState implicit_state, worker_state, other_state;
static unsigned char worker_identity, other_identity;
static unsigned resolves;
static uint16_t resolved_input, resolve_result;

static PreparedState *recording(bgfx_encoder_t *encoder)
{
    if (!encoder) return &implicit_state;
    if (encoder == worker_state.encoder) return &worker_state;
    TEST_ASSERT_EQUAL_PTR(other_state.encoder, encoder);
    return &other_state;
}
static uint16_t counted_resolve(uint16_t program)
{
    ++resolves;
    resolved_input = program;
    return program == JCE_DEFERRED_FIRST ? resolve_result : program;
}
static void uniform(bgfx_encoder_t *encoder, bgfx_uniform_handle_t handle,
    const void *value, uint16_t num)
{
    PreparedState *pending = recording(encoder);
    TEST_ASSERT_EQUAL_UINT16(19, handle.idx);
    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_EQUAL_UINT16(1, num);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, pending->uniforms,
        "a rejected draw retained its uniform in the next draw");
    ++pending->uniforms;
    ++pending->commands;
}
static void counted_uniform(bgfx_uniform_handle_t handle, const void *value, uint16_t num)
{ uniform(NULL, handle, value, num); }
static void counted_encoder_uniform(bgfx_encoder_t *encoder, bgfx_uniform_handle_t handle,
    const void *value, uint16_t num)
{ uniform(encoder, handle, value, num); }
static void state(bgfx_encoder_t *encoder, uint64_t bits, uint32_t rgba)
{
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0xaabbccdd), rgba);
    recording(encoder)->state = bits;
}
static void counted_state(uint64_t bits, uint32_t rgba) { state(NULL, bits, rgba); }
static void counted_encoder_state(bgfx_encoder_t *encoder, uint64_t bits, uint32_t rgba)
{ state(encoder, bits, rgba); }
static void discard(bgfx_encoder_t *encoder, uint8_t flags)
{
    PreparedState *pending = recording(encoder);
    ++pending->discards;
    pending->discard_flags = flags;
    /* ALL is required for this aborted command range and complete draw state. */
    if (flags == BGFX_DISCARD_ALL) {
        pending->uniforms = pending->commands = 0;
        pending->state = 0;
    }
}
static void counted_discard(uint8_t flags) { discard(NULL, flags); }
static void counted_encoder_discard(bgfx_encoder_t *encoder, uint8_t flags)
{ discard(encoder, flags); }
static void submitted(bgfx_encoder_t *encoder, bgfx_view_id_t view,
    bgfx_program_handle_t program, uint32_t depth, uint8_t flags)
{
    PreparedState *pending = recording(encoder);
    ++pending->submits;
    pending->submitted_program = program.idx;
    pending->view = view;
    pending->depth = depth;
    pending->submit_flags = flags;
}
static void counted_submit(bgfx_view_id_t view, bgfx_program_handle_t program,
    uint32_t depth, uint8_t flags)
{ submitted(NULL, view, program, depth, flags); }
static void counted_encoder_submit(bgfx_encoder_t *encoder, bgfx_view_id_t view,
    bgfx_program_handle_t program, uint32_t depth, uint8_t flags)
{ submitted(encoder, view, program, depth, flags); }
static void counted_indirect(bgfx_view_id_t view, bgfx_program_handle_t program,
    bgfx_indirect_buffer_handle_t indirect, uint32_t start, uint32_t num,
    uint32_t depth, uint8_t flags)
{
    implicit_state.indirect = indirect.idx;
    implicit_state.start = start;
    implicit_state.num = num;
    submitted(NULL, view, program, depth, flags);
}

#define bgfx_set_uniform counted_uniform
#define bgfx_encoder_set_uniform counted_encoder_uniform
#define bgfx_set_state counted_state
#define bgfx_encoder_set_state counted_encoder_state
#define bgfx_discard counted_discard
#define bgfx_encoder_discard counted_encoder_discard
#define bgfx_submit counted_submit
#define bgfx_encoder_submit counted_encoder_submit
#define bgfx_submit_indirect counted_indirect
#define jce_shader_deferred_resolve counted_resolve
#include JCE_TEST_RENDER_ENCODER_OWNER

#if defined(_MSC_VER)
__declspec(thread) bgfx_encoder_t *jce_tls_encoder;
#else
__thread bgfx_encoder_t *jce_tls_encoder;
#endif
uint32_t jce_dbg_xform_matrices;

void setUp(void)
{
    memset(&implicit_state, 0, sizeof implicit_state);
    memset(&worker_state, 0, sizeof worker_state);
    memset(&other_state, 0, sizeof other_state);
    worker_state.encoder = (bgfx_encoder_t *)&worker_identity;
    other_state.encoder = (bgfx_encoder_t *)&other_identity;
    resolves = 0;
    resolve_result = UINT16_MAX;
    resolved_input = 0;
    jce_dbg_xform_matrices = 73;
    jce_render_encoder_set(NULL);
}
void tearDown(void) { jce_render_encoder_set(NULL); }

typedef enum Route { TLS, IMPLICIT, INDIRECT } Route;
static void prepare(Route route)
{
    bgfx_uniform_handle_t handle = {19};
    const float value[4] = {1, 2, 3, 4};
    if (route == TLS) {
        jce_enc_set_uniform(handle, value, 1);
        jce_enc_set_state(UINT64_C(0x1000000003), UINT32_C(0xaabbccdd));
    } else {
        counted_uniform(handle, value, 1);
        counted_state(UINT64_C(0x1000000003), UINT32_C(0xaabbccdd));
    }
}
static void submit(Route route, uint16_t program, uint8_t flags)
{
    bgfx_program_handle_t handle = {program};
    bgfx_indirect_buffer_handle_t indirect = {47};
    if (route == TLS) jce_enc_submit(23, handle, UINT32_C(0xf1234567), flags);
    else if (route == IMPLICIT)
        jce_enc_submit_implicit(23, handle, UINT32_C(0xf1234567), flags);
    else jce_enc_submit_indirect_implicit(23, handle, indirect, 7, 11,
        UINT32_C(0xf1234567), flags);
}
static void assert_aborted(const PreparedState *pending)
{
    TEST_ASSERT_EQUAL_UINT32(1, pending->discards);
    TEST_ASSERT_EQUAL_HEX8(BGFX_DISCARD_ALL, pending->discard_flags);
    TEST_ASSERT_EQUAL_UINT32(0, pending->submits);
    TEST_ASSERT_EQUAL_UINT32(0, pending->uniforms);
    TEST_ASSERT_EQUAL_UINT32(0, pending->commands);
    TEST_ASSERT_EQUAL_UINT64(0, pending->state);
}
static void assert_success(const PreparedState *pending, uint16_t program, uint8_t flags)
{
    TEST_ASSERT_EQUAL_UINT32(1, pending->submits);
    TEST_ASSERT_EQUAL_UINT16(program, pending->submitted_program);
    TEST_ASSERT_EQUAL_UINT16(23, pending->view);
    TEST_ASSERT_EQUAL_UINT32(UINT32_C(0xf1234567), pending->depth);
    TEST_ASSERT_EQUAL_HEX8(flags, pending->submit_flags);
    TEST_ASSERT_EQUAL_UINT32(1, pending->uniforms);
    TEST_ASSERT_EQUAL_UINT32(1, pending->commands);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(0x1000000003), pending->state);
    TEST_ASSERT_EQUAL_UINT32(73, jce_dbg_xform_matrices);
}
static void rejected_then_valid(Route route, bool worker)
{
    jce_render_encoder_set(worker ? worker_state.encoder : NULL);
    prepare(route);
    submit(route, JCE_DEFERRED_FIRST, BGFX_DISCARD_NONE);
    PreparedState *pending = route == TLS && worker ? &worker_state : &implicit_state;
    TEST_ASSERT_EQUAL_UINT32(1, resolves);
    TEST_ASSERT_EQUAL_UINT16(JCE_DEFERRED_FIRST, resolved_input);
    assert_aborted(pending);
    prepare(route); /* Same handle must now be admissible on this recording. */
    submit(route, 41, BGFX_DISCARD_STATE | BGFX_DISCARD_TRANSFORM);
    TEST_ASSERT_EQUAL_UINT32(2, resolves);
    TEST_ASSERT_EQUAL_UINT32(1, pending->discards);
    assert_success(pending, 41, BGFX_DISCARD_STATE | BGFX_DISCARD_TRANSFORM);
    if (route == INDIRECT) {
        TEST_ASSERT_EQUAL_UINT16(47, pending->indirect);
        TEST_ASSERT_EQUAL_UINT32(7, pending->start);
        TEST_ASSERT_EQUAL_UINT32(11, pending->num);
    }
}
static void implicit_submit_retires_failed_preparation(void) { rejected_then_valid(TLS, false); }
static void worker_submit_retires_failed_preparation(void) { rejected_then_valid(TLS, true); }
static void explicit_implicit_submit_retires_failed_preparation(void)
{ rejected_then_valid(IMPLICIT, true); }
static void implicit_indirect_submit_retires_failed_preparation(void)
{ rejected_then_valid(INDIRECT, true); }
static void worker_abort_preserves_other_recordings(void)
{
    prepare(IMPLICIT);
    jce_render_encoder_set(other_state.encoder); prepare(TLS);
    jce_render_encoder_set(worker_state.encoder); prepare(TLS);
    submit(TLS, JCE_DEFERRED_FIRST, BGFX_DISCARD_ALL);
    assert_aborted(&worker_state);
    TEST_ASSERT_EQUAL_UINT32(1, implicit_state.uniforms);
    TEST_ASSERT_EQUAL_UINT32(1, implicit_state.commands);
    TEST_ASSERT_EQUAL_UINT32(0, implicit_state.discards);
    TEST_ASSERT_EQUAL_UINT32(1, other_state.uniforms);
    TEST_ASSERT_EQUAL_UINT32(1, other_state.commands);
    TEST_ASSERT_EQUAL_UINT32(0, other_state.discards);
    TEST_ASSERT_EQUAL_PTR(worker_state.encoder, jce_tls_encoder);
}
static void implicit_abort_preserves_unrelated_tls_recording(void)
{
    jce_render_encoder_set(worker_state.encoder); prepare(TLS);
    prepare(IMPLICIT); submit(IMPLICIT, JCE_DEFERRED_FIRST, BGFX_DISCARD_ALL);
    assert_aborted(&implicit_state);
    TEST_ASSERT_EQUAL_UINT32(1, worker_state.uniforms);
    TEST_ASSERT_EQUAL_UINT32(1, worker_state.commands);
    TEST_ASSERT_EQUAL_UINT32(0, worker_state.discards);
    TEST_ASSERT_EQUAL_PTR(worker_state.encoder, jce_tls_encoder);
}
static void successful_resolution_preserves_program_and_requested_flags(void)
{
    const uint8_t flags = BGFX_DISCARD_INDEX_BUFFER | BGFX_DISCARD_BINDINGS;
    for (unsigned route = TLS; route <= INDIRECT; ++route) {
        setUp();
        jce_render_encoder_set(worker_state.encoder);
        resolve_result = 89;
        prepare((Route)route); submit((Route)route, JCE_DEFERRED_FIRST, flags);
        const PreparedState *pending = route == TLS ? &worker_state : &implicit_state;
        TEST_ASSERT_EQUAL_UINT32(1, resolves);
        TEST_ASSERT_EQUAL_UINT16(JCE_DEFERRED_FIRST, resolved_input);
        TEST_ASSERT_EQUAL_UINT32(0, pending->discards);
        assert_success(pending, 89, flags);
    }
}
static void repeated_rejection_retires_each_prepared_attempt(void)
{
    jce_render_encoder_set(worker_state.encoder);
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        prepare(TLS); submit(TLS, JCE_DEFERRED_FIRST, BGFX_DISCARD_NONE);
        TEST_ASSERT_EQUAL_UINT32(attempt + 1, worker_state.discards);
        TEST_ASSERT_EQUAL_UINT32(0, worker_state.submits);
        TEST_ASSERT_EQUAL_UINT32(0, worker_state.commands);
    }
    TEST_ASSERT_EQUAL_UINT32(2, resolves);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(implicit_submit_retires_failed_preparation);
    RUN_TEST(worker_submit_retires_failed_preparation);
    RUN_TEST(explicit_implicit_submit_retires_failed_preparation);
    RUN_TEST(implicit_indirect_submit_retires_failed_preparation);
    RUN_TEST(worker_abort_preserves_other_recordings);
    RUN_TEST(implicit_abort_preserves_unrelated_tls_recording);
    RUN_TEST(successful_resolution_preserves_program_and_requested_flags);
    RUN_TEST(repeated_rejection_retires_each_prepared_attempt);
    return UNITY_END();
}
