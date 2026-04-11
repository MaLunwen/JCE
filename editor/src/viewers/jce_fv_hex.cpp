/*
 * jce_fv_hex.cpp  Binary hex dump and Scene sub-viewers.
 *
 * Scene viewer delegates to fv_render_code() for its text content.
 */

#include "jce_fv_common.h"

/* ══════════════════════════════════════════════════════════════════════
 *  SUB-VIEWER: SCENE
 * ══════════════════════════════════════════════════════════════════════ */

void fv_render_scene(FvTab *tab)
{
    if (ImGui::Button(jce_editor_i18n("viewer.openExternal"))) {
#ifdef _WIN32
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "explorer /select,\"%s\"", tab->path);
        system(cmd);
#endif
    }
    ImGui::SameLine();
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
        "%s  |  %.1f KB", jce_editor_i18n("viewer.scene"), (double)tab->file_size / 1024.0);
    ImGui::Separator();

    ImGui::Spacing();
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s", jce_editor_i18n("viewer.sceneFile"));
    ImGui::Separator();
    ImGui::Spacing();
    fv_render_code(tab);
}

/* ══════════════════════════════════════════════════════════════════════
 *  SUB-VIEWER: BINARY (HEX DUMP)
 * ══════════════════════════════════════════════════════════════════════ */

void fv_render_hex(FvTab *tab)
{
    if (ImGui::Button(jce_editor_i18n("viewer.openExternal"))) {
#ifdef _WIN32
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "explorer /select,\"%s\"", tab->path);
        system(cmd);
#endif
    }
    ImGui::SameLine();
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
        "%s  |  %.1f KB", jce_editor_i18n("viewer.binary"), (double)tab->file_size / 1024.0);
    ImGui::Separator();

    ImGui::BeginChild("##hex", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    int total = (tab->content_len > 4096) ? 4096 : tab->content_len;
    const unsigned char *d = (const unsigned char *)tab->content;

    for (int offset = 0; offset < total; offset += 16) {
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%08X", offset);
        ImGui::SameLine();

        char hex_buf[64];
        int pos = 0;
        for (int j = 0; j < 16; j++) {
            if (offset + j < total)
                pos += snprintf(hex_buf + pos, sizeof(hex_buf) - (size_t)pos,
                                "%02X ", d[offset + j]);
            else
                pos += snprintf(hex_buf + pos, sizeof(hex_buf) - (size_t)pos,
                                "   ");
            if (j == 7)
                pos += snprintf(hex_buf + pos, sizeof(hex_buf) - (size_t)pos,
                                " ");
        }
        ImGui::Text("%s", hex_buf);
        ImGui::SameLine();

        char ascii[17];
        for (int j = 0; j < 16; j++) {
            if (offset + j < total) {
                unsigned char c = d[offset + j];
                ascii[j] = (c >= 32 && c < 127) ? (char)c : '.';
            } else {
                ascii[j] = ' ';
            }
        }
        ascii[16] = '\0';
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", ascii);
    }

    if (tab->content_len > 4096) {
        ImGui::Spacing();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
            "... (%d %s)", tab->content_len - 4096, jce_editor_i18n("viewer.moreBytes"));
    }

    ImGui::EndChild();
}
