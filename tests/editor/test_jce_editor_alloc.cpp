// test_jce_editor_alloc.cpp — editor allocator macros round-trip.
//
// `jce_editor_alloc.h` is a header-only wrapper around the engine's
// public allocator vtable.  It must:
//   1. Return a stable (cached) vtable pointer across calls.
//   2. Allocate -> read/write -> free without leaks or asan hits.
//   3. ED_CALLOC must zero-initialize the buffer.
//   4. ED_REALLOC must preserve existing bytes when growing.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_alloc.h"

#include <cstring>

TEST_CASE("ed_alloc_default_ptr returns a stable, cached vtable")
{
    const jce_allocator_t *a = ed_alloc_default_ptr();
    const jce_allocator_t *b = ed_alloc_default_ptr();
    REQUIRE(a != nullptr);
    CHECK(a == b);
    CHECK(a->alloc   != nullptr);
    CHECK(a->free    != nullptr);
    CHECK(a->realloc != nullptr);
}

TEST_CASE("ED_MALLOC / ED_FREE round-trip")
{
    constexpr size_t N = 128;
    void *p = ED_MALLOC(N);
    REQUIRE(p != nullptr);
    std::memset(p, 0xAB, N);
    /* Touch the bytes back through a byte view to defeat the optimizer. */
    CHECK(static_cast<unsigned char *>(p)[0] == 0xAB);
    CHECK(static_cast<unsigned char *>(p)[N - 1] == 0xAB);
    ED_FREE(p);
}

TEST_CASE("ED_CALLOC zero-initializes")
{
    constexpr size_t N = 64;
    void *p = ED_CALLOC(N, 1);
    REQUIRE(p != nullptr);
    bool all_zero = true;
    for (size_t i = 0; i < N; ++i) {
        if (static_cast<unsigned char *>(p)[i] != 0) { all_zero = false; break; }
    }
    CHECK(all_zero);
    ED_FREE(p);
}

TEST_CASE("ED_REALLOC preserves prefix bytes when growing")
{
    constexpr size_t N1 = 16;
    constexpr size_t N2 = 256;
    unsigned char *p = static_cast<unsigned char *>(ED_MALLOC(N1));
    REQUIRE(p != nullptr);
    for (size_t i = 0; i < N1; ++i) p[i] = static_cast<unsigned char>(i + 1);

    unsigned char *q = static_cast<unsigned char *>(ED_REALLOC(p, N2));
    REQUIRE(q != nullptr);
    bool prefix_ok = true;
    for (size_t i = 0; i < N1; ++i) {
        if (q[i] != static_cast<unsigned char>(i + 1)) { prefix_ok = false; break; }
    }
    CHECK(prefix_ok);
    ED_FREE(q);
}

TEST_CASE("ED_FREE on null is a no-op")
{
    ED_FREE(nullptr);
    CHECK(true);
}
