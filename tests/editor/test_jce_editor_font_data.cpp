#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_alloc.h"
#include <jce/os/core/jce_alloc.h>
#include <cstring>
#include <initializer_list>

namespace {
struct FontReleaseProbe {
    void *source = nullptr;
    unsigned releases = 0;
    unsigned duplicates = 0;
};
FontReleaseProbe *s_release_probe = nullptr; // Fixture only; sequential atlas owner.

void checked_font_free(void *data)
{
    if (s_release_probe && data == s_release_probe->source) {
        ++s_release_probe->releases;
        if (s_release_probe->releases > 1) {
            ++s_release_probe->duplicates;
            return; // Count a regression without passing a freed pointer to mimalloc.
        }
    }
    ED_FREE(data);
}
} // namespace

// The same counted release port covers ImGui rollback and a regressed helper.
// This does not replace parsing, AddFont or the actual ownership transaction.
#pragma push_macro("ED_FREE")
#undef ED_FREE
#define ED_FREE(data) checked_font_free(data)
#pragma push_macro("jce_free")
#define jce_free(data) checked_font_free(data)
#include "ui/jce_editor_font_data.h"
#pragma pop_macro("jce_free")
#pragma pop_macro("ED_FREE")
#include <jce/tools/jce_imgui.hpp>
#include <cstdio>

namespace {
struct FontContext {
    ImGuiMemAllocFunc previous_alloc = nullptr;
    ImGuiMemFreeFunc previous_free = nullptr;
    void *previous_userdata = nullptr;
    FontReleaseProbe *previous_probe = nullptr;
    ImGuiContext *previous_context = nullptr;
    ImGuiContext *context = nullptr;

    explicit FontContext(FontReleaseProbe &probe)
    {
        ImGui::GetAllocatorFunctions(&previous_alloc, &previous_free, &previous_userdata);
        previous_probe = s_release_probe;
        previous_context = ImGui::GetCurrentContext();
        s_release_probe = &probe;
        ImGui::SetAllocatorFunctions(
            [](size_t size, void *) -> void * { return ED_MALLOC(size); },
            [](void *data, void *) { checked_font_free(data); }, nullptr);
        context = ImGui::CreateContext();
        // Keep the real default assert policy. Only the production owning-call
        // wrapper may temporarily allow a recoverable parser error to return.
        ImGuiIO &io = ImGui::GetIO();
        REQUIRE(io.ConfigErrorRecoveryEnableAssert);
        io.ConfigErrorRecoveryEnableDebugLog = true;
        io.ConfigErrorRecoveryEnableTooltip = false;
    }

    void destroy()
    {
        if (context) {
            ImGui::DestroyContext(context);
            context = nullptr;
            ImGui::SetCurrentContext(previous_context);
        }
    }
    ~FontContext()
    {
        destroy();
        ImGui::SetAllocatorFunctions(previous_alloc, previous_free, previous_userdata);
        s_release_probe = previous_probe;
    }
};
} // namespace

TEST_CASE("host font ownership transfers without a second input allocation")
{
    void *raw = jce_malloc(128);
    REQUIRE(raw != nullptr);
    std::memset(raw, 0x5a, 128);
    jce_alloc_frame_delta(nullptr, nullptr);
    void *data = ed_font_adopt_host_buffer(raw, 128);
    uint64_t allocations = 0, allocation_bytes = 0;
    jce_alloc_frame_delta(&allocations, &allocation_bytes);
    CHECK(allocations == 0);
    CHECK(allocation_bytes == 0);
    REQUIRE(data != nullptr);
    CHECK(data == raw);
    CHECK(static_cast<unsigned char *>(data)[127] == 0x5a);
    ED_FREE(data); // Actual public allocation / cached default free pairing.
}

TEST_CASE("invalid font sizes and owning typed consumer release exactly once")
{
    CHECK(ed_font_adopt_host_buffer(nullptr, 12) == nullptr);
    CHECK(ed_font_adopt_host_buffer(jce_malloc(1), 0) == nullptr);
    CHECK(ed_font_adopt_host_buffer(jce_malloc(1), uint64_t(INT_MAX) + 1) == nullptr);
    FontReleaseProbe probe;
    FontContext context(probe);
    int calls = 0, releases = 0;
    void *raw = jce_malloc(32);
    REQUIRE(raw != nullptr);
    probe.source = raw;
    CHECK(ed_font_transfer(raw, 32, [&](void *data, int bytes) -> void * {
        ++calls;
        CHECK(data == raw);
        CHECK(bytes == 32);
        checked_font_free(data); // Typed port models owning AddFont's failed rollback.
        ++releases;
        return nullptr;
    }) == nullptr);
    CHECK(calls == 1);
    CHECK(releases == 1);
    CHECK(probe.releases == 1);
    CHECK(probe.duplicates == 0);
    context.destroy();
    CHECK(probe.releases == 1);
    size_t bytes = 99;
    CHECK(ed_font_read_host("_missing_font_data_test_.ttf", &bytes) == nullptr);
    CHECK(bytes == 0);
}

TEST_CASE("real host font bytes remain valid through atlas Clear and reload")
{
    FontReleaseProbe probe;
    FontContext context(probe);
    ImGuiIO &io = ImGui::GetIO();
    ImFontAtlas seed;
    REQUIRE(seed.AddFontDefault() != nullptr);
    REQUIRE(seed.Sources.Size > 0);
    const ImFontConfig &source = seed.Sources[0];
    REQUIRE(source.FontData != nullptr);
    REQUIRE(source.FontDataSize > 0);
    const char *path = "_ut_font_data_builtin.ttf";
    REQUIRE(jce_fs_host_write_all(path, source.FontData, source.FontDataSize));
    ImFontAtlas atlas;
    for (int reload = 0; reload < 3; ++reload) {
        size_t bytes = 0;
        void *data = ed_font_read_host(path, &bytes);
        REQUIRE(data != nullptr);
        REQUIRE(bytes == size_t(source.FontDataSize));
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = true;
        ImFont *font = ed_font_transfer_recoverable(
            data, int(bytes), io.ConfigErrorRecoveryEnableAssert, [&](void *p, int n) {
            return atlas.AddFontFromMemoryTTF(p, n, 20.0f, &cfg);
        });
        REQUIRE(font != nullptr);
        CHECK(io.ConfigErrorRecoveryEnableAssert);
        REQUIRE(atlas.Sources.Size == 1);
        CHECK(atlas.Sources[0].FontData == data);
        CHECK(std::memcmp(data, source.FontData, bytes) == 0);
        // ImGui owns the original bytes for later on-demand rasterization.
        atlas.Clear();
        CHECK(atlas.Sources.Size == 0);
    }
    CHECK(jce_fs_host_remove_file(path));
}

TEST_CASE("real invalid font parser rollback consumes source before returning NULL")
{
    FontReleaseProbe probe;
    FontContext context(probe);
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    REQUIRE(atlas->Sources.Size == 0);
    void *data = ED_MALLOC(128);
    REQUIRE(data != nullptr);
    std::memset(data, 0, 128); // Unsupported signature, safely before table reads.
    probe.source = data;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = true;
    REQUIRE(ImGui::GetIO().ConfigErrorRecoveryEnableAssert);
    ImFont *font = ed_font_transfer_recoverable(
        data, 128, ImGui::GetIO().ConfigErrorRecoveryEnableAssert, [&](void *p, int bytes) {
        return atlas->AddFontFromMemoryTTF(p, bytes, 20.0f, &cfg);
    });
    data = nullptr; // Caller relinquished ownership at the owning invocation.
    CHECK(font == nullptr);
    CHECK(ImGui::GetIO().ConfigErrorRecoveryEnableAssert);
    CHECK(probe.releases == 1);
    CHECK(probe.duplicates == 0);
    CHECK(atlas->Sources.Size == 0);
    CHECK(atlas->Fonts.Size == 0);
    atlas->Clear();
    CHECK(probe.releases == 1);
    CHECK(probe.duplicates == 0);
    context.destroy();
    CHECK(probe.releases == 1);
    CHECK(probe.duplicates == 0);
}

TEST_CASE("real merged invalid font rollback preserves accepted font and releases once")
{
    FontReleaseProbe probe;
    FontContext context(probe);
    ImFontAtlas *atlas = ImGui::GetIO().Fonts;
    ImFont *accepted = atlas->AddFontDefault();
    REQUIRE(accepted != nullptr);
    REQUIRE(atlas->Sources.Size == 1);
    void *accepted_bytes = atlas->Sources[0].FontData;
    void *data = ED_MALLOC(128);
    REQUIRE(data != nullptr);
    std::memset(data, 0, 128);
    probe.source = data;
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = true;
    cfg.MergeMode = true;
    // Also preserve an existing recoverable policy, rather than forcing true.
    ImGui::GetIO().ConfigErrorRecoveryEnableAssert = false;
    CHECK(ed_font_transfer_recoverable(
        data, 128, ImGui::GetIO().ConfigErrorRecoveryEnableAssert, [&](void *p, int bytes) {
        return atlas->AddFontFromMemoryTTF(p, bytes, 20.0f, &cfg);
    }) == nullptr);
    data = nullptr;
    CHECK_FALSE(ImGui::GetIO().ConfigErrorRecoveryEnableAssert);
    CHECK(probe.releases == 1);
    CHECK(probe.duplicates == 0);
    REQUIRE(atlas->Sources.Size == 1);
    REQUIRE(atlas->Fonts.Size == 1);
    CHECK(atlas->Fonts[0] == accepted);
    CHECK(atlas->Sources[0].FontData == accepted_bytes);
    CHECK(accepted->Sources.Size == 1);
    atlas->Clear();
    CHECK(probe.releases == 1);
    CHECK(probe.duplicates == 0);
    context.destroy();
    CHECK(probe.releases == 1);
    CHECK(probe.duplicates == 0);
}

TEST_CASE("tiny host sources release before the owning AddFont admission")
{
    FontReleaseProbe probe;
    FontContext context(probe);
    for (uint64_t bytes : {uint64_t(1), uint64_t(100)}) {
        probe.releases = probe.duplicates = 0;
        void *raw = jce_malloc(size_t(bytes));
        REQUIRE(raw != nullptr);
        probe.source = raw;
        void *data = ed_font_adopt_host_buffer(raw, bytes);
        CHECK(data == nullptr);
        CHECK(probe.releases == 1);
        CHECK(probe.duplicates == 0);
        unsigned admission_calls = 0;
        if (data) {
            (void)ed_font_transfer_recoverable(
                data, int(bytes), ImGui::GetIO().ConfigErrorRecoveryEnableAssert,
                [&](void *p, int) -> ImFont * {
                    ++admission_calls;
                    checked_font_free(p);
                    return nullptr;
                });
        }
        CHECK(admission_calls == 0);
        CHECK(ImGui::GetIO().ConfigErrorRecoveryEnableAssert);
        CHECK(probe.releases == 1);
        CHECK(probe.duplicates == 0);
    }
    probe.source = nullptr;
}
