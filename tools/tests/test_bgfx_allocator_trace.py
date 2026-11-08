"""Run the actual private trace entry against counted ring/profiler ports.

The production header's native tests exercise real aligned allocations, the
core ring, atomics and the bounded private identity ledger. These exact-TU controls guard the callback entry against accidentally
calling the optional profiler. They do not certify heap, GPU or bgfx execution.
"""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def load_helper(filename, name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools/tests" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


production_function = load_helper(
    "test_canvas_transparent_draw.py", "bgfx_counter_source").production_function
execute = load_helper("test_material_owned_cleanup.py", "bgfx_counter_compiler").execute


class RuntimeFailure:
    """The negative must compile successfully, then fail its runtime contract."""
    def __init__(self, test):
        self.test = test
        self.phase = 0

    def assertEqual(self, actual, expected, message):
        self.phase += 1
        if self.phase == 1:
            self.test.assertEqual(actual, expected, message)
        else:
            self.test.assertNotEqual(actual, expected, message)

    def skipTest(self, message):
        self.test.skipTest(message)


PORTS = r"""
#include <jce/os/core/jce_trace.h>
#include <assert.h>
#include <string.h>
static unsigned emitted, plotted;
static int s_enabled = 1;
static JceTraceEvent observed;
static int SDL_GetAtomicInt(const int *v) { return *v; }
static bool trace_thread_suppressed(void) { return false; }
#define JCE_PROFILE_PLOT_I(name_, value_) \
    do { (void)(name_); (void)(value_); ++plotted; } while (0)
static void trace_emit(JceTraceEventType type, JceTraceCategory category,
    uint64_t id, uint64_t parent, uint64_t value, uint32_t v0, uint32_t v1,
    const char *name)
{
    ++emitted;
    observed.type = type; observed.category = category;
    observed.id = id; observed.parent_id = parent; observed.value_u64 = value;
    observed.value_u32[0] = v0; observed.value_u32[1] = v1;
    assert(strlen(name) < sizeof(observed.name));
    memcpy(observed.name, name, strlen(name) + 1);
}
"""
CONTROL = r"""
int main(void)
{
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER, "bgfx.alloc.requested_size",
                                     -42, 37, 11);
    assert(emitted == 1 && plotted == 0);
    assert(observed.type == JCE_TRACE_EVENT_COUNTER);
    assert(observed.category == JCE_TRACE_CATEGORY_RENDER);
    assert(observed.id == 37 && observed.parent_id == 11);
    assert(observed.value_u64 == (uint64_t)(int64_t)-42);
    assert(observed.value_u32[0] == 0 && observed.value_u32[1] == 0);
    assert(strcmp(observed.name, "bgfx.alloc.requested_size") == 0);
    return 0;
}
"""


def ring_probe(profiled_owner=False):
    text = (ROOT / "engine/src/os/core/jce_trace.c").read_text(encoding="utf-8")
    name = "jce_trace_counter_scoped_internal" if profiled_owner else "jce_trace_counter_ring_internal"
    owner = production_function(text, "void " + name + "(")
    if profiled_owner:
        # Reuse the actual existing profiled owner, rather than inventing a
        # copied counter implementation. The ports expose its forbidden call.
        owner = owner.replace(name, "jce_trace_counter_ring_internal", 1)
    return PORTS + owner + CONTROL




LEDGER_PORTS = r"""
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <assert.h>
#include <string.h>
struct JceAtomicI32 { int32_t value; };
int32_t JCE_CALL jce_atomic_i32_load(const JceAtomicI32 *a) { return a->value; }
void JCE_CALL jce_atomic_i32_store(JceAtomicI32 *a, int32_t value) { a->value = value; }
int32_t JCE_CALL jce_atomic_i32_add(JceAtomicI32 *a, int32_t value)
{ int32_t old = a->value; a->value += value; return old; }
static bool jce_atomic_i32_compare_exchange_internal(JceAtomicI32 *a, int32_t old, int32_t value)
{ if (!a || a->value != old) return false; a->value = value; return true; }
static uint64_t next_id, warmups;
static JceAtomicI32 *guard;
static JceTraceEvent values[512];
static unsigned value_count, provider_calls;
bool JCE_CALL jce_trace_enabled(void) { return true; }
uint64_t JCE_CALL jce_trace_next_id(void) { return ++next_id; }
uint64_t JCE_CALL jce_trace_task_current_id(void) { return 17; }
uint32_t jce_trace_capture_generation(void) { return 1; }
uint64_t JCE_CALL jce_time_ticks_ns(void) { return ++warmups; }
void jce_trace_counter_ring_internal(JceTraceCategory category, const char *name,
                                    int64_t value, uint64_t id, uint64_t parent)
{
    assert(!guard || !jce_atomic_i32_load(guard));
    assert(value_count < sizeof(values) / sizeof(values[0]));
    assert(strlen(name) < JCE_TRACE_NAME_CAP);
    JceTraceEvent *event = &values[value_count++];
    event->category = category; event->id = id; event->parent_id = parent;
    event->value_u64 = (uint64_t)value; strcpy(event->name, name);
}
"""
LEDGER_CONTROL = r"""
static unsigned char pool[2][128];
static void *pool_port(void *ptr, size_t size, size_t alignment, void *user)
{
    (void)alignment; (void)user;
    assert(!jce_atomic_i32_load(guard)); ++provider_calls;
    if (!size) return NULL;
    assert(size <= sizeof(pool[0]));
    return !ptr ? pool[0] : ptr == pool[0] ? pool[1] : pool[0];
}
static uint64_t operation_value(unsigned index, const char *name)
{
    uint64_t id = 0;
    for (unsigned i = 0; i < value_count; ++i)
        if (!strcmp(values[i].name, "bgfx.alloc.request_index") && values[i].value_u64 == index)
            id = values[i].id;
    assert(id);
    for (unsigned i = 0; i < value_count; ++i)
        if (values[i].id == id && !strcmp(values[i].name, name)) return values[i].value_u64;
    assert(0); return 0;
}
static uint64_t summary_value(const char *name)
{
    for (unsigned i = value_count; i; --i)
        if (!strcmp(values[i-1].name, name)) return values[i-1].value_u64;
    assert(0); return 0;
}
int main(void)
{
    static JceBgfxAllocatorTrace observer;
    guard = &observer.metadata_gate;
    jce_bgfx_trace_begin(&observer, true, true);
    void *a = jce_bgfx_trace_reallocate(&observer, pool_port, NULL, NULL, 16, 0, 5);
    void *b = jce_bgfx_trace_reallocate(&observer, pool_port, NULL, a, 32, 0, 5);
    assert(a != b);
    assert(!jce_bgfx_trace_reallocate(&observer, pool_port, NULL, b, 0, 0, 5));
    void *c = jce_bgfx_trace_reallocate(&observer, pool_port, NULL, NULL, 16, 0, 5);
    assert(c == a);
    assert(!jce_bgfx_trace_reallocate(&observer, pool_port, NULL, c, 0, 0, 5));
    jce_bgfx_trace_end(&observer, JCE_BGFX_ALLOC_SUMMARY_JOINED);
    const uint64_t birth = operation_value(1, "bgfx.alloc.allocation_id");
    assert(birth && birth == operation_value(2, "bgfx.alloc.allocation_id"));
    assert(birth == operation_value(3, "bgfx.alloc.allocation_id"));
    assert(birth != operation_value(4, "bgfx.alloc.allocation_id"));
    assert(operation_value(2, "bgfx.alloc.old_size") == 16);
    assert(operation_value(2, "bgfx.alloc.new_size") == 32);
    assert(operation_value(2, "bgfx.alloc.allocation_revision") == 1);
    assert(summary_value("bgfx.alloc.all_released") == 1);
    assert(summary_value("bgfx.alloc.event_history_complete") == 1);
    assert(summary_value("bgfx.alloc.request_limit") == 0);
    assert(summary_value("bgfx.alloc.ledger_storage_bytes") == sizeof(observer.entries));
    assert(provider_calls == 5 && warmups == 1);
    return 0;
}
"""


def ledger_probe(mutation=None):
    # Compile the complete current private owner. Only dependency ports are
    # replaced; no ledger algorithm is copied into the harness. Native tests
    # separately provide actual OS atomics/ring/aligned heap and concurrency.
    owner = (ROOT / "engine/src/renderer/jce_bgfx_allocator_trace_internal.h").read_text(encoding="utf-8")
    for dependency in ("jce_atomic_storage.h", "jce_trace_scope_internal.h", "jce_trace_capture_internal.h"):
        owner = owner.replace('#include "os/core/' + dependency + '"', '')
    if mutation == "lost_resize_identity":
        old = "uint64_t allocation_id = old.serial;"
        assert owner.count(old) == 1
        owner = owner.replace(old, "uint64_t allocation_id = 0;")
    elif mutation == "address_identity":
        old = "allocation_id = ++state->next_allocation;"
        assert owner.count(old) == 1
        owner = owner.replace(old, "allocation_id = (uint64_t)address;")
    elif mutation == "locked_publication":
        old = '    jce_bgfx_trace_unlock(state);\n#define BR'
        assert owner.count(old) == 1
        owner = owner.replace(old,
            '    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER, "bgfx.alloc.locked_negative", 1, 0, 0);\n' + old)
    return LEDGER_PORTS + owner + LEDGER_CONTROL


class BgfxAllocatorTraceTests(unittest.TestCase):
    def test_ring_only_counter_preserves_data_without_profiler_callback(self):
        execute(self, ring_probe())

    def test_actual_profiled_owner_negative_cannot_pass_callback_contract(self):
        execute(RuntimeFailure(self), ring_probe(profiled_owner=True))


    def test_actual_private_ledger_pairs_moved_and_reused_addresses(self):
        execute(self, ledger_probe())

    def test_lost_resize_identity_mutant_fails_at_runtime(self):
        execute(RuntimeFailure(self), ledger_probe("lost_resize_identity"))

    def test_address_identity_mutant_fails_at_runtime(self):
        execute(RuntimeFailure(self), ledger_probe("address_identity"))

    def test_locked_publication_mutant_fails_at_runtime(self):
        execute(RuntimeFailure(self), ledger_probe("locked_publication"))


if __name__ == "__main__":
    unittest.main()
