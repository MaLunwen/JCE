/*
 * test_jce_editor_reflect_registry.cpp — pure registry coverage.
 *
 * jce_reflect.cpp was split (Nov 2025) into:
 *   - jce_reflect.cpp        (this TU: register/find/count/at — pure C)
 *   - jce_reflect_draw.cpp   (ImGui drawer — not tested here)
 *
 * The split lets the registry compile into the test exe without
 * pulling in <jce/tools/jce_imgui.hpp>, jce_editor_state.h, or any
 * other UI/state surface. No stub_includes shim required.
 *
 * Behaviour verified:
 *   - NULL inputs are no-ops (defensive guards)
 *   - register stores types in insertion order; count() reflects size
 *   - duplicate by-pointer is rejected
 *   - duplicate by display_name is rejected (string compare wins even
 *     when the JceReflectType pointer differs)
 *   - find() matches by display_name; missing / NULL returns nullptr
 *   - at() bounds: negative, in-range, past-end
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_reflect.h"

#include <cstddef>

namespace {

struct Foo { float x; int   n; };
struct Bar { int   k; float y; };

const JceReflectField k_foo_fields[] = {
    { "x", "X", JCE_FT_FLOAT, offsetof(Foo, x), sizeof(float),
      0.0f, 0.0f, 0.0f, nullptr, 0, nullptr, nullptr, JCE_FT_NONE, 0, 0, 0 },
    { "n", "N", JCE_FT_INT,   offsetof(Foo, n), sizeof(int),
      0.0f, 0.0f, 0.0f, nullptr, 0, nullptr, nullptr, JCE_FT_NONE, 0, 0, 0 },
};
const JceReflectType k_foo_type = {
    "Foo", "Foo", sizeof(Foo), k_foo_fields, 2, nullptr
};

const JceReflectField k_bar_fields[] = {
    { "k", "K", JCE_FT_INT,   offsetof(Bar, k), sizeof(int),
      0.0f, 0.0f, 0.0f, nullptr, 0, nullptr, nullptr, JCE_FT_NONE, 0, 0, 0 },
    { "y", "Y", JCE_FT_FLOAT, offsetof(Bar, y), sizeof(float),
      0.0f, 0.0f, 0.0f, nullptr, 0, nullptr, nullptr, JCE_FT_NONE, 0, 0, 0 },
};
const JceReflectType k_bar_type = {
    "Bar", "Bar", sizeof(Bar), k_bar_fields, 2, nullptr
};

/* Different pointer, same display name as k_foo_type — should be
 * rejected as a duplicate. */
const JceReflectType k_foo_alias = {
    "Foo", "FooAlias", sizeof(Foo), k_foo_fields, 2, nullptr
};

} /* namespace */

TEST_CASE("reflect registry: NULL guards")
{
    const int base = jce_reflect_count();
    jce_reflect_register(nullptr);
    CHECK(jce_reflect_count() == base);
    CHECK(jce_reflect_find(nullptr) == nullptr);
    CHECK(jce_reflect_find("nonexistent-XYZ") == nullptr);
}

TEST_CASE("reflect registry: register + find + count + at")
{
    const int base = jce_reflect_count();

    jce_reflect_register(&k_foo_type);
    jce_reflect_register(&k_bar_type);

    CHECK(jce_reflect_count() == base + 2);

    const JceReflectType *foo = jce_reflect_find("Foo");
    const JceReflectType *bar = jce_reflect_find("Bar");
    REQUIRE(foo != nullptr);
    REQUIRE(bar != nullptr);
    CHECK(foo == &k_foo_type);
    CHECK(bar == &k_bar_type);
    CHECK(foo->size == sizeof(Foo));
    CHECK(foo->field_count == 2);
    CHECK(bar->size == sizeof(Bar));

    /* at() — last two entries are the ones we just inserted, in order. */
    const JceReflectType *last_minus_1 = jce_reflect_at(base + 0);
    const JceReflectType *last         = jce_reflect_at(base + 1);
    CHECK(last_minus_1 == &k_foo_type);
    CHECK(last         == &k_bar_type);

    /* Out-of-range. */
    CHECK(jce_reflect_at(-1) == nullptr);
    CHECK(jce_reflect_at(jce_reflect_count()) == nullptr);
}

TEST_CASE("reflect registry: duplicate rejected (by pointer)")
{
    const int before = jce_reflect_count();
    jce_reflect_register(&k_foo_type);   /* already registered above */
    CHECK(jce_reflect_count() == before);
}

TEST_CASE("reflect registry: duplicate rejected (by display_name)")
{
    const int before = jce_reflect_count();
    jce_reflect_register(&k_foo_alias);  /* same name "Foo", diff ptr */
    CHECK(jce_reflect_count() == before);
    /* find() still returns the original, not the alias. */
    CHECK(jce_reflect_find("Foo") == &k_foo_type);
}
