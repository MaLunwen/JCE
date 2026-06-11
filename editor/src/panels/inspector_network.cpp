/*
 * inspector_network.cpp  Inspector drawers for Network ECS components (P4-C.1).
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
