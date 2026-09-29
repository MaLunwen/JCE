// test_jce_editor_i18n.cpp — pure paths of the editor i18n facade.
//
// We do NOT initialize the i18n system here (that requires loading a
// real PAK archive).  Instead we verify the uninitialized-state contract
// every translated call must honour:
//
//   * jce_editor_i18n(key)          -> returns key unchanged
//   * jce_editor_i18n_or(key, fb)   -> returns fb (never key)
//   * jce_editor_i18n_lookup_locale -> returns NULL
//   * locale getters / setters      -> safe defaults / clamped writes
//   * jce_editor_i18n_id            -> deterministic "label###id" format
//   * jce_editor_i18n_combo         -> NUL-separated string suitable for
//                                       ImGui::Combo (immediate form)
//
// The whole TU compiles cleanly without ImGui because the editor i18n
// module only depends on jce_json + jce_pak_loader.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_i18n.h"

#include <cstring>

TEST_CASE("locale_count is zero before init (locales are runtime-discovered)")
{
    /* The compile-time locale enum was replaced by a registry populated by
       scanning the PAK for i18n/*.json; with no PAK loaded here, no locales
       are discovered yet. */
    CHECK(jce_editor_i18n_locale_count() == 0);
}

TEST_CASE("get_locale defaults to JCE_LOCALE_EN")
{
    CHECK(jce_editor_i18n_get_locale() == JCE_LOCALE_EN);
}

TEST_CASE("set_locale clamps out-of-range values")
{
    JceLocale before = jce_editor_i18n_get_locale();
    jce_editor_i18n_set_locale(static_cast<JceLocale>(-1));
    CHECK(jce_editor_i18n_get_locale() == before);
    jce_editor_i18n_set_locale(static_cast<JceLocale>(JCE_MAX_LOCALES + 5));
    CHECK(jce_editor_i18n_get_locale() == before);
}

TEST_CASE("uninitialized jce_editor_i18n returns the key unchanged")
{
    const char *r = jce_editor_i18n("some.key");
    REQUIRE(r != nullptr);
    CHECK(std::strcmp(r, "some.key") == 0);
}

TEST_CASE("uninitialized jce_editor_i18n is null-safe")
{
    const char *r = jce_editor_i18n(nullptr);
    REQUIRE(r != nullptr);
    CHECK(r[0] == '\0');
}

TEST_CASE("uninitialized jce_editor_i18n_or returns the fallback (not the key)")
{
    const char *r = jce_editor_i18n_or("missing.key", "Hello");
    REQUIRE(r != nullptr);
    CHECK(std::strcmp(r, "Hello") == 0);
}

TEST_CASE("uninitialized jce_editor_i18n_or with null fallback returns empty string")
{
    const char *r = jce_editor_i18n_or("k", nullptr);
    REQUIRE(r != nullptr);
    /* Contract: when uninitialized, the fallback path runs first; with a
       null fallback the function returns "" (does NOT fall back to key). */
    CHECK(r[0] == '\0');
}

TEST_CASE("uninitialized lookup_locale always returns NULL")
{
    CHECK(jce_editor_i18n_lookup_locale(JCE_LOCALE_EN,             "x") == nullptr);
    CHECK(jce_editor_i18n_lookup_locale(static_cast<JceLocale>(1), "x") == nullptr);
    CHECK(jce_editor_i18n_lookup_locale(static_cast<JceLocale>(-1), "x") == nullptr);
}

TEST_CASE("jce_editor_i18n_id formats key###suffix.key when both provided")
{
    /* Uninitialized: i18n("inspector.transform") -> "inspector.transform". */
    const char *r = jce_editor_i18n_id("inspector.transform", "uim");
    REQUIRE(r != nullptr);
    CHECK(std::strstr(r, "inspector.transform") != nullptr);
    CHECK(std::strstr(r, "###uim.inspector.transform") != nullptr);
}

TEST_CASE("jce_editor_i18n_id falls back to plain text when no id_suffix")
{
    const char *r = jce_editor_i18n_id("k", nullptr);
    REQUIRE(r != nullptr);
    /* Format: "k###k" (key embedded as ID for stability). */
    CHECK(std::strstr(r, "###k") != nullptr);
}

TEST_CASE("jce_editor_i18n_combo builds NUL-separated entries")
{
    const char *keys[] = { "alpha", "beta", "gamma" };
    const char *r = jce_editor_i18n_combo(keys, 3);
    REQUIRE(r != nullptr);
    /* Sequence: "alpha\0beta\0gamma\0\0" (since uninit returns keys). */
    CHECK(std::strcmp(r, "alpha") == 0);
    const char *p = r + std::strlen(r) + 1;
    CHECK(std::strcmp(p, "beta") == 0);
    p += std::strlen(p) + 1;
    CHECK(std::strcmp(p, "gamma") == 0);
}

TEST_CASE("shutdown is safe to call without init")
{
    jce_editor_i18n_shutdown();
    /* And idempotent. */
    jce_editor_i18n_shutdown();
    CHECK(true);
}
