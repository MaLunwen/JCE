/*
 * jce_ui_rmlui.cpp  RmlUi backend implementation for the JCE UI system.
 *
 * Implements the extern "C" bridge declared in jce_ui_backend.h by
 * delegating to RmlUi's Core API.
 *
 * Render interface methods are stubs for now — the actual bgfx geometry
 * pipeline will be wired in a follow-up.
 */

#include "jce_ui_backend.h"

#include <RmlUi/Core.h>
#include <jce/core/jce_log.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

#define LOG_TAG "ui.rml"

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
/* Render interface  (stubs — bgfx integration is a follow-up)        */
/* ================================================================== */

class JceRmlRenderInterface : public Rml::RenderInterface {
public:
    /* Pure-virtual in RmlUi 5.x — must be overridden. */
    void RenderGeometry(Rml::Vertex *vertices, int num_vertices,
                        int *indices, int num_indices,
                        Rml::TextureHandle texture,
                        const Rml::Vector2f &translation) override
    {
        /* TODO: Submit immediate geometry through bgfx. */
        LOG_TRACE(LOG_TAG, "RenderGeometry: %d verts, %d indices (stub)",
                  num_vertices, num_indices);
        (void)vertices;
        (void)num_vertices;
        (void)indices;
        (void)num_indices;
        (void)texture;
        (void)translation;
    }

    /* Optional — compile static geometry for faster repeat rendering. */
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Vertex *vertices,
                                                int num_vertices,
                                                int *indices,
                                                int num_indices,
                                                Rml::TextureHandle texture) override
    {
        LOG_TRACE(LOG_TAG, "CompileGeometry: %d verts, %d indices (stub)",
                  num_vertices, num_indices);
        (void)vertices;
        (void)num_vertices;
        (void)indices;
        (void)num_indices;
        (void)texture;
        return Rml::CompiledGeometryHandle(0);
    }

    /* Optional — render previously compiled geometry. */
    void RenderCompiledGeometry(Rml::CompiledGeometryHandle geometry,
                                const Rml::Vector2f &translation) override
    {
        LOG_TRACE(LOG_TAG, "RenderCompiledGeometry handle=%llu (stub)",
                  (unsigned long long)geometry);
        (void)geometry;
        (void)translation;
    }

    /* Optional — free compiled geometry resources. */
    void ReleaseCompiledGeometry(Rml::CompiledGeometryHandle geometry) override
    {
        LOG_TRACE(LOG_TAG, "ReleaseCompiledGeometry handle=%llu (stub)",
                  (unsigned long long)geometry);
        (void)geometry;
    }

    void EnableScissorRegion(bool enable) override
    {
        /* TODO: Set bgfx scissor state. */
        LOG_TRACE(LOG_TAG, "EnableScissorRegion(%s) (stub)",
                  enable ? "true" : "false");
        (void)enable;
    }

    void SetScissorRegion(int x, int y, int width, int height) override
    {
        /* TODO: Apply scissor rectangle via bgfx. */
        LOG_TRACE(LOG_TAG, "SetScissorRegion(%d,%d,%d,%d) (stub)",
                  x, y, width, height);
        (void)x;
        (void)y;
        (void)width;
        (void)height;
    }
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
    Rml::Context           *context          = nullptr;

    std::vector<Rml::ElementDocument *> documents;
    std::vector<Rml::Element *>         elements;

    /* Keeps event adapters alive for the lifetime of the backend. */
    std::vector<JceRmlEventAdapter *>   event_adapters;

    /* Temporary storage so elem_get_text can return a stable c_str(). */
    std::string                         temp_text;

    uint32_t width  = 0;
    uint32_t height = 0;
};

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JceRmlBackend *jce_rml_create(uint32_t width, uint32_t height)
{
    auto *b = new (std::nothrow) JceRmlBackend();
    if (!b) {
        LOG_ERROR(LOG_TAG, "failed to allocate JceRmlBackend");
        return nullptr;
    }

    b->width  = width;
    b->height = height;

    b->sys_interface    = new (std::nothrow) JceRmlSystemInterface();
    b->render_interface = new (std::nothrow) JceRmlRenderInterface();
    if (!b->sys_interface || !b->render_interface) {
        LOG_ERROR(LOG_TAG, "failed to allocate RmlUi interfaces");
        delete b->render_interface;
        delete b->sys_interface;
        delete b;
        return nullptr;
    }

    Rml::SetSystemInterface(b->sys_interface);
    Rml::SetRenderInterface(b->render_interface);

    if (!Rml::Initialise()) {
        LOG_ERROR(LOG_TAG, "Rml::Initialise() failed");
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
        delete b->render_interface;
        delete b->sys_interface;
        delete b;
        return nullptr;
    }

    LOG_SUCCESS(LOG_TAG, "RmlUi backend created (%ux%u)", width, height);
    return b;
}

void jce_rml_destroy(JceRmlBackend *b)
{
    if (!b) return;

    for (auto *adapter : b->event_adapters) {
        delete adapter;
    }
    b->event_adapters.clear();

    Rml::Shutdown();

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
    (void)name; /* RmlUi does not use a separate name for memory docs. */

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
/* Per-frame                                                           */
/* ================================================================== */

void jce_rml_process_input(JceRmlBackend *b, const void *input)
{
    /* TODO: Wire SDL/platform input events to RmlUi key/mouse events.
     * This requires mapping JceInput fields to Rml::Context::Process*
     * methods (ProcessKeyDown, ProcessMouseMove, ProcessMouseButtonDown,
     * ProcessTextInput, etc.).  Deferred to the input-integration pass. */
    (void)b;
    (void)input;
}

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
}

void jce_rml_set_dp_ratio(JceRmlBackend *b, float dp_ratio)
{
    if (!b || !b->context || dp_ratio <= 0.0f) return;
    b->context->SetDensityIndependentPixelRatio(dp_ratio);
    LOG_DEBUG(LOG_TAG, "RmlUi dp-ratio set to %.2f", (double)dp_ratio);
}
