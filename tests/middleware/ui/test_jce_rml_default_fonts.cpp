/* Exact bridge once + real RmlUi font/layout/atlas and bgfx Noop.
 * Resolver ports return only original licensed fixture bytes, not system font
 * copies. CJK role admission below does not certify CJK glyph coverage. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#ifndef JCE_RML_OWNER_SOURCE
#define JCE_RML_OWNER_SOURCE "middleware/ui/jce_ui_rmlui.cpp"
#endif
#include JCE_RML_OWNER_SOURCE
#include <jce/resource/jce_archive_writer.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_trace.h>
#include <jce/renderer/jce_renderer.h>
#include <cstdio>
#include <cmath>

static char test_font_path[1024];
static int latin_mode, cjk_mode, symbols_mode, latin_calls, cjk_calls, symbols_calls;
extern "C" bool jce_test_rml_host_default_font(JceHostFontRole role, char *out, size_t cap)
{
    const int mode = role == JCE_HOST_FONT_UI_LATIN ? (++latin_calls, latin_mode)
        : role == JCE_HOST_FONT_UI_CJK ? (++cjk_calls, cjk_mode)
        : role == JCE_HOST_FONT_UI_SYMBOLS ? (++symbols_calls, symbols_mode) : 0;
    if (!out || !cap) return false;
    out[0] = '\0';
    if (!mode || std::strlen(test_font_path) + 1 > cap) return false;
    std::memcpy(out, test_font_path, std::strlen(test_font_path) + 1);
    return true;
}

/* Rml clears contexts before NotifyShutdown, while core_data still exists.
 * Its GetNumContexts must not be queried after core_data.Shutdown. A real
 * borrowed plugin observes context destruction and the zero count in that
 * valid owner interval. NotifyShutdown unregisters the plugin automatically. */
struct RmlReleaseWitness final : Rml::Plugin {
    unsigned context_destroy_calls = 0, shutdown_calls = 0;
    int contexts_at_shutdown = -1;
    int GetEventClasses() override { return EVT_BASIC; }
    void OnContextDestroy(Rml::Context *) override { ++context_destroy_calls; }
    void OnShutdown() override
    {
        ++shutdown_calls;
        contexts_at_shutdown = Rml::GetNumContexts();
        std::fprintf(stderr, "[rml-font-fixture] phase=shutdown-witness contexts=%d context_destroy_calls=%u shutdown_calls=%u\n",
                     contexts_at_shutdown, context_destroy_calls, shutdown_calls);
        std::fflush(stderr);
    }
};

struct RmlFixture {
    JceRenderer *renderer = nullptr;
    JceRmlBackend *backend = nullptr;
    JcePakArchive *pak = nullptr;
    JceArchiveWriter *writer = nullptr;
    void *font = nullptr, *blob = nullptr, *scratch = nullptr;
    uint64_t font_size = 0;
    char root[1024] = {};
    uint16_t baseline_textures = 0;
    bool trace_started = false;
    RmlReleaseWitness release_witness;
    void mark(const char *phase, unsigned value = 0) const
    {
        std::fprintf(stderr, "[rml-font-fixture] phase=%s value=%u root=%s\n", phase, value, root);
        std::fflush(stderr);
    }
    ~RmlFixture()
    {
        mark("cleanup:begin");
        if (backend) {
            mark("rml-destroy:begin");
            jce_rml_destroy(backend);
            backend = nullptr;
            mark("rml-destroy:end");
        }
        if (writer) jce_archive_writer_destroy(writer);
        if (pak) jce_pak_close(pak);
        jce_fs_buffer_free(font); jce_free(blob); jce_fs_buffer_free(scratch);
        if (renderer) {
            /* REQUIRE can unwind before close_and_check. Deliver pending Noop
             * resource commands before the actual renderer shutdown/join. */
            mark("cleanup-noop-delivery:begin");
            for (int i = 0; i < 6; ++i) frame();
            mark("cleanup-noop-delivery:end");
            mark("renderer-destroy:begin");
            jce_renderer_destroy(renderer);
            renderer = nullptr;
            mark("renderer-destroy:end");
        }
        if (trace_started) jce_trace_set_enabled(false);
        if (root[0]) {
            char normalized[1024] = {}, base[1024] = {};
            const bool safe = jce_path_normalize(normalized, sizeof normalized, root)
                && jce_path_normalize(base, sizeof base, JCE_RML_TEST_OUTPUT_BASE);
            const size_t n = std::strlen(base);
            if (safe && n && std::strncmp(normalized, base, n) == 0
                && normalized[n] == '/' && std::strncmp(normalized+n+1, "case", 4) == 0
                && !std::strchr(normalized+n+1, '/'))
                (void)jce_fs_host_remove_recursive(normalized);
        }
    }
    /* Headless JCE end_frame intentionally returns without rendering. This
     * backend integration fixture explicitly delivers its real Noop resource
     * commands; it does not clone a production loop or prove GPU completion. */
    void frame() { (void)bgfx_frame(false); }
    uint16_t textures()
    {
        mark("texture-sync:begin");
        for (int i = 0; i < 6; ++i) frame();
        JceGpuStats stats = {};
        REQUIRE(jce_renderer_get_gpu_stats(&stats));
        mark("texture-sync:end", (unsigned)stats.num_textures);
        return stats.num_textures;
    }
    void add(const char *name, const void *bytes, size_t size)
    { REQUIRE(jce_archive_writer_add(writer, name, bytes, size)); }
    void prepare(bool valid_stock, int latin, int cjk, int symbols = 0,
                 bool stock_cjk = false)
    {
        mark("prepare:begin");
        latin_mode = latin; cjk_mode = cjk; symbols_mode = symbols;
        latin_calls = cjk_calls = symbols_calls = 0;
        font = jce_fs_host_read_all(JCE_RML_TEST_STOCK_FONT, &font_size);
        REQUIRE(font); REQUIRE(font_size == 12676u);
        char desired[1024];
        REQUIRE(jce_path_is_absolute(JCE_RML_TEST_OUTPUT_BASE));
        REQUIRE(jce_fs_host_create_directory(JCE_RML_TEST_OUTPUT_BASE));
        REQUIRE(jce_path_join(desired, sizeof desired, JCE_RML_TEST_OUTPUT_BASE, "case"));
        REQUIRE(jce_fs_host_make_unique_path(desired, root, sizeof root));
        REQUIRE(jce_fs_host_create_directory(root));
        REQUIRE(jce_path_join(test_font_path, sizeof test_font_path, root, "source.ttf"));
        if (latin == 3) REQUIRE(jce_fs_host_write_all(test_font_path, "invalid font", 12));
        else if (latin != 2) REQUIRE(jce_fs_host_write_all(test_font_path, font, font_size));
        JceArchiveWriterConfig config = {}; config.emit_debug_paths = true;
        writer = jce_archive_writer_create(&config); REQUIRE(writer);
        if (valid_stock) add("fonts/Aileron-Regular.ttf", font, (size_t)font_size);
        else add("fonts/Aileron-Regular.ttf", "invalid font", 12);
        if (stock_cjk) add("fonts/JCE-CJK-Fallback.ttf", font, (size_t)font_size);
        add("fonts/ProjectFace.ttf", font, (size_t)font_size);
        add("fonts/BadFace.ttf", "invalid font", 12);
        add("fonts/JCE.ttf", "not an automatic default", 24);
        const char *documents[] = {"engine_debug_hud.rml", "engine_console_overlay.rml", "engine_settings.rml"};
        for (const char *leaf : documents) {
            char file[1024]; REQUIRE(jce_path_join(file, sizeof file, JCE_RML_TEST_STOCK_UI, leaf));
            uint64_t size = 0; scratch = jce_fs_host_read_all(file, &size); REQUIRE(scratch);
            REQUIRE(std::strstr(static_cast<const char *>(scratch), "font-family: \"JCE-UI\""));
            add(leaf, scratch, (size_t)size); jce_fs_buffer_free(scratch); scratch = nullptr;
        }
        size_t size = 0; REQUIRE(jce_archive_writer_finish(writer, &blob, &size));
        jce_archive_writer_destroy(writer); writer = nullptr;
        pak = jce_pak_open_owned(blob, size); REQUIRE(pak); blob = nullptr;
        mark("renderer-create:begin");
        renderer = jce_renderer_create_headless(); REQUIRE(renderer);
        mark("renderer-create:end");
        baseline_textures = textures();
        jce_trace_set_enabled(false); jce_trace_reset(); jce_trace_set_enabled(true); trace_started = true;
        mark("rml-create:begin");
        backend = jce_rml_create(640, 480, renderer, pak); REQUIRE(backend);
        Rml::RegisterPlugin(&release_witness);
        mark("rml-create:end");
    }
    bool has_face(const char *family)
    {
        auto *fonts = Rml::GetFontEngineInterface(); REQUIRE(fonts);
        /* Direct FontEngineInterface callers obey Rml's lowercase-family
         * contract; CSS normally performs this normalization before lookup. */
        const Rml::String normalized = Rml::StringUtilities::ToLower(family);
        return fonts->GetFontFaceHandle(normalized, Rml::Style::FontStyle::Normal,
            Rml::Style::FontWeight::Normal, 22) != 0;
    }
    void inspect_box(const char *role, Rml::Element *node)
    {
        if (!node) return;
        const auto dims = node->GetBox().GetSize(Rml::BoxArea::Border);
        std::fprintf(stderr, "[rml-font-fixture] phase=box-inspect role=%s tag=%s id=%s offset=%.3fx%.3f border=%.3fx%.3f boxes=%d children=%d\n",
            role, node->GetTagName().c_str(), node->GetId().c_str(),
            (double)node->GetOffsetWidth(), (double)node->GetOffsetHeight(),
            (double)dims.x, (double)dims.y, node->GetNumBoxes(), node->GetNumChildren());
        const char *properties[] = {"display", "position", "width", "height", "font-size", "white-space"};
        for (const char *name : properties) {
            const Rml::Property *property = node->GetProperty(name);
            const Rml::String value = property ? property->ToString() : "<none>";
            std::fprintf(stderr, "[rml-font-fixture] phase=property-inspect role=%s property=%s value=%s\n",
                role, name, value.c_str());
        }
        const int count = node->GetNumBoxes();
        for (int i = 0; i < count && i < 3; ++i) {
            Rml::Vector2f offset;
            const auto size = node->GetBox(i, offset).GetSize(Rml::BoxArea::Border);
            std::fprintf(stderr, "[rml-font-fixture] phase=fragment-inspect role=%s index=%d border=%.3fx%.3f offset=%.3f,%.3f\n",
                role, i, (double)size.x, (double)size.y, (double)offset.x, (double)offset.y);
        }
        std::fflush(stderr);
    }
    void show_text(const char *family)
    {
        mark("layout:begin");
        const Rml::String sample_text = "Wide WABC 0123";
        /* Rml documents have authored CSS, not browser body defaults. The
         * document must span its context; an auto-sized document containing
         * only an absolute child has no in-flow intrinsic width. This sizes
         * the viewport, never the font witness's measured content. */
        std::string markup = "<rml><head><style>body{width:100%;height:100%;font-family:\"" + std::string(family)
            + "\";font-size:22px;color:#ffffff;}#sample{display:inline-block;position:absolute;left:0px;top:0px;}"
            "</style></head><body><span id=\"sample\">" + sample_text + "</span></body></rml>";
        const uint32_t doc = jce_rml_doc_load(backend, "font-witness", markup.data(), (uint32_t)markup.size());
        REQUIRE(doc != UINT32_MAX); jce_rml_doc_show(backend, doc);
        jce_rml_update(backend, 0.0f); jce_rml_update(backend, 0.0f);
        const uint32_t element = jce_rml_find_element(backend, doc, "sample"); REQUIRE(element != UINT32_MAX);
        float x = 0, y = 0, w = 0, h = 0;
        REQUIRE(jce_rml_elem_get_bounds(backend, element, &x, &y, &w, &h));
        const auto context_dimensions = backend->context->GetDimensions();
        std::fprintf(stderr, "[rml-font-fixture] phase=context-inspect width=%d height=%d\n",
                     context_dimensions.x, context_dimensions.y);
        Rml::Element *diagnostic_sample = backend->elements[element];
        inspect_box("document", backend->documents[doc]);
        CHECK(backend->documents[doc]->GetOffsetWidth() == (float)context_dimensions.x);
        CHECK(backend->documents[doc]->GetOffsetHeight() == (float)context_dimensions.y);
        inspect_box("parent", diagnostic_sample->GetParentNode());
        inspect_box("sample", diagnostic_sample);
        if (diagnostic_sample->GetNumChildren() > 0)
            inspect_box("text-child", diagnostic_sample->GetChild(0));
        CHECK(w > 1.0f); CHECK(h > 1.0f);
        /* A single intrinsic inline-block has a measurable public border box;
         * a fragmented inline span need not. No authored fixed width hides a
         * missing face: compare layout to the real same-face font owner. */
        REQUIRE(element < backend->elements.size());
        Rml::Element *sample = backend->elements[element]; REQUIRE(sample);
        const Rml::FontFaceHandle face = sample->GetFontFaceHandle(); REQUIRE(face != 0);
        auto *fonts = Rml::GetFontEngineInterface(); REQUIRE(fonts);
        const auto &computed = sample->GetComputedValues();
        const Rml::TextShapingContext shaping{computed.language(), computed.direction(),
            computed.font_kerning(), computed.letter_spacing()};
        const int measured = fonts->GetStringWidth(face, Rml::StringView(sample_text), shaping);
        REQUIRE(measured > 1);
        /* Font metrics are integer pixels; layout coordinates can round. */
        CHECK(std::abs(w - (float)measured) <= 1.0f);
        std::fprintf(stderr, "[rml-font-fixture] phase=intrinsic-measure layout_width=%.3f font_width=%d height=%.3f\n",
                     (double)w, measured, (double)h);
        std::fflush(stderr);
        mark("layout:end");
        mark("atlas-render:begin");
        jce_rml_render(backend); frame();
        mark("atlas-render:end");
        JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
        JceTraceEvent events[512]; const uint32_t n = jce_trace_read(&cursor, events, 512);
        bool geometry = false, atlas = false;
        for (uint32_t i = 0; i < n; ++i) {
            if (events[i].type != JCE_TRACE_EVENT_COUNTER || !events[i].value_u64) continue;
            geometry |= std::strcmp(events[i].name, "ui.rml.geometry.compile") == 0;
            atlas |= std::strcmp(events[i].name, "ui.rml.texture.generated") == 0;
        }
        CHECK(geometry); CHECK(atlas); CHECK(cursor.lost_events == 0);
        CHECK(textures() > baseline_textures);
    }
    void close_and_check()
    {
        const unsigned before_shutdown = release_witness.shutdown_calls;
        const unsigned before_context_destroy = release_witness.context_destroy_calls;
        mark("rml-destroy:begin");
        jce_rml_destroy(backend); backend = nullptr;
        mark("rml-destroy:end");
        CHECK(release_witness.shutdown_calls == before_shutdown + 1);
        CHECK(release_witness.context_destroy_calls == before_context_destroy + 1);
        CHECK(release_witness.contexts_at_shutdown == 0);
        CHECK_FALSE(s_rml_backend_owned);
        CHECK(Rml::GetSystemInterface() == nullptr);
        CHECK(Rml::GetRenderInterface() == nullptr);
        CHECK(Rml::GetFileInterface() == nullptr);
        CHECK(Rml::GetFontEngineInterface() == nullptr);
        CHECK(textures() == baseline_textures);
        JceTraceStats stats; jce_trace_get_stats(&stats);
        CHECK(stats.active_work_items == 0); CHECK(jce_trace_task_current_id() == 0);
        mark("close-check:end");
    }
};
TEST_CASE("Rml default roles retain actual source bytes through font layout and Noop atlas")
{
    RmlFixture f; f.prepare(true, 1, 1, 1);
    CHECK(latin_calls == 1); CHECK(cjk_calls == 1); CHECK(symbols_calls == 1);
    REQUIRE(f.backend->default_font_loaded); CHECK(f.backend->default_cjk_loaded);
    CHECK(f.backend->font_data_buffers.count == 3);
    CHECK(f.has_face("JCE-UI")); CHECK(f.has_face("JCE-UI-CJK")); CHECK_FALSE(f.has_face("JCE"));
    CHECK(f.has_face("JCE-UI-Symbols"));
    REQUIRE(jce_fs_host_remove_file(test_font_path)); /* bytes already independently retained */
    f.show_text("JCE-UI"); f.show_text("JCE-UI-Symbols"); f.close_and_check();
}
TEST_CASE("Rml missing host roles consume licensed stock and all three default documents")
{
    RmlFixture f; f.prepare(true, 0, 0);
    CHECK(symbols_calls == 1); CHECK_FALSE(f.has_face("JCE-UI-Symbols"));
    REQUIRE(f.backend->default_font_loaded); CHECK_FALSE(f.backend->default_cjk_loaded);
    CHECK(f.backend->font_data_buffers.count == 1); CHECK(f.has_face("JCE-UI"));
    const char *documents[] = {"engine_debug_hud.rml", "engine_console_overlay.rml", "engine_settings.rml"};
    for (const char *path : documents) CHECK(jce_rml_doc_load_file(f.backend, path) != UINT32_MAX);
    f.show_text("JCE-UI"); f.close_and_check();
}
TEST_CASE("Rml rejected host face falls back without retaining rejected bytes")
{
    RmlFixture f; f.prepare(true, 3, 0);
    REQUIRE(f.backend->default_font_loaded);
    CHECK(f.backend->font_data_buffers.count == 1); CHECK(f.backend->font_data_buffers.bytes == f.font_size);
    f.show_text("JCE-UI"); f.close_and_check();
}
TEST_CASE("Rml missing CJK and symbols use one licensed stock fallback buffer")
{
    RmlFixture f; f.prepare(true, 0, 0, 0, true);
    CHECK(latin_calls == 1); CHECK(cjk_calls == 1); CHECK(symbols_calls == 1);
    REQUIRE(f.backend->default_font_loaded); REQUIRE(f.backend->default_cjk_loaded);
    CHECK(f.backend->font_data_buffers.count == 2);
    CHECK(f.has_face("JCE-UI-CJK"));
    f.show_text("JCE-UI-CJK"); f.close_and_check();
}
TEST_CASE("Rml explicit project face survives missing defaults and rejected project retry")
{
    RmlFixture f; f.prepare(false, 2, 0);
    CHECK_FALSE(f.backend->default_font_loaded); CHECK(f.backend->font_data_buffers.count == 0);
    CHECK_FALSE(jce_rml_load_font(f.backend, "fonts/BadFace.ttf"));
    CHECK(f.backend->font_data_buffers.count == 0);
    REQUIRE(jce_rml_load_font(f.backend, "fonts/ProjectFace.ttf"));
    CHECK(f.has_face("ProjectFace")); CHECK_FALSE(f.has_face("JCE-UI"));
    CHECK(f.backend->font_data_buffers.count == 1);
    f.show_text("ProjectFace"); f.close_and_check();
}
TEST_CASE("Rml duplicate owner rejection leaves first context alive and permits clean retry")
{
    RmlFixture f; f.prepare(true, 0, 0);
    CHECK(jce_rml_create(640, 480, f.renderer, f.pak) == nullptr);
    CHECK(Rml::GetNumContexts() == 1); f.show_text("JCE-UI"); f.close_and_check();
    jce_trace_set_enabled(false); jce_trace_reset(); jce_trace_set_enabled(true);
    f.backend = jce_rml_create(640, 480, f.renderer, f.pak); REQUIRE(f.backend);
    Rml::RegisterPlugin(&f.release_witness); /* prior Shutdown unregistered it */
    REQUIRE(f.backend->default_font_loaded); f.show_text("JCE-UI"); f.close_and_check();
}
