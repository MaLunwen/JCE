/* jce_aid_transport_mock.c -- scripted transport for tests (spec F.4:
 * all cascade/timeout logic is mock-covered, no real network needed).
 * FIFO of scripted outcomes; optional artificial delay simulates slow
 * providers (executed on the calling = worker thread). */
#include "jce_aid_transport.h"

#include <jce/os/core/jce_thread.h>

#define MOCK_MAX_SCRIPT 32

typedef struct MockEntry {
    int      fail;      /* nonzero: transport failure */
    int      status;
    char*    body;      /* module-owned copy */
    uint32_t delay_ms;
} MockEntry;

static struct {
    MockEntry entries[MOCK_MAX_SCRIPT];
    uint32_t  head, count;
    uint32_t  post_calls;
    JceAidTransport vtable;
    int       installed;
} g_mock;

static int mock_post(void* self, const char* url, const char* bearer,
                     const char* body, uint32_t timeout_ms,
                     int* out_status, char** out_body, size_t* out_len)
{
    MockEntry* e;
    (void)self; (void)url; (void)bearer; (void)body; (void)timeout_ms;

    g_mock.post_calls++;
    if (g_mock.count == 0) return 1; /* nothing scripted: fail */

    e = &g_mock.entries[g_mock.head];
    g_mock.head = (g_mock.head + 1u) % MOCK_MAX_SCRIPT;
    g_mock.count--;

    if (e->delay_ms) jce_thread_sleep_ms(e->delay_ms);
    if (e->fail) {
        if (e->body) { jce_aid_free(e->body); e->body = NULL; }
        return 1;
    }
    *out_status = e->status;
    if (e->body) {
        size_t n = strlen(e->body);
        char*  copy = (char*)jce_aid_malloc(n + 1);
        if (!copy) { jce_aid_free(e->body); e->body = NULL; return 1; }
        memcpy(copy, e->body, n + 1);
        *out_body = copy;
        *out_len  = n;
        jce_aid_free(e->body);
        e->body = NULL;
    } else {
        *out_body = NULL;
        *out_len  = 0;
    }
    return 0;
}

void jce_aid_mock_install(void)
{
    jce_aid_mock_reset();
    g_mock.vtable.post_json = mock_post;
    g_mock.vtable.self      = NULL;
    g_mock.installed        = 1;
    jce_aid_set_transport(&g_mock.vtable);
}

void jce_aid_mock_reset(void)
{
    uint32_t i;
    for (i = 0; i < MOCK_MAX_SCRIPT; ++i) {
        if (g_mock.entries[i].body) {
            jce_aid_free(g_mock.entries[i].body);
            g_mock.entries[i].body = NULL;
        }
        g_mock.entries[i].fail = 0;
        g_mock.entries[i].status = 0;
        g_mock.entries[i].delay_ms = 0;
    }
    g_mock.head = 0;
    g_mock.count = 0;
    g_mock.post_calls = 0;
}

static MockEntry* mock_alloc_entry(void)
{
    uint32_t slot;
    JCE_AID_ASSERT(g_mock.count < MOCK_MAX_SCRIPT);
    if (g_mock.count >= MOCK_MAX_SCRIPT) return NULL;
    slot = (g_mock.head + g_mock.count) % MOCK_MAX_SCRIPT;
    g_mock.count++;
    return &g_mock.entries[slot];
}

void jce_aid_mock_push_response(int status, const char* body,
                                uint32_t delay_ms)
{
    MockEntry* e = mock_alloc_entry();
    if (!e) return;
    e->fail = 0;
    e->status = status;
    e->delay_ms = delay_ms;
    e->body = body ? jce_aid_strdup(body) : NULL;
}

void jce_aid_mock_push_fail(uint32_t delay_ms)
{
    MockEntry* e = mock_alloc_entry();
    if (!e) return;
    e->fail = 1;
    e->status = 0;
    e->delay_ms = delay_ms;
    e->body = NULL;
}

uint32_t jce_aid_mock_post_calls(void)
{
    return g_mock.post_calls;
}
