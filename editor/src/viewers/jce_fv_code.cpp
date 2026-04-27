/*
 * jce_fv_code.cpp  Text / Code sub-viewer with syntax highlighting,
 *                  find/replace, and edit mode.
 *
 * Reference: Java CodeViewerWindow.
 */

#include "io/jce_editor_file_util.h"
#include "jce_fv_common.h"
#include "core/jce_hotkeys.h"

#include <algorithm>
#include <string>
#include <vector>

#define LOG_TAG "fv_code"

/* ══════════════════════════════════════════════════════════════════════
 *  SYNTAX HIGHLIGHTING
 * ══════════════════════════════════════════════════════════════════════ */

static const ImVec4 SYN_COMMENT = ImVec4(0.42f, 0.60f, 0.33f, 1.0f);
static const ImVec4 SYN_STRING  = ImVec4(0.80f, 0.56f, 0.33f, 1.0f);
static const ImVec4 SYN_NUMBER  = ImVec4(0.70f, 0.85f, 0.55f, 1.0f);
static const ImVec4 SYN_PREPROC = ImVec4(0.60f, 0.50f, 0.80f, 1.0f);
static const ImVec4 SYN_DEFAULT = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);

static const char *s_c_keywords[] = {
    "auto","break","case","char","const","continue","default","do","double",
    "else","enum","extern","float","for","goto","if","int","long","register",
    "return","short","signed","sizeof","static","struct","switch","typedef",
    "union","unsigned","void","volatile","while",
    "bool","true","false","NULL","inline","restrict",
    "int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t","uint64_t",
    "size_t","ptrdiff_t","nullptr",
    "class","namespace","template","typename","virtual","override","public",
    "private","protected","new","delete","this","throw","try","catch","using",
    NULL
};

static const char *s_java_keywords[] = {
    "abstract","assert","boolean","break","byte","case","catch","char","class",
    "const","continue","default","do","double","else","enum","extends","final",
    "finally","float","for","goto","if","implements","import","instanceof",
    "int","interface","long","native","new","null","package","private",
    "protected","public","return","short","static","strictfp","super","switch",
    "synchronized","this","throw","throws","transient","try","void","volatile","while",
    "true","false","var","record","sealed","permits","yield",
    NULL
};

static const char *s_py_keywords[] = {
    "and","as","assert","async","await","break","class","continue","def","del",
    "elif","else","except","finally","for","from","global","if","import","in",
    "is","lambda","nonlocal","not","or","pass","raise","return","try","while",
    "with","yield","True","False","None","self",
    NULL
};

static bool fv_is_keyword(const char *s, int len, const char **kw_list)
{
    for (int i = 0; kw_list[i]; i++) {
        int klen = (int)strlen(kw_list[i]);
        if (klen == len && memcmp(s, kw_list[i], (size_t)len) == 0)
            return true;
    }
    return false;
}

static const char **fv_keyword_list(const char *ext)
{
    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".cpp") == 0
        || strcmp(ext, ".h") == 0 || strcmp(ext, ".hpp") == 0
        || strcmp(ext, ".glsl") == 0 || strcmp(ext, ".hlsl") == 0
        || strcmp(ext, ".vert") == 0 || strcmp(ext, ".frag") == 0
        || strcmp(ext, ".sc") == 0)
        return s_c_keywords;
    if (strcmp(ext, ".java") == 0 || strcmp(ext, ".kt") == 0)
        return s_java_keywords;
    if (strcmp(ext, ".py") == 0 || strcmp(ext, ".lua") == 0)
        return s_py_keywords;
    return NULL;
}

static ImVec4 fv_code_keyword_color(const char *ext)
{
    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".cpp") == 0
        || strcmp(ext, ".h") == 0 || strcmp(ext, ".hpp") == 0)
        return ImVec4(0.4f, 0.8f, 0.9f, 1.0f);
    if (strcmp(ext, ".java") == 0 || strcmp(ext, ".kt") == 0)
        return ImVec4(0.9f, 0.6f, 0.3f, 1.0f);
    if (strcmp(ext, ".glsl") == 0 || strcmp(ext, ".hlsl") == 0
        || strcmp(ext, ".vert") == 0 || strcmp(ext, ".frag") == 0
        || strcmp(ext, ".sc") == 0)
        return ImVec4(0.5f, 0.9f, 0.5f, 1.0f);
    if (strcmp(ext, ".json") == 0 || strcmp(ext, ".xml") == 0
        || strcmp(ext, ".yaml") == 0 || strcmp(ext, ".yml") == 0)
        return ImVec4(0.9f, 0.9f, 0.4f, 1.0f);
    if (strcmp(ext, ".py") == 0 || strcmp(ext, ".lua") == 0)
        return ImVec4(0.9f, 0.5f, 0.9f, 1.0f);
    return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
}

/* ── Render a single line with inline syntax coloring ────────────── */

static void fv_render_syntax_line(const char *start, const char *end,
                                  const char **kw_list, ImVec4 base_col)
{
    if (start >= end) { ImGui::TextUnformatted(""); return; }

    const char *p = start;
    bool first_token = true;

    while (p < end) {
        if (p[0] == '/' && (p + 1) < end && p[1] == '/') {
            if (!first_token) ImGui::SameLine(0, 0);
            ImGui::PushStyleColor(ImGuiCol_Text, SYN_COMMENT);
            ImGui::TextUnformatted(p, end);
            ImGui::PopStyleColor();
            return;
        }

        if (p == start && *p == '#') {
            ImGui::PushStyleColor(ImGuiCol_Text, SYN_PREPROC);
            ImGui::TextUnformatted(start, end);
            ImGui::PopStyleColor();
            return;
        }

        if (*p == '"' || *p == '\'') {
            char quote = *p;
            const char *str_start = p++;
            while (p < end && *p != quote) {
                if (*p == '\\' && (p + 1) < end) p++;
                p++;
            }
            if (p < end) p++;
            if (!first_token) ImGui::SameLine(0, 0);
            ImGui::PushStyleColor(ImGuiCol_Text, SYN_STRING);
            ImGui::TextUnformatted(str_start, p);
            ImGui::PopStyleColor();
            first_token = false;
            continue;
        }

        if (isdigit((unsigned char)*p) ||
            (*p == '.' && (p + 1) < end && isdigit((unsigned char)p[1]))) {
            const char *num_start = p;
            if (*p == '0' && (p + 1) < end && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                while (p < end && isxdigit((unsigned char)*p)) p++;
            } else {
                while (p < end && (isdigit((unsigned char)*p) || *p == '.')) p++;
            }
            while (p < end && isalpha((unsigned char)*p)) p++;
            if (!first_token) ImGui::SameLine(0, 0);
            ImGui::PushStyleColor(ImGuiCol_Text, SYN_NUMBER);
            ImGui::TextUnformatted(num_start, p);
            ImGui::PopStyleColor();
            first_token = false;
            continue;
        }

        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *id_start = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int id_len = (int)(p - id_start);
            bool is_kw = kw_list && fv_is_keyword(id_start, id_len, kw_list);
            if (!first_token) ImGui::SameLine(0, 0);
            ImGui::PushStyleColor(ImGuiCol_Text, is_kw ? base_col : SYN_DEFAULT);
            ImGui::TextUnformatted(id_start, p);
            ImGui::PopStyleColor();
            first_token = false;
            continue;
        }

        if (*p == ' ' || *p == '\t') {
            const char *ws_start = p;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            if (!first_token) ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(ws_start, p);
            first_token = false;
            continue;
        }

        {
            const char *punc_start = p;
            p++;
            if (!first_token) ImGui::SameLine(0, 0);
            ImGui::PushStyleColor(ImGuiCol_Text, SYN_DEFAULT);
            ImGui::TextUnformatted(punc_start, p);
            ImGui::PopStyleColor();
            first_token = false;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  FIND / REPLACE HELPERS
 * ══════════════════════════════════════════════════════════════════════ */

/* Case-insensitive strstr. */
static const char *fv_stristr(const char *haystack, const char *needle)
{
    if (!needle[0]) return haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        bool match = true;
        for (size_t i = 0; i < nlen; i++) {
            if (!haystack[i]) { match = false; break; }
            if (tolower((unsigned char)haystack[i]) !=
                tolower((unsigned char)needle[i])) { match = false; break; }
        }
        if (match) return haystack;
    }
    return NULL;
}

/* Count occurrences of needle in text (case-insensitive). */
static int fv_count_matches(const char *text, const char *needle)
{
    if (!needle[0]) return 0;
    int count = 0;
    size_t nlen = strlen(needle);
    const char *p = text;
    while ((p = fv_stristr(p, needle)) != NULL) {
        count++;
        p += nlen;
    }
    return count;
}

/* Find the Nth occurrence (0-based) of needle in text. Returns offset or -1. */
static int fv_find_nth(const char *text, const char *needle, int n)
{
    if (!needle[0]) return -1;
    size_t nlen = strlen(needle);
    const char *p = text;
    int idx = 0;
    while ((p = fv_stristr(p, needle)) != NULL) {
        if (idx == n) return (int)(p - text);
        idx++;
        p += nlen;
    }
    return -1;
}

/* ══════════════════════════════════════════════════════════════════════
 *  CLEANUP
 * ══════════════════════════════════════════════════════════════════════ */

void fv_code_close_tab(FvTab *tab)
{
    if (tab->edit_buf) {
        ED_FREE(tab->edit_buf);
        tab->edit_buf = NULL;
    }
    tab->edit_mode = false;
    tab->modified  = false;
}

/* ══════════════════════════════════════════════════════════════════════
 *  RENDER
 * ══════════════════════════════════════════════════════════════════════ */

void fv_render_code(FvTab *tab)
{
    const char *src = tab->edit_mode ? tab->edit_buf : tab->content;
    int src_len = tab->edit_mode ? (int)strlen(tab->edit_buf) : tab->content_len;

    /* ── Toolbar ─────────────────────────────────────────────────── */
    {
        /* Edit / View toggle */
        if (tab->edit_mode) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.65f, 0.25f, 1.0f));
            if (ImGui::Button(jce_editor_i18n("codeViewer.editing"))) {
                /* Switch back to view mode */
                tab->edit_mode = false;
            }
            ImGui::PopStyleColor(2);
        } else {
            if (ImGui::Button(jce_editor_i18n("codeViewer.edit"))) {
                /* Activate edit mode — allocate buffer */
                if (!tab->edit_buf) {
                    int cap = (tab->content_len + 1 > FV_EDIT_BUF_CAP)
                            ? tab->content_len + 1 : FV_EDIT_BUF_CAP;
                    tab->edit_buf = (char *)ED_MALLOC((size_t)cap);
                    tab->edit_buf_cap = cap;
                    if (tab->edit_buf) {
                        memcpy(tab->edit_buf, tab->content, (size_t)tab->content_len);
                        tab->edit_buf[tab->content_len] = '\0';
                    }
                }
                if (tab->edit_buf)
                    tab->edit_mode = true;
            }
        }

        /* Find button (Ctrl+F) */
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("codeViewer.find"))) {
            tab->show_find_replace = !tab->show_find_replace;
        }

        /* Save (only if modified) */
        if (tab->modified) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.45f, 0.7f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 0.8f, 1.0f));
            if (ImGui::Button(jce_editor_i18n("codeViewer.save"))) {
                const char *save_src = tab->edit_buf ? tab->edit_buf : tab->content;
                size_t save_len = strlen(save_src);
                if (ed_write_file(tab->path, save_src, save_len)) {
                    tab->modified = false;
                    /* Refresh content from edit buffer */
                    if (tab->edit_buf) {
                        int new_len = (int)save_len;
                        ED_FREE(tab->content);
                        tab->content = (char *)ED_MALLOC((size_t)new_len + 1);
                        if (tab->content) {
                            memcpy(tab->content, tab->edit_buf, (size_t)new_len);
                            tab->content[new_len] = '\0';
                            tab->content_len = new_len;
                        }
                    }
                    LOG_INFO(LOG_TAG, "saved '%s'", tab->display_name);
                }
            }
            ImGui::PopStyleColor(2);
        }

        /* Reload */
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("codeViewer.reload"))) {
            size_t got = 0, total = 0;
            char *buf = (char *)ed_read_file_capped(tab->path, FV_MAX_CONTENT,
                                                    &got, &total);
            if (buf) {
                int n = (int)got;
                ED_FREE(tab->content);
                tab->content = buf;
                tab->content_len = n;
                tab->file_size = (long)total;
                /* Refresh edit buffer too */
                if (tab->edit_buf) {
                    int cap = (n + 1 > FV_EDIT_BUF_CAP) ? n + 1 : FV_EDIT_BUF_CAP;
                    ED_FREE(tab->edit_buf);
                    tab->edit_buf = (char *)ED_MALLOC((size_t)cap);
                    tab->edit_buf_cap = cap;
                    if (tab->edit_buf) {
                        memcpy(tab->edit_buf, tab->content, (size_t)n);
                        tab->edit_buf[n] = '\0';
                    }
                }
                tab->modified = false;
            }
        }

        /* Open in VS Code */
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("viewer.code.openInEditor"))) {
            jce_host_open_in_text_editor(tab->path);
        }

        /* File info (right side) */
        ImGui::SameLine();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", tab->ext + 1);
        ImGui::SameLine();
        if (tab->file_size >= 1024)
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                "  %.1f KB  %d bytes", (double)tab->file_size / 1024.0,
                src_len);
        else
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                "  %ld bytes", tab->file_size);
        if (tab->modified) {
            ImGui::SameLine();
            ImGui::TextColored(JCE_COLOR_TEXT_WARNING, "%s", jce_editor_i18n("codeViewer.modified"));
        }
    }

    ImGui::Separator();

    /* ── Find / Replace Panel ────────────────────────────────────── */
    if (tab->show_find_replace) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.2f, 1.0f));
        ImGui::BeginChild("##findpanel", ImVec2(0, tab->edit_mode ? 60 : 32),
                          ImGuiChildFlags_Borders);
        {
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText("##find", tab->find_buf, sizeof(tab->find_buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                /* Enter = Find Next */
                if (tab->find_buf[0]) {
                    int total = fv_count_matches(src, tab->find_buf);
                    if (total > 0) {
                        tab->find_index = (tab->find_index + 1) % total;
                    }
                }
            }
            ImGui::SameLine();
            int match_count = (tab->find_buf[0]) ? fv_count_matches(src, tab->find_buf) : 0;
            if (ImGui::Button(jce_editor_i18n("codeViewer.findNext")) && match_count > 0) {
                tab->find_index = (tab->find_index + 1) % match_count;
            }
            ImGui::SameLine();
            if (ImGui::Button(jce_editor_i18n("codeViewer.findPrev")) && match_count > 0) {
                tab->find_index = (tab->find_index - 1 + match_count) % match_count;
            }
            ImGui::SameLine();
            if (match_count > 0)
                ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%d/%d",
                    tab->find_index + 1, match_count);
            else if (tab->find_buf[0])
                ImGui::TextColored(JCE_COLOR_TEXT_ERROR, "%s", jce_editor_i18n("codeViewer.noMatches"));
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                tab->show_find_replace = false;
            }

            /* Replace row (only in edit mode) */
            if (tab->edit_mode) {
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("##replace", tab->replace_buf, sizeof(tab->replace_buf));
                ImGui::SameLine();
                if (ImGui::Button(jce_editor_i18n("codeViewer.replaceOne")) && tab->edit_buf && tab->find_buf[0]) {
                    int offset = fv_find_nth(tab->edit_buf, tab->find_buf, tab->find_index);
                    if (offset >= 0) {
                        size_t flen = strlen(tab->find_buf);
                        size_t rlen = strlen(tab->replace_buf);
                        size_t slen = strlen(tab->edit_buf);
                        size_t new_len = slen - flen + rlen;
                        if ((int)new_len + 1 <= tab->edit_buf_cap) {
                            memmove(tab->edit_buf + offset + rlen,
                                    tab->edit_buf + offset + flen,
                                    slen - (size_t)offset - flen + 1);
                            memcpy(tab->edit_buf + offset, tab->replace_buf, rlen);
                            tab->modified = true;
                        }
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button(jce_editor_i18n("codeViewer.replaceAll")) && tab->edit_buf && tab->find_buf[0]) {
                    size_t flen = strlen(tab->find_buf);
                    size_t rlen = strlen(tab->replace_buf);
                    /* Build a new string with all replacements */
                    std::string result;
                    const char *p = tab->edit_buf;
                    while (*p) {
                        const char *found = fv_stristr(p, tab->find_buf);
                        if (found) {
                            result.append(p, found - p);
                            result.append(tab->replace_buf, rlen);
                            p = found + flen;
                        } else {
                            result.append(p);
                            break;
                        }
                    }
                    if ((int)result.size() + 1 <= tab->edit_buf_cap) {
                        memcpy(tab->edit_buf, result.c_str(), result.size() + 1);
                        tab->modified = true;
                    }
                    tab->find_index = 0;
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    /* Ctrl+F shortcut */
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && jce_hotkey_pressed(JCE_HK_EDIT_FIND))
    {
        tab->show_find_replace = true;
    }

    /* ── Code Area ───────────────────────────────────────────────── */
    if (tab->edit_mode && tab->edit_buf) {
        /* Editable multi-line text */
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.y < 50.0f) avail.y = 50.0f;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.12f, 0.15f, 1.0f));
        ImFont *code_font = (ImGui::GetIO().Fonts->Fonts.Size > 1)
                          ? ImGui::GetIO().Fonts->Fonts[1]
                          : ImGui::GetIO().FontDefault;
        ImGui::PushFont(code_font);

        if (ImGui::InputTextMultiline("##editarea", tab->edit_buf, (size_t)tab->edit_buf_cap,
                                      avail, ImGuiInputTextFlags_AllowTabInput))
        {
            tab->modified = true;
        }

        ImGui::PopFont();
        ImGui::PopStyleColor();
    } else {
        /* Read-only view with line numbers + syntax highlighting */
        ImGui::BeginChild("##code", ImVec2(0, 0), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        int line_count = 1;
        for (int c = 0; c < tab->content_len; c++)
            if (tab->content[c] == '\n') line_count++;

        int digits = 1;
        { int tmp = line_count; while (tmp >= 10) { digits++; tmp /= 10; } }
        char num_fmt[16];
        snprintf(num_fmt, sizeof(num_fmt), "%%%dd", digits);

        ImVec4 code_col = fv_code_keyword_color(tab->ext);
        const char **kw_list = fv_keyword_list(tab->ext);

        /* Precompute find highlight offset (if searching) */
        int hl_offset = -1;
        int hl_len = 0;
        if (tab->find_buf[0] && !tab->edit_mode) {
            hl_offset = fv_find_nth(tab->content, tab->find_buf, tab->find_index);
            hl_len = (int)strlen(tab->find_buf);
        }

        const char *line_start = tab->content;
        int line_num = 1;
        int char_offset = 0;
        for (;;) {
            const char *line_end = line_start;
            while (*line_end && *line_end != '\n') line_end++;

            char num_buf[16];
            snprintf(num_buf, sizeof(num_buf), num_fmt, line_num);
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", num_buf);
            ImGui::SameLine();

            /* Check if search highlight is on this line */
            int line_len = (int)(line_end - line_start);
            if (hl_offset >= 0
                && hl_offset >= char_offset
                && hl_offset < char_offset + line_len)
            {
                /* Highlight the match on this line */
                int rel = hl_offset - char_offset;
                /* Before match */
                if (rel > 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, SYN_DEFAULT);
                    ImGui::TextUnformatted(line_start, line_start + rel);
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0, 0);
                }
                /* The match (yellow bg) */
                int mlen = hl_len;
                if (rel + mlen > line_len) mlen = line_len - rel;
                ImVec2 mpos = ImGui::GetCursorScreenPos();
                ImVec2 msz = ImGui::CalcTextSize(line_start + rel,
                                                 line_start + rel + mlen);
                ImGui::GetWindowDrawList()->AddRectFilled(
                    mpos, ImVec2(mpos.x + msz.x, mpos.y + msz.y),
                    IM_COL32(180, 160, 60, 120), 2.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.2f, 1.0f));
                ImGui::TextUnformatted(line_start + rel, line_start + rel + mlen);
                ImGui::PopStyleColor();
                /* After match */
                if (rel + mlen < line_len) {
                    ImGui::SameLine(0, 0);
                    ImGui::PushStyleColor(ImGuiCol_Text, SYN_DEFAULT);
                    ImGui::TextUnformatted(line_start + rel + mlen, line_end);
                    ImGui::PopStyleColor();
                }
            } else if (line_end > line_start) {
                if (kw_list) {
                    fv_render_syntax_line(line_start, line_end, kw_list, code_col);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, code_col);
                    ImGui::TextUnformatted(line_start, line_end);
                    ImGui::PopStyleColor();
                }
            } else {
                ImGui::TextUnformatted("");
            }

            if (*line_end == '\0') break;
            char_offset += line_len + 1;
            line_start = line_end + 1;
            line_num++;
        }

        ImGui::EndChild();
    }
}
