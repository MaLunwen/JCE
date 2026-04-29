/*
 * jce_reflect.cpp  Registry + ImGui drawer for JceReflect.
 */

#include "jce_reflect.h"

#include "jce_editor_i18n.h"
#include "jce_editor_state.h"

#include <jce/tools/jce_imgui.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
bool g_reflect_batch_open = false;
void track_undo(void)
{
    if (ImGui::IsItemActivated() && !g_reflect_batch_open) {
        jce_state_begin_batch_edit();
        g_reflect_batch_open = true;
    }
    if (ImGui::IsItemDeactivated() && g_reflect_batch_open) {
        jce_state_end_batch_edit();
        g_reflect_batch_open = false;
    }
}
void track_undo_bool(bool *value, bool changed)
{
    if (!changed) return;
    bool now = *value;
    *value = !now;
    jce_state_begin_batch_edit();
    *value = now;
    jce_state_end_batch_edit();
}
} /* namespace */

namespace {

struct Registry {
    std::vector<const JceReflectType *> types;
};

Registry &reg(void)
{
    static Registry r;
    return r;
}

float *as_float(void *base, const JceReflectField *f)
{
    return reinterpret_cast<float *>(static_cast<char *>(base) + f->offset);
}
int *as_int(void *base, const JceReflectField *f)
{
    return reinterpret_cast<int *>(static_cast<char *>(base) + f->offset);
}
bool *as_bool(void *base, const JceReflectField *f)
{
    return reinterpret_cast<bool *>(static_cast<char *>(base) + f->offset);
}
char *as_char(void *base, const JceReflectField *f)
{
    return static_cast<char *>(base) + f->offset;
}

bool draw_field(const JceReflectField *f, void *base, const void *defaults_base)
{
    bool changed = false;
    /* Try i18n first: keys are formed as "reflect.<TypeName>.<field_name>"
       so localizers can translate any reflected struct without us
       changing the JCE_FIELD macros. Fall back to the C label / name. */
    const char *raw_label = f->label ? f->label : f->name;
    char i18n_key[160];
    snprintf(i18n_key, sizeof(i18n_key), "reflect.field.%s", f->name);
    const char *label = jce_editor_i18n_or(i18n_key, raw_label);
    ImGui::PushID(f->name);
    switch (f->type) {
    case JCE_FT_BOOL:
        changed = ImGui::Checkbox(label, as_bool(base, f));
        track_undo_bool(as_bool(base, f), changed);
        break;
    case JCE_FT_INT: {
        int *p = as_int(base, f);
        if (f->vmax > f->vmin)
            changed = ImGui::SliderInt(label, p, (int)f->vmin, (int)f->vmax);
        else
            changed = ImGui::DragInt(label, p,
                                     f->vstep > 0 ? f->vstep : 1.0f);
        track_undo();
    } break;
    case JCE_FT_FLOAT: {
        float *p = as_float(base, f);
        float step = f->vstep > 0.0f ? f->vstep : 0.01f;
        if (f->vmax > f->vmin)
            changed = ImGui::SliderFloat(label, p, f->vmin, f->vmax);
        else
            changed = ImGui::DragFloat(label, p, step);
        track_undo();
    } break;
    case JCE_FT_VEC2: {
        float *p = as_float(base, f);
        changed = ImGui::DragFloat2(label, p,
                                    f->vstep > 0.0f ? f->vstep : 0.01f);
        track_undo();
    } break;
    case JCE_FT_VEC3: {
        float *p = as_float(base, f);
        changed = ImGui::DragFloat3(label, p,
                                    f->vstep > 0.0f ? f->vstep : 0.01f);
        track_undo();
    } break;
    case JCE_FT_VEC4: {
        float *p = as_float(base, f);
        changed = ImGui::DragFloat4(label, p,
                                    f->vstep > 0.0f ? f->vstep : 0.01f);
        track_undo();
    } break;
    case JCE_FT_QUAT: {
        /* Display Euler XYZ (deg).  Convert without external math
         * dependency to keep the drawer self-contained. */
        float *q = as_float(base, f);                /* x,y,z,w */
        float sx = 2.0f * (q[3] * q[0] + q[1] * q[2]);
        float cx = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
        float ex = std::atan2(sx, cx);
        float sy = 2.0f * (q[3] * q[1] - q[2] * q[0]);
        if (sy >  1.0f) sy =  1.0f;
        if (sy < -1.0f) sy = -1.0f;
        float ey = std::asin(sy);
        float sz = 2.0f * (q[3] * q[2] + q[0] * q[1]);
        float cz = 1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]);
        float ez = std::atan2(sz, cz);
        const float r2d = 57.29577951308232f;
        float deg[3] = { ex * r2d, ey * r2d, ez * r2d };
        if (ImGui::DragFloat3(label, deg, 0.5f)) {
            const float d2r = 0.017453292519943295f;
            float hx = deg[0] * 0.5f * d2r;
            float hy = deg[1] * 0.5f * d2r;
            float hz = deg[2] * 0.5f * d2r;
            float cx2 = std::cos(hx), sx2 = std::sin(hx);
            float cy2 = std::cos(hy), sy2 = std::sin(hy);
            float cz2 = std::cos(hz), sz2 = std::sin(hz);
            q[0] = sx2 * cy2 * cz2 - cx2 * sy2 * sz2;
            q[1] = cx2 * sy2 * cz2 + sx2 * cy2 * sz2;
            q[2] = cx2 * cy2 * sz2 - sx2 * sy2 * cz2;
            q[3] = cx2 * cy2 * cz2 + sx2 * sy2 * sz2;
            changed = true;
        }
        track_undo();
    } break;
    case JCE_FT_COLOR3:
        changed = ImGui::ColorEdit3(label, as_float(base, f));
        track_undo();
        break;
    case JCE_FT_COLOR4:
        changed = ImGui::ColorEdit4(label, as_float(base, f));
        track_undo();
        break;
    case JCE_FT_STRING: {
        char *p = as_char(base, f);
        changed = ImGui::InputText(label, p, f->size);
        track_undo();
    } break;
    case JCE_FT_ENUM_INT: {
        int *p = as_int(base, f);
        const char *preview =
            (*p >= 0 && *p < f->enum_count && f->enum_labels)
                ? f->enum_labels[*p] : "?";
        if (ImGui::BeginCombo(label, preview)) {
            for (int i = 0; i < f->enum_count; ++i) {
                bool sel = (*p == i);
                if (ImGui::Selectable(f->enum_labels[i], sel)) {
                    *p = i;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
    } break;
    case JCE_FT_ASSET_REF: {
        char *p = as_char(base, f);
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 60);
        changed = ImGui::InputText(label, p, f->size);
        ImGui::PopItemWidth();
        track_undo();
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload *pl = ImGui::AcceptDragDropPayload("JCE_ASSET_PATH")) {
                const char *src = (const char *)pl->Data;
                if (src && pl->DataSize > 0) {
                    size_t n = (size_t)pl->DataSize;
                    if (n >= f->size) n = f->size - 1;
                    std::memcpy(p, src, n);
                    p[n] = '\0';
                    changed = true;
                    jce_state_begin_batch_edit();
                    jce_state_end_batch_edit();
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            p[0] = '\0';
            changed = true;
            jce_state_begin_batch_edit();
            jce_state_end_batch_edit();
        }
        if (f->asset_kind && f->asset_kind[0]) {
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", f->asset_kind);
        }
    } break;
    case JCE_FT_STRUCT_NESTED: {
        const JceReflectType *nt = f->nested_type_name
            ? jce_reflect_find(f->nested_type_name) : nullptr;
        if (!nt) {
            ImGui::TextDisabled("%s: nested type '%s' not registered",
                                label, f->nested_type_name ? f->nested_type_name : "?");
        } else {
            if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
                void *child = static_cast<char *>(base) + f->offset;
                if (jce_reflect_draw(nt, child)) changed = true;
                ImGui::TreePop();
            }
        }
    } break;
    case JCE_FT_ARRAY: {
        char *arr_base   = static_cast<char *>(base) + f->offset;
        int  *count_p    = reinterpret_cast<int *>(static_cast<char *>(base) + f->count_offset);
        int   count      = *count_p;
        if (count < 0) count = 0;
        if (count > f->max_count) count = f->max_count;

        char hdr[160];
        std::snprintf(hdr, sizeof(hdr), "%s [%d/%d]",
                      label, count, f->max_count);
        if (ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_DefaultOpen)) {
            int  remove_idx = -1;
            for (int i = 0; i < count; ++i) {
                ImGui::PushID(i);
                ImGui::Text("[%d]", i);
                ImGui::SameLine();
                void *elem_ptr = arr_base + (size_t)i * f->element_size;
                JceReflectField sub = *f;
                sub.type   = f->element_type;
                sub.offset = 0;
                sub.size   = f->element_size;
                sub.element_type = JCE_FT_NONE;
                sub.element_size = 0;
                sub.max_count    = 0;
                if (draw_field(&sub, elem_ptr, nullptr)) changed = true;
                ImGui::SameLine();
                if (ImGui::SmallButton("-")) remove_idx = i;
                ImGui::PopID();
            }
            if (remove_idx >= 0 && count > 0) {
                if (remove_idx < count - 1) {
                    std::memmove(arr_base + (size_t)remove_idx * f->element_size,
                                 arr_base + (size_t)(remove_idx + 1) * f->element_size,
                                 (size_t)(count - remove_idx - 1) * f->element_size);
                }
                std::memset(arr_base + (size_t)(count - 1) * f->element_size,
                            0, f->element_size);
                *count_p = count - 1;
                changed = true;
                jce_state_begin_batch_edit();
                jce_state_end_batch_edit();
            }
            if (count < f->max_count) {
                if (ImGui::SmallButton(jce_editor_i18n("reflect.button.add"))) {
                    std::memset(arr_base + (size_t)count * f->element_size,
                                0, f->element_size);
                    *count_p = count + 1;
                    changed = true;
                    jce_state_begin_batch_edit();
                    jce_state_end_batch_edit();
                }
            } else {
                ImGui::TextDisabled("%s", jce_editor_i18n("reflect.label.atCapacity"));
            }
            ImGui::TreePop();
        }
    } break;
    default:
        ImGui::TextDisabled("%s", jce_editor_i18n("reflect.label.unsupportedField"));
        break;
    }

    /* Right-click "Reset to Default" — Unity-style. Only when the type
     * registered a defaults blob and the field has a known size. */
    if (defaults_base && f->size > 0 && f->type != JCE_FT_STRUCT_NESTED &&
        f->type != JCE_FT_ARRAY) {
        if (ImGui::BeginPopupContextItem("##reset_field_ctx")) {
            void       *dst = static_cast<char *>(base) + f->offset;
            const void *src = static_cast<const char *>(defaults_base) + f->offset;
            bool already_default = (std::memcmp(dst, src, f->size) == 0);
            if (ImGui::MenuItem(jce_editor_i18n("reflect.menu.resetToDefault"), nullptr, false, !already_default)) {
                jce_state_begin_batch_edit();
                std::memcpy(dst, src, f->size);
                jce_state_end_batch_edit();
                changed = true;
            }
            ImGui::EndPopup();
        }
    }

    ImGui::PopID();
    return changed;
}

} /* anonymous namespace */

extern "C" void jce_reflect_register(const JceReflectType *type)
{
    if (!type) return;
    auto &v = reg().types;
    for (const auto *t : v) {
        if (t == type || (t->display_name && type->display_name &&
                          std::strcmp(t->display_name, type->display_name) == 0))
            return;
    }
    v.push_back(type);
}

extern "C" const JceReflectType *jce_reflect_find(const char *display_name)
{
    if (!display_name) return nullptr;
    for (const auto *t : reg().types) {
        if (t->display_name && std::strcmp(t->display_name, display_name) == 0)
            return t;
    }
    return nullptr;
}

extern "C" int jce_reflect_count(void)
{
    return (int)reg().types.size();
}

extern "C" const JceReflectType *jce_reflect_at(int idx)
{
    auto &v = reg().types;
    if (idx < 0 || idx >= (int)v.size()) return nullptr;
    return v[(size_t)idx];
}

bool jce_reflect_draw(const JceReflectType *type, void *instance)
{
    if (!type || !instance) return false;
    bool any_changed = false;
    for (int i = 0; i < type->field_count; ++i) {
        if (draw_field(&type->fields[i], instance, type->default_instance))
            any_changed = true;
    }
    return any_changed;
}
