/*
 * jce_panel_inspector_network.cpp  Inspector drawers for Network ECS components (P4-C.1).
 */

#include "jce_panel_inspector_common.h"

void draw_comp_network_object(JceNetworkObjectComponent *c)
{
    if (!c) return;

    ImGui::PushItemWidth(-1);

    /* net_id is server-assigned at spawn; show it read-only so designers
     * can correlate live sessions, never author it. */
    ImGui::TextDisabled("%s: %u%s",
                        jce_editor_i18n("inspector.net_object.netId"),
                        c->net_id,
                        c->net_id == 0
                            ? jce_editor_i18n("inspector.net_object.netIdUnspawned")
                            : "");

    int owner = (int)c->owner;
    if (ImGui::DragInt(jce_editor_i18n("inspector.net_object.owner"),
                       &owner, 1.0f, 0, 65535)) {
        jce_state_begin_batch_edit();
        c->owner = (uint16_t)owner;
        jce_state_end_batch_edit();
    }
    insp_track_edit();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("inspector.net_object.owner.tip"));

    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.net_object.gatekeeperNote"));

    ImGui::PopItemWidth();
}

void draw_comp_net_transform(JceNetTransformComponent *c)
{
    if (!c) return;

    ImGui::PushItemWidth(-1);

    int sync_hz = c->sync_rate_hz;
    if (ImGui::DragInt(jce_editor_i18n("inspector.net_transform.sync_rate"),
                       &sync_hz, 1.0f, 1, 128)) {
        jce_state_begin_batch_edit();
        c->sync_rate_hz = (uint8_t)sync_hz;
        jce_state_end_batch_edit();
    }
    insp_track_edit();

    int interp = c->interp_ms;
    if (ImGui::DragInt(jce_editor_i18n("inspector.net_transform.interp"),
                       &interp, 1.0f, 0, 1000)) {
        jce_state_begin_batch_edit();
        c->interp_ms = (uint16_t)interp;
        jce_state_end_batch_edit();
    }
    insp_track_edit();

    ImGui::DragFloat(jce_editor_i18n("inspector.net_transform.tolerance"),
                     &c->tolerance, 0.01f, 0.0f, 100.0f);
    insp_track_edit();

    const char *auth_items[] = {
        jce_editor_i18n("inspector.net_transform.authority.server"),
        jce_editor_i18n("inspector.net_transform.authority.owner"),
    };
    int auth = (int)c->authority_mode;
    if (ImGui::Combo(jce_editor_i18n("inspector.net_transform.authority"),
                     &auth, auth_items, 2)) {
        jce_state_begin_batch_edit();
        c->authority_mode = (uint8_t)auth;
        jce_state_end_batch_edit();
    }

    ImGui::PopItemWidth();
}

void draw_comp_net_animator(JceNetAnimatorComponent *c)
{
    insp_unwired_badge();
    if (!c) return;

    ImGui::PushItemWidth(-1);

    int sync_hz = c->sync_rate_hz;
    if (ImGui::DragInt(jce_editor_i18n("inspector.net_animator.sync_rate"),
                       &sync_hz, 1.0f, 1, 128)) {
        jce_state_begin_batch_edit();
        c->sync_rate_hz = (uint8_t)sync_hz;
        jce_state_end_batch_edit();
    }
    insp_track_edit();

    int interp = c->interp_ms;
    if (ImGui::DragInt(jce_editor_i18n("inspector.net_animator.interp"),
                       &interp, 1.0f, 0, 1000)) {
        jce_state_begin_batch_edit();
        c->interp_ms = (uint16_t)interp;
        jce_state_end_batch_edit();
    }
    insp_track_edit();

    const char *auth_items[] = {
        jce_editor_i18n("inspector.net_animator.authority.server"),
        jce_editor_i18n("inspector.net_animator.authority.owner"),
    };
    int auth = (int)c->authority_mode;
    if (ImGui::Combo(jce_editor_i18n("inspector.net_animator.authority"),
                     &auth, auth_items, 2)) {
        jce_state_begin_batch_edit();
        c->authority_mode = (uint8_t)auth;
        jce_state_end_batch_edit();
    }

    ImGui::PopItemWidth();
}

void draw_comp_network_variable(JceNetworkVariableComponent *c)
{
    if (!c) return;

    ImGui::PushItemWidth(-1);

    /* Variable name (designer label for the replicated field). */
    {
        char buf[sizeof(c->var_name)];
        snprintf(buf, sizeof(buf), "%s", c->var_name);
        if (ImGui::InputText(jce_editor_i18n("inspector.net_var.name"),
                             buf, sizeof(buf))) {
            jce_state_begin_batch_edit();
            snprintf(c->var_name, sizeof(c->var_name), "%s", buf);
            jce_state_end_batch_edit();
        }
        insp_track_edit();
    }

    /* Type: F32 / I32 / Bool — matches the engine NetVar scalar types
     * (Bool replicates over the i32 NetVar as 0/1). */
    const char *type_items[] = {
        jce_editor_i18n("inspector.net_var.type.f32"),
        jce_editor_i18n("inspector.net_var.type.i32"),
        jce_editor_i18n("inspector.net_var.type.bool"),
    };
    int type = (int)c->var_type;
    if (type < 0) type = 0;
    if (type > 2) type = 2;
    if (ImGui::Combo(jce_editor_i18n("inspector.net_var.type"),
                     &type, type_items, 3)) {
        jce_state_begin_batch_edit();
        c->var_type = (uint8_t)type;
        jce_state_end_batch_edit();
    }
    insp_track_edit();

    /* Authority: Server / Client / Owner — matches the net authority model. */
    const char *auth_items[] = {
        jce_editor_i18n("inspector.net_var.authority.server"),
        jce_editor_i18n("inspector.net_var.authority.client"),
        jce_editor_i18n("inspector.net_var.authority.owner"),
    };
    int auth = (int)c->authority;
    if (auth < 0) auth = 0;
    if (auth > 2) auth = 2;
    if (ImGui::Combo(jce_editor_i18n("inspector.net_var.authority"),
                     &auth, auth_items, 3)) {
        jce_state_begin_batch_edit();
        c->authority = (uint8_t)auth;
        jce_state_end_batch_edit();
    }
    insp_track_edit();

    /* Initial value — the editor exposes one float field that covers all
     * three types (I32 rounds, Bool reads non-zero) to keep the POD
     * serialiser-friendly, mirroring the engine union choice. */
    if (type == 2 /* Bool */) {
        bool b = (c->initial_value != 0.0f);
        if (ImGui::Checkbox(jce_editor_i18n("inspector.net_var.initial"), &b)) {
            jce_state_begin_batch_edit();
            c->initial_value = b ? 1.0f : 0.0f;
            jce_state_end_batch_edit();
        }
        insp_track_edit();
    } else if (type == 1 /* I32 */) {
        int iv = (int)c->initial_value;
        if (ImGui::DragInt(jce_editor_i18n("inspector.net_var.initial"),
                           &iv, 1.0f)) {
            jce_state_begin_batch_edit();
            c->initial_value = (float)iv;
            jce_state_end_batch_edit();
        }
        insp_track_edit();
    } else { /* F32 */
        ImGui::DragFloat(jce_editor_i18n("inspector.net_var.initial"),
                         &c->initial_value, 0.01f);
        insp_track_edit();
    }

    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.net_var.note"));

    ImGui::PopItemWidth();
}

void draw_comp_net_rigidbody(JceNetRigidbodyComponent *c)
{
    insp_unwired_badge();
    if (!c) return;

    ImGui::PushItemWidth(-1);

    int sync_hz = c->sync_rate_hz;
    if (ImGui::DragInt(jce_editor_i18n("inspector.net_rigidbody.sync_rate"),
                       &sync_hz, 1.0f, 1, 128)) {
        jce_state_begin_batch_edit();
        c->sync_rate_hz = (uint8_t)sync_hz;
        jce_state_end_batch_edit();
    }
    insp_track_edit();

    int interp = c->interp_ms;
    if (ImGui::DragInt(jce_editor_i18n("inspector.net_rigidbody.interp"),
                       &interp, 1.0f, 0, 1000)) {
        jce_state_begin_batch_edit();
        c->interp_ms = (uint16_t)interp;
        jce_state_end_batch_edit();
    }
    insp_track_edit();

    ImGui::DragFloat(jce_editor_i18n("inspector.net_rigidbody.tolerance"),
                     &c->tolerance, 0.01f, 0.0f, 100.0f);
    insp_track_edit();

    const char *auth_items[] = {
        jce_editor_i18n("inspector.net_rigidbody.authority.server"),
        jce_editor_i18n("inspector.net_rigidbody.authority.owner"),
    };
    int auth = (int)c->authority_mode;
    if (ImGui::Combo(jce_editor_i18n("inspector.net_rigidbody.authority"),
                     &auth, auth_items, 2)) {
        jce_state_begin_batch_edit();
        c->authority_mode = (uint8_t)auth;
        jce_state_end_batch_edit();
    }

    ImGui::PopItemWidth();
}
