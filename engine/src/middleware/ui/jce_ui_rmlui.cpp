/*
 * jce_ui_rmlui.cpp  RmlUi backend implementation for the JCE UI system.
 *
 * Implements the extern "C" bridge declared in jce_ui_backend.h by
 * delegating to RmlUi's Core API.
 *
 * Provides:
 *   - bgfx RenderInterface (transient buffer geometry on JCE_VIEW_UI)
 *   - PAK FileInterface (load RML/RCSS from archive)
 *   - Font loading via Rml::LoadFontFace
 *   - Input forwarding from JceInput → Rml::Context
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_pak_loader.h>
#include <jce/os/platform/jce_input.h>
#include <jce/renderer/jce_texture.h>
#include <jce/renderer/jce_views.h>

#include "jce_ui_backend.h"
#include "os/core/jce_memory.h"
#include "renderer/jce_renderer_internal.h"

#include <bgfx/c99/bgfx.h>
#include <RmlUi/Core.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#define LOG_TAG "ui.rml"

/* ================================================================== */
/* Vertex layout matching engine's PosColorTexVertex                   */
/* ================================================================== */

struct RmlBgfxVertex {
    float    x, y, z;
    uint32_t abgr;
    float    u, v;
};

static inline uint32_t rml_colour_to_abgr(const Rml::Colourb &c)
{
    return ((uint32_t)c.alpha << 24) | ((uint32_t)c.blue << 16)
         | ((uint32_t)c.green << 8)  |  (uint32_t)c.red;
}

/* ================================================================== */
/* System interface                                                    */
/* ================================================================== */

class JceRmlSystemInterface : public Rml::SystemInterface {
public:
    double GetElapsedTime() override { return elapsed_time_; }

    bool LogMessage(Rml::Log::Type type, const Rml::String &message) override
    {
        switch (type) {
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT:
            LOG_ERROR(LOG_TAG, "%s", message.c_str());
            break;
        case Rml::Log::LT_WARNING:
            LOG_WARN(LOG_TAG, "%s", message.c_str());
            break;
        case Rml::Log::LT_INFO:
            LOG_INFO(LOG_TAG, "%s", message.c_str());
            break;
        default:
            LOG_TRACE(LOG_TAG, "%s", message.c_str());
            break;
        }
        return true;
    }

    void AccumulateTime(double dt) { elapsed_time_ += dt; }

private:
    double elapsed_time_ = 0.0;
};

/* ================================================================== */
/* File interface (PAK archive)                                        */
/* ================================================================== */

class JceRmlFileInterface : public Rml::FileInterface {
public:
    explicit JceRmlFileInterface(JcePakArchive *pak) : pak_(pak) {}

    Rml::FileHandle Open(const Rml::String &path) override
    {
        if (!pak_) return 0;

        const JcePakAsset *asset = jce_pak_find(pak_, path.c_str());
        if (!asset) {
            LOG_WARN(LOG_TAG, "file not found in PAK: %s", path.c_str());
            return 0;
        }

        auto *f = new (std::nothrow) FileState();
        if (!f) return 0;

        f->size = (size_t)asset->original_size;
        f->data = (uint8_t *)JCE_MALLOC(f->size);
        if (!f->data) {
            delete f;
            return 0;
        }

        size_t decompressed = jce_pak_decompress(asset, f->data, f->size);
        if (decompressed == 0) {
            LOG_ERROR(LOG_TAG, "jce_pak_decompress failed: %s", path.c_str());
            JCE_FREE(f->data);
            delete f;
            return 0;
        }

        f->pos = 0;
        return reinterpret_cast<Rml::FileHandle>(f);
    }

    void Close(Rml::FileHandle file) override
    {
        auto *f = reinterpret_cast<FileState *>(file);
        if (f) {
            JCE_FREE(f->data);
            delete f;
        }
    }

    size_t Read(void *buffer, size_t size, Rml::FileHandle file) override
    {
        auto *f = reinterpret_cast<FileState *>(file);
        if (!f) return 0;

        size_t remaining = f->size - f->pos;
        size_t to_read = size < remaining ? size : remaining;
        memcpy(buffer, f->data + f->pos, to_read);
        f->pos += to_read;
        return to_read;
    }

    bool Seek(Rml::FileHandle file, long offset, int origin) override
    {
        auto *f = reinterpret_cast<FileState *>(file);
        if (!f) return false;

        long new_pos = 0;
        switch (origin) {
        case SEEK_SET: new_pos = offset; break;
        case SEEK_CUR: new_pos = (long)f->pos + offset; break;
        case SEEK_END: new_pos = (long)f->size + offset; break;
        default:       return false;
        }

        if (new_pos < 0 || (size_t)new_pos > f->size) return false;
        f->pos = (size_t)new_pos;
        return true;
    }

    size_t Tell(Rml::FileHandle file) override
    {
        auto *f = reinterpret_cast<FileState *>(file);
        return f ? f->pos : 0;
    }

    size_t Length(Rml::FileHandle file) override
    {
        auto *f = reinterpret_cast<FileState *>(file);
        return f ? f->size : 0;
    }

private:
    JcePakArchive *pak_;

    struct FileState {
        uint8_t *data = nullptr;
        size_t   size = 0;
        size_t   pos  = 0;
    };
};

/* ================================================================== */
/* Render interface (bgfx)                                             */
/* ================================================================== */

class JceRmlRenderInterface : public Rml::RenderInterface {
public:
    explicit JceRmlRenderInterface(JceRenderer *renderer)
        : renderer_(renderer) {}

    ~JceRmlRenderInterface() override
    {
        for (auto tex : generated_textures_)
            jce_texture_destroy(tex);
    }

    /* ── Immediate geometry (transient buffers) ────────────────────── */

    void RenderGeometry(Rml::Vertex *vertices, int num_vertices,
                        int *indices, int num_indices,
                        Rml::TextureHandle texture,
                        const Rml::Vector2f &translation) override
    {
        if (!renderer_ || num_vertices <= 0 || num_indices <= 0) return;

        bool textured = (texture != 0);
        const bgfx_vertex_layout_t *layout = textured
            ? jce_renderer_get_layout_textured(renderer_)
            : jce_renderer_get_layout(renderer_);
        bgfx_program_handle_t prog = textured
            ? jce_renderer_get_program_textured(renderer_)
            : jce_renderer_get_program(renderer_);

        if (!layout || prog.idx == UINT16_MAX) return;

        bgfx_transient_vertex_buffer_t tvb;
        bgfx_transient_index_buffer_t  tib;

        if (!bgfx_alloc_transient_buffers(&tvb, layout,
                (uint32_t)num_vertices, &tib,
                (uint32_t)num_indices, false))
            return;

        /* Copy vertices, converting Rml layout → engine layout. */
        if (textured) {
            auto *dst = reinterpret_cast<RmlBgfxVertex *>(tvb.data);
            for (int i = 0; i < num_vertices; i++) {
                dst[i].x    = vertices[i].position.x + translation.x;
                dst[i].y    = vertices[i].position.y + translation.y;
                dst[i].z    = 0.0f;
                dst[i].abgr = rml_colour_to_abgr(vertices[i].colour);
                dst[i].u    = vertices[i].tex_coord.x;
                dst[i].v    = vertices[i].tex_coord.y;
            }
        } else {
            /* Color-only vertex: {float x,y,z; uint32_t abgr} */
            struct PosColor { float x, y, z; uint32_t abgr; };
            auto *dst = reinterpret_cast<PosColor *>(tvb.data);
            for (int i = 0; i < num_vertices; i++) {
                dst[i].x    = vertices[i].position.x + translation.x;
                dst[i].y    = vertices[i].position.y + translation.y;
                dst[i].z    = 0.0f;
                dst[i].abgr = rml_colour_to_abgr(vertices[i].colour);
            }
        }

        /* Copy indices (RmlUi uses int, bgfx uses uint16_t). */
        auto *idx = reinterpret_cast<uint16_t *>(tib.data);
        for (int i = 0; i < num_indices; i++)
            idx[i] = (uint16_t)indices[i];

        bgfx_set_transient_vertex_buffer(0, &tvb, 0, (uint32_t)num_vertices);
        bgfx_set_transient_index_buffer(&tib, 0, (uint32_t)num_indices);

        if (textured) {
            JceTexture tex_handle;
            tex_handle.idx = (uint16_t)(texture & 0xFFFF);
            bgfx_texture_handle_t bgfx_tex = { tex_handle.idx };
            JceUniformHandle uh = jce_renderer_get_tex_uniform(renderer_);
            bgfx_uniform_handle_t sampler = { uh.idx };
            bgfx_set_texture(0, sampler, bgfx_tex, UINT32_MAX);
        }

          /* RmlUi font atlases are generated as transparent white with glyph
              coverage in alpha. Use standard alpha blending so fully transparent
              atlas texels do not render as solid white rectangles. */
          uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                              | BGFX_STATE_BLEND_ALPHA;

        if (scissor_enabled_)
            bgfx_set_scissor(scissor_x_, scissor_y_,
                             scissor_w_, scissor_h_);

        bgfx_set_state(state, 0);
        bgfx_submit(JCE_VIEW_UI, prog, 0, BGFX_DISCARD_ALL);
    }

    /* ── Scissor ──────────────────────────────────────────────────── */

    void EnableScissorRegion(bool enable) override
    {
        scissor_enabled_ = enable;
    }

    void SetScissorRegion(int x, int y, int width, int height) override
    {
        scissor_x_ = (uint16_t)(x > 0 ? x : 0);
        scissor_y_ = (uint16_t)(y > 0 ? y : 0);
        scissor_w_ = (uint16_t)(width  > 0 ? width  : 0);
        scissor_h_ = (uint16_t)(height > 0 ? height : 0);
    }

    /* ── Textures ─────────────────────────────────────────────────── */

    bool LoadTexture(Rml::TextureHandle &texture_handle,
                     Rml::Vector2i &texture_dimensions,
                     const Rml::String &source) override
    {
        /* RmlUI calls this for <img src="..."> in documents.
           We load from PAK via the engine's texture loader. */
        if (!pak_) return false;

        JceTexture tex = jce_texture_load(pak_, source.c_str());
        if (!jce_texture_valid(tex)) {
            LOG_WARN(LOG_TAG, "LoadTexture failed: %s", source.c_str());
            return false;
        }

        uint32_t w = 0, h = 0;
        jce_texture_get_size(tex, &w, &h);
        texture_handle = (Rml::TextureHandle)tex.idx;
        texture_dimensions = Rml::Vector2i((int)w, (int)h);

        generated_textures_.push_back(tex);
        return true;
    }

    bool GenerateTexture(Rml::TextureHandle &texture_handle,
                         const Rml::byte *source,
                         const Rml::Vector2i &source_dimensions) override
    {
          /* RmlUi generated textures are premultiplied RGBA. bgfx uses linear
              filtering by default when the point-sampler flags are omitted,
              which keeps UI text from looking blocky at non-integer scale. */
        if (!source || source_dimensions.x <= 0 || source_dimensions.y <= 0)
            return false;

        const uint32_t width = (uint32_t)source_dimensions.x;
        const uint32_t height = (uint32_t)source_dimensions.y;
        const bgfx_memory_t *mem = bgfx_alloc(width * height * 4);
        memcpy(mem->data, source, width * height * 4);

        bgfx_texture_handle_t handle = bgfx_create_texture_2d(
            (uint16_t)width, (uint16_t)height,
            false, 1,
            BGFX_TEXTURE_FORMAT_RGBA8,
            BGFX_TEXTURE_NONE
                | BGFX_SAMPLER_U_CLAMP
                | BGFX_SAMPLER_V_CLAMP,
            mem);

        if (handle.idx == UINT16_MAX)
            return false;

        JceTexture tex;
        tex.idx = handle.idx;
        if (!jce_texture_valid(tex)) return false;

        texture_handle = (Rml::TextureHandle)tex.idx;
        generated_textures_.push_back(tex);
        return true;
    }

    void ReleaseTexture(Rml::TextureHandle texture) override
    {
        JceTexture tex;
        tex.idx = (uint16_t)(texture & 0xFFFF);
        for (auto it = generated_textures_.begin();
             it != generated_textures_.end(); ++it) {
            if (it->idx == tex.idx) {
                jce_texture_destroy(*it);
                generated_textures_.erase(it);
                return;
            }
        }
    }

    void SetPak(JcePakArchive *pak) { pak_ = pak; }

private:
    JceRenderer *renderer_ = nullptr;
    JcePakArchive  *pak_      = nullptr;

    bool     scissor_enabled_ = false;
    uint16_t scissor_x_ = 0, scissor_y_ = 0;
    uint16_t scissor_w_ = 0, scissor_h_ = 0;

    std::vector<JceTexture> generated_textures_;
};

/* ================================================================== */
/* Event listener adapter                                              */
/* ================================================================== */

class JceRmlEventAdapter : public Rml::EventListener {
public:
    JceRmlEventAdapter(uint32_t elem_idx, jce_rml_event_fn fn, void *ud)
        : elem_idx_(elem_idx), fn_(fn), ud_(ud) {}

    void ProcessEvent(Rml::Event &event) override
    {
        if (fn_) {
            fn_(elem_idx_, event.GetType().c_str(), ud_);
        }
    }

private:
    uint32_t         elem_idx_;
    jce_rml_event_fn fn_;
    void            *ud_;
};

/* ================================================================== */
/* Backend struct                                                      */
/* ================================================================== */

struct JceRmlBackend {
    JceRmlSystemInterface  *sys_interface    = nullptr;
    JceRmlRenderInterface  *render_interface = nullptr;
    JceRmlFileInterface    *file_interface   = nullptr;
    Rml::Context           *context          = nullptr;

    JceRenderer            *renderer         = nullptr;
    JcePakArchive             *pak              = nullptr;

    std::vector<Rml::ElementDocument *> documents;
    std::vector<Rml::Element *>         elements;

    /* Keeps event adapters alive for the lifetime of the backend. */
    std::vector<JceRmlEventAdapter *>   event_adapters;

    /* Font data buffers — must remain alive while RmlUI uses the fonts.
       RmlUI::LoadFontFace(data,...) does NOT copy the data. */
    std::vector<void *>                 font_data_buffers;

    /* Temporary storage so elem_get_text can return a stable c_str(). */
    std::string                         temp_text;

    uint32_t width  = 0;
    uint32_t height = 0;
};

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

/* Reference height for dp scaling.  At 1080px the dp ratio is 1.0;
   smaller windows shrink, larger windows grow.  Clamped to [0.6, 4.0]. */
static float compute_dp_ratio(uint32_t h)
{
    float r = (float)h / 1080.0f;
    if (r < 0.6f)  r = 0.6f;
    if (r > 4.0f)  r = 4.0f;
    return r;
}

JceRmlBackend *jce_rml_create(uint32_t width, uint32_t height,
                              JceRenderer *renderer, JcePakArchive *pak)
{
    auto *b = new (std::nothrow) JceRmlBackend();
    if (!b) {
        LOG_ERROR(LOG_TAG, "failed to allocate JceRmlBackend");
        return nullptr;
    }

    b->width    = width;
    b->height   = height;
    b->renderer = renderer;
    b->pak      = pak;

    b->sys_interface    = new (std::nothrow) JceRmlSystemInterface();
    b->render_interface = new (std::nothrow) JceRmlRenderInterface(renderer);
    b->file_interface   = new (std::nothrow) JceRmlFileInterface(pak);
    if (!b->sys_interface || !b->render_interface || !b->file_interface) {
        LOG_ERROR(LOG_TAG, "failed to allocate RmlUi interfaces");
        delete b->file_interface;
        delete b->render_interface;
        delete b->sys_interface;
        delete b;
        return nullptr;
    }
    b->render_interface->SetPak(pak);

    Rml::SetSystemInterface(b->sys_interface);
    Rml::SetRenderInterface(b->render_interface);
    Rml::SetFileInterface(b->file_interface);

    if (!Rml::Initialise()) {
        LOG_ERROR(LOG_TAG, "Rml::Initialise() failed");
        delete b->file_interface;
        delete b->render_interface;
        delete b->sys_interface;
        delete b;
        return nullptr;
    }

    b->context = Rml::CreateContext("jce_ui",
                                   Rml::Vector2i((int)width, (int)height));
    if (!b->context) {
        LOG_ERROR(LOG_TAG, "Rml::CreateContext() failed");
        Rml::Shutdown();
        delete b->file_interface;
        delete b->render_interface;
        delete b->sys_interface;
        delete b;
        return nullptr;
    }

    float dp = compute_dp_ratio(height);
    b->context->SetDensityIndependentPixelRatio(dp);

    LOG_SUCCESS(LOG_TAG, "RmlUi backend created (%ux%u, dp=%.2f)",
                width, height, dp);
    return b;
}

void jce_rml_destroy(JceRmlBackend *b)
{
    if (!b) return;

    for (auto *adapter : b->event_adapters)
        delete adapter;
    b->event_adapters.clear();

    Rml::Shutdown();

    /* Free font data buffers AFTER Rml::Shutdown (FreeType is done). */
    for (void *buf : b->font_data_buffers)
        JCE_FREE(buf);
    b->font_data_buffers.clear();

    delete b->file_interface;
    delete b->render_interface;
    delete b->sys_interface;
    delete b;

    LOG_INFO(LOG_TAG, "RmlUi backend destroyed");
}

/* ================================================================== */
/* Documents                                                           */
/* ================================================================== */

uint32_t jce_rml_doc_load(JceRmlBackend *b, const char *name,
                          const char *markup, uint32_t len)
{
    if (!b || !b->context || !markup) return UINT32_MAX;
    (void)name;

    Rml::ElementDocument *doc =
        b->context->LoadDocumentFromMemory(std::string(markup, len));
    if (!doc) {
        LOG_ERROR(LOG_TAG, "LoadDocumentFromMemory failed for '%s'",
                  name ? name : "<null>");
        return UINT32_MAX;
    }

    uint32_t idx = (uint32_t)b->documents.size();
    b->documents.push_back(doc);
    LOG_INFO(LOG_TAG, "document '%s' loaded (idx=%u)", name ? name : "?", idx);
    return idx;
}

uint32_t jce_rml_doc_load_file(JceRmlBackend *b, const char *path)
{
    if (!b || !b->context || !path) return UINT32_MAX;

    Rml::ElementDocument *doc = b->context->LoadDocument(path);
    if (!doc) {
        LOG_ERROR(LOG_TAG, "LoadDocument failed for '%s'", path);
        return UINT32_MAX;
    }

    uint32_t idx = (uint32_t)b->documents.size();
    b->documents.push_back(doc);
    LOG_INFO(LOG_TAG, "document '%s' loaded (idx=%u)", path, idx);
    return idx;
}

void jce_rml_doc_show(JceRmlBackend *b, uint32_t doc_idx)
{
    if (!b || doc_idx >= (uint32_t)b->documents.size()) return;
    Rml::ElementDocument *doc = b->documents[doc_idx];
    if (doc) doc->Show();
}

void jce_rml_doc_hide(JceRmlBackend *b, uint32_t doc_idx)
{
    if (!b || doc_idx >= (uint32_t)b->documents.size()) return;
    Rml::ElementDocument *doc = b->documents[doc_idx];
    if (doc) doc->Hide();
}

void jce_rml_doc_close(JceRmlBackend *b, uint32_t doc_idx)
{
    if (!b || doc_idx >= (uint32_t)b->documents.size()) return;
    Rml::ElementDocument *doc = b->documents[doc_idx];
    if (doc) {
        doc->Close();
        b->documents[doc_idx] = nullptr;
    }
}

/* ================================================================== */
/* Elements                                                            */
/* ================================================================== */

uint32_t jce_rml_find_element(JceRmlBackend *b, uint32_t doc_idx,
                              const char *id)
{
    if (!b || !id || doc_idx >= (uint32_t)b->documents.size()) return UINT32_MAX;
    Rml::ElementDocument *doc = b->documents[doc_idx];
    if (!doc) return UINT32_MAX;

    Rml::Element *elem = doc->GetElementById(id);
    if (!elem) {
        LOG_WARN(LOG_TAG, "element '%s' not found in doc %u", id, doc_idx);
        return UINT32_MAX;
    }

    uint32_t idx = (uint32_t)b->elements.size();
    b->elements.push_back(elem);
    return idx;
}

void jce_rml_elem_set_text(JceRmlBackend *b, uint32_t elem_idx,
                           const char *text)
{
    if (!b || !text || elem_idx >= (uint32_t)b->elements.size()) return;
    Rml::Element *elem = b->elements[elem_idx];
    if (elem) elem->SetInnerRML(text);
}

const char *jce_rml_elem_get_text(JceRmlBackend *b, uint32_t elem_idx)
{
    if (!b || elem_idx >= (uint32_t)b->elements.size()) return "";
    Rml::Element *elem = b->elements[elem_idx];
    if (!elem) return "";

    b->temp_text = elem->GetInnerRML();
    return b->temp_text.c_str();
}

void jce_rml_elem_set_property(JceRmlBackend *b, uint32_t elem_idx,
                               const char *prop, const char *val)
{
    if (!b || !prop || !val || elem_idx >= (uint32_t)b->elements.size()) return;
    Rml::Element *elem = b->elements[elem_idx];
    if (elem) elem->SetProperty(prop, val);
}

const char *jce_rml_elem_get_value(JceRmlBackend *b, uint32_t elem_idx)
{
    if (!b || elem_idx >= (uint32_t)b->elements.size()) return "";
    Rml::Element *elem = b->elements[elem_idx];
    Rml::ElementFormControl *control = dynamic_cast<Rml::ElementFormControl *>(elem);
    if (!control) return "";

    b->temp_text = control->GetValue();
    return b->temp_text.c_str();
}

void jce_rml_elem_set_value(JceRmlBackend *b, uint32_t elem_idx,
                            const char *value)
{
    if (!b || !value || elem_idx >= (uint32_t)b->elements.size()) return;
    Rml::Element *elem = b->elements[elem_idx];
    Rml::ElementFormControl *control = dynamic_cast<Rml::ElementFormControl *>(elem);
    if (control) control->SetValue(value);
}

const char *jce_rml_elem_get_attribute(JceRmlBackend *b, uint32_t elem_idx,
                                       const char *attr)
{
    if (!b || !attr || elem_idx >= (uint32_t)b->elements.size()) return "";
    Rml::Element *elem = b->elements[elem_idx];
    if (!elem) return "";

    Rml::Variant *var = elem->GetAttribute(attr);
    if (!var) return "";

    b->temp_text = var->Get<Rml::String>();
    return b->temp_text.c_str();
}

void jce_rml_elem_set_attribute(JceRmlBackend *b, uint32_t elem_idx,
                                const char *attr, const char *val)
{
    if (!b || !attr || !val || elem_idx >= (uint32_t)b->elements.size()) return;
    Rml::Element *elem = b->elements[elem_idx];
    if (elem) elem->SetAttribute(attr, Rml::String(val));
}

void jce_rml_elem_remove_attribute(JceRmlBackend *b, uint32_t elem_idx,
                                   const char *attr)
{
    if (!b || !attr || elem_idx >= (uint32_t)b->elements.size()) return;
    Rml::Element *elem = b->elements[elem_idx];
    if (elem) elem->RemoveAttribute(attr);
}

void jce_rml_elem_set_inner_rml(JceRmlBackend *b, uint32_t elem_idx,
                                const char *rml)
{
    if (!b || !rml || elem_idx >= (uint32_t)b->elements.size()) return;
    Rml::Element *elem = b->elements[elem_idx];
    if (elem) elem->SetInnerRML(rml);
}

bool jce_rml_elem_get_bounds(JceRmlBackend *b, uint32_t elem_idx,
                             float *x, float *y, float *w, float *h)
{
    if (!b || elem_idx >= (uint32_t)b->elements.size()) return false;
    Rml::Element *elem = b->elements[elem_idx];
    if (!elem || !x || !y || !w || !h) return false;

    *x = elem->GetAbsoluteLeft();
    *y = elem->GetAbsoluteTop();
    *w = elem->GetOffsetWidth();
    *h = elem->GetOffsetHeight();
    return true;
}

uint32_t jce_rml_doc_get_body(JceRmlBackend *b, uint32_t doc_idx)
{
    if (!b || doc_idx >= (uint32_t)b->documents.size()) return UINT32_MAX;
    Rml::ElementDocument *doc = b->documents[doc_idx];
    if (!doc) return UINT32_MAX;

    Rml::Element *body = nullptr;
    const int child_count = doc->GetNumChildren(true);
    for (int i = 0; i < child_count; ++i) {
        Rml::Element *child = doc->GetChild(i);
        if (child && child->GetTagName() == "body") {
            body = child;
            break;
        }
    }
    if (!body)
        body = doc;

    uint32_t idx = (uint32_t)b->elements.size();
    b->elements.push_back(body);
    return idx;
}

/* ================================================================== */
/* Events                                                              */
/* ================================================================== */

void jce_rml_elem_on(JceRmlBackend *b, uint32_t elem_idx,
                     const char *evt, jce_rml_event_fn fn, void *ud)
{
    if (!b || !evt || !fn || elem_idx >= (uint32_t)b->elements.size()) return;
    Rml::Element *elem = b->elements[elem_idx];
    if (!elem) return;

    auto *adapter = new (std::nothrow) JceRmlEventAdapter(elem_idx, fn, ud);
    if (!adapter) {
        LOG_ERROR(LOG_TAG, "failed to allocate event adapter");
        return;
    }

    b->event_adapters.push_back(adapter);
    elem->AddEventListener(evt, adapter);
}

/* ================================================================== */
/* Font loading                                                        */
/* ================================================================== */

bool jce_rml_load_font(JceRmlBackend *b, const char *pak_path)
{
    if (!b || !pak_path || !b->pak) return false;

    const JcePakAsset *asset = jce_pak_find(b->pak, pak_path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "font not found in PAK: %s", pak_path);
        return false;
    }

    void *data = JCE_MALLOC((size_t)asset->original_size);
    if (!data) return false;

    size_t sz = jce_pak_decompress(asset, data, (size_t)asset->original_size);
    if (sz == 0) {
        JCE_FREE(data);
        return false;
    }

    /* Extract family name from filename (e.g. "fonts/Caveat.ttf" → "Caveat"). */
    std::string family;
    {
        std::string path_str(pak_path);
        size_t slash = path_str.find_last_of("/\\");
        size_t dot   = path_str.find_last_of('.');
        if (slash != std::string::npos)
            family = path_str.substr(slash + 1,
                dot != std::string::npos ? dot - slash - 1 : std::string::npos);
        else
            family = path_str.substr(0,
                dot != std::string::npos ? dot : std::string::npos);
    }

    bool ok = Rml::LoadFontFace(
        reinterpret_cast<const Rml::byte *>(data), (int)sz,
        family, Rml::Style::FontStyle::Normal,
        Rml::Style::FontWeight::Normal, false);

    if (ok) {
        /* RmlUI keeps a pointer to the font data — must stay alive. */
        b->font_data_buffers.push_back(data);
        LOG_SUCCESS(LOG_TAG, "font loaded: %s (family: %s)", pak_path, family.c_str());
    } else {
        JCE_FREE(data);
        LOG_ERROR(LOG_TAG, "Rml::LoadFontFace failed: %s", pak_path);
    }

    return ok;
}

/* ================================================================== */
/* Per-frame: input                                                    */
/* ================================================================== */

void jce_rml_process_pointer_input(JceRmlBackend *b, const JceInput *input)
{
    if (!b || !b->context || !input) return;

    /* Mouse position. */
    float mx = 0, my = 0;
    jce_input_mouse_pos(input, &mx, &my);
    b->context->ProcessMouseMove((int)mx, (int)my, 0);

    /* Mouse buttons (left=0, right=1, middle=2). */
    for (int btn = 0; btn < 3; btn++) {
        if (jce_input_mouse_button_pressed(input, btn + 1))
            b->context->ProcessMouseButtonDown(btn, 0);
        if (jce_input_mouse_button_released(input, btn + 1))
            b->context->ProcessMouseButtonUp(btn, 0);
    }

    /* Mouse wheel. */
    float wheel = jce_input_mouse_wheel(input);
    if (wheel != 0.0f)
        b->context->ProcessMouseWheel(-wheel, 0);
}

void jce_rml_process_input(JceRmlBackend *b, const JceInput *input)
{
    if (!b || !b->context || !input) return;

    jce_rml_process_pointer_input(b, input);

    /* Key presses for UI navigation. */
    struct KeyMap { JceKey jce; Rml::Input::KeyIdentifier rml; };
    static const KeyMap kmap[] = {
        { JCE_KEY_TAB,       Rml::Input::KI_TAB    },
        { JCE_KEY_RETURN,    Rml::Input::KI_RETURN  },
        { JCE_KEY_ESCAPE,    Rml::Input::KI_ESCAPE  },
        { JCE_KEY_BACKSPACE, Rml::Input::KI_BACK    },
        { JCE_KEY_DELETE,    Rml::Input::KI_DELETE   },
        { JCE_KEY_LEFT,      Rml::Input::KI_LEFT    },
        { JCE_KEY_RIGHT,     Rml::Input::KI_RIGHT   },
        { JCE_KEY_UP,        Rml::Input::KI_UP      },
        { JCE_KEY_DOWN,      Rml::Input::KI_DOWN    },
        { JCE_KEY_HOME,      Rml::Input::KI_HOME    },
        { JCE_KEY_END,       Rml::Input::KI_END     },
    };
    for (const auto &km : kmap) {
        if (jce_input_key_pressed(input, km.jce))
            b->context->ProcessKeyDown(km.rml, 0);
        if (jce_input_key_released(input, km.jce))
            b->context->ProcessKeyUp(km.rml, 0);
    }
}

/* ================================================================== */
/* Per-frame: update, render, resize                                   */
/* ================================================================== */

void jce_rml_update(JceRmlBackend *b, float dt)
{
    if (!b || !b->context) return;
    b->sys_interface->AccumulateTime((double)dt);
    b->context->Update();
}

void jce_rml_render(JceRmlBackend *b)
{
    if (!b || !b->context) return;
    b->context->Render();
}

void jce_rml_resize(JceRmlBackend *b, uint32_t w, uint32_t h)
{
    if (!b || !b->context) return;
    b->width  = w;
    b->height = h;
    b->context->SetDimensions(Rml::Vector2i((int)w, (int)h));
    b->context->SetDensityIndependentPixelRatio(compute_dp_ratio(h));
}

void jce_rml_set_dp_ratio(JceRmlBackend *b, float dp_ratio)
{
    if (!b || !b->context || dp_ratio <= 0.0f) return;
    b->context->SetDensityIndependentPixelRatio(dp_ratio);
    LOG_DEBUG(LOG_TAG, "RmlUi dp-ratio set to %.2f", (double)dp_ratio);
}
