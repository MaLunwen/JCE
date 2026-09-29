/*
 * jce_fv_code.cpp  Text / Code sub-viewer with syntax highlighting,
 *                  find/replace, and edit mode.
 *
 * Reference: Java CodeViewerWindow.
 */

#include "io/jce_editor_file_util.h"
#include "jce_fv_common.h"
#include "jce_json_classify.h"
#include "core/jce_hotkeys.h"
#include "core/jce_editor_config.h"

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

/* JSON palette (VS Code Dark+ inspired: keys pop, punctuation recedes —
 * the old path painted whole JSON lines in one flat yellow). */
static const ImVec4 SYN_JSON_KEY   = ImVec4(0.61f, 0.86f, 1.00f, 1.0f);
static const ImVec4 SYN_JSON_LIT   = ImVec4(0.34f, 0.61f, 0.84f, 1.0f);  /* true/false/null */
static const ImVec4 SYN_JSON_PUNCT = ImVec4(0.55f, 0.55f, 0.58f, 1.0f);

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
        || strcmp(ext, ".yaml") == 0 || strcmp(ext, ".yml") == 0
        || strcmp(ext, ".rml") == 0)
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

/* ── Render a single JSON line: keys / strings / numbers / literals ──── */

static void fv_render_json_line(const char *start, const char *end)
{
    if (start >= end) { ImGui::TextUnformatted(""); return; }

    const char *p = start;
    bool first_token = true;

    auto emit = [&](const char *a, const char *b, const ImVec4 &col) {
        if (!first_token) ImGui::SameLine(0, 0);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(a, b);
        ImGui::PopStyleColor();
        first_token = false;
    };

    while (p < end) {
        if (*p == ' ' || *p == '\t') {
            const char *ws = p;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            if (!first_token) ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(ws, p);
            first_token = false;
            continue;
        }

        if (*p == '"') {
            const char *str_start = p++;
            while (p < end && *p != '"') {
                if (*p == '\\' && (p + 1) < end) p++;
                p++;
            }
            if (p < end) p++;
            /* Key iff the next non-space char is ':' (JSON property name). */
            const char *q = p;
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            bool is_key = (q < end && *q == ':');
            emit(str_start, p, is_key ? SYN_JSON_KEY : SYN_STRING);
            continue;
        }

        if (isdigit((unsigned char)*p)
            || (*p == '-' && (p + 1) < end && isdigit((unsigned char)p[1]))) {
            const char *num = p;
            if (*p == '-') p++;
            while (p < end && (isdigit((unsigned char)*p) || *p == '.'
                               || *p == 'e' || *p == 'E' || *p == '+' || *p == '-'))
                p++;
            emit(num, p, SYN_NUMBER);
            continue;
        }

        if (isalpha((unsigned char)*p)) {
            const char *id = p;
            while (p < end && isalpha((unsigned char)*p)) p++;
            int idl = (int)(p - id);
            bool lit = (idl == 4 && memcmp(id, "true", 4) == 0)
                    || (idl == 5 && memcmp(id, "false", 5) == 0)
                    || (idl == 4 && memcmp(id, "null", 4) == 0);
            emit(id, p, lit ? SYN_JSON_LIT : SYN_DEFAULT);
            continue;
        }

        /* Punctuation run: {}[],: etc — dim so the data pops. */
        {
            const char *punc = p;
            while (p < end && !isalnum((unsigned char)*p)
                   && *p != '"' && *p != ' ' && *p != '\t' && *p != '-')
                p++;
            if (p == punc) p++;   /* lone '-' not starting a number */
            emit(punc, p, SYN_JSON_PUNCT);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  LINE INDEX (offset of each line start; enables clipped rendering and
 *  O(log n) offset->line lookups for find / goto)
 * ══════════════════════════════════════════════════════════════════════ */

void fv_code_invalidate_index(FvTab *tab)
{
    if (tab->line_offs) { ED_FREE(tab->line_offs); tab->line_offs = NULL; }
    tab->line_count      = 0;
    tab->json_classified = false;
}

static void fv_code_build_line_index(FvTab *tab)
{
    int lines = 1;
    for (int c = 0; c < tab->content_len; c++)
        if (tab->content[c] == '\n') lines++;

    tab->line_offs = (int *)ED_MALLOC(sizeof(int) * (size_t)(lines + 1));
    if (!tab->line_offs) { tab->line_count = 0; return; }

    int li = 0;
    tab->line_offs[li++] = 0;
    for (int c = 0; c < tab->content_len; c++)
        if (tab->content[c] == '\n') tab->line_offs[li++] = c + 1;
    tab->line_offs[li] = tab->content_len;   /* sentinel: end of last line */
    tab->line_count = lines;
}

/* 0-based line containing byte `offset` (binary search over line starts). */
static int fv_code_line_of_offset(const FvTab *tab, int offset)
{
    if (!tab->line_offs || tab->line_count <= 0) return 0;
    int lo = 0, hi = tab->line_count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (tab->line_offs[mid] <= offset) lo = mid;
        else                               hi = mid - 1;
    }
    return lo;
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
    fv_code_invalidate_index(tab);
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
                        fv_code_invalidate_index(tab);
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
                tab->file_size = (uint64_t)total;
                fv_code_invalidate_index(tab);
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

        /* Open in the user's configured external script editor
           (Preferences > External Tools); empty falls back to VS Code /
           the OS default handler. */
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("viewer.code.openInEditor"))) {
            JceEditorConfig cfg;
            jce_editor_config_load(&cfg);
            if (!jce_host_open_in_editor(cfg.external_script_editor, tab->path))
                jce_host_open_in_text_editor(tab->path);
        }

        /* JSON type badge: classify once per content change and show a
         * rounded chip so the user always knows WHAT kind of JSON this is
         * (scene / anim state machine / particles / ...). */
        if (strcmp(tab->ext, ".json") == 0 || tab->type == JCE_FV_SCENE) {
            if (!tab->json_classified) {
                tab->json_kind = (int)jce_json_classify(tab->content,
                                                        tab->content_len,
                                                        tab->path);
                tab->json_classified = true;
            }
            if (tab->json_kind > (int)JCE_JSONK_NOT_JSON) {
                const char *badge = jce_json_kind_label((JceJsonKind)tab->json_kind);
                if (badge && badge[0]) {
                    ImGui::SameLine(0, 10.0f);
                    ImVec2 ts = ImGui::CalcTextSize(badge);
                    const float pad_x = 7.0f, pad_y = 2.0f;
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float h = ts.y + pad_y * 2.0f;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(p, ImVec2(p.x + ts.x + pad_x * 2.0f, p.y + h),
                                      jce_json_kind_color((JceJsonKind)tab->json_kind),
                                      h * 0.5f);
                    dl->AddText(ImVec2(p.x + pad_x, p.y + pad_y),
                                IM_COL32(235, 235, 235, 255), badge);
                    ImGui::Dummy(ImVec2(ts.x + pad_x * 2.0f, h));
                }
            }
        }

        /* File info (right side) */
        ImGui::SameLine();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", tab->ext + 1);
        ImGui::SameLine();
        if (tab->file_size >= 1024)
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                jce_editor_i18n_or("viewer.code.sizeKb", "  %.1f KB  %d bytes"),
                (double)tab->file_size / 1024.0, src_len);
        else
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                jce_editor_i18n_or("viewer.code.sizeBytes", "  %ld bytes"), (long)tab->file_size);
        if ((uint64_t)tab->content_len < tab->file_size && !tab->edit_mode) {
            ImGui::SameLine();
            ImGui::TextColored(JCE_COLOR_TEXT_WARNING, "%s",
                jce_editor_i18n_or("viewer.code.truncated", "(truncated preview)"));
        }
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
                        tab->scroll_to_find = true;
                    }
                }
            }
            ImGui::SameLine();
            int match_count = (tab->find_buf[0]) ? fv_count_matches(src, tab->find_buf) : 0;
            if (ImGui::Button(jce_editor_i18n("codeViewer.findNext")) && match_count > 0) {
                tab->find_index = (tab->find_index + 1) % match_count;
                tab->scroll_to_find = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(jce_editor_i18n("codeViewer.findPrev")) && match_count > 0) {
                tab->find_index = (tab->find_index - 1 + match_count) % match_count;
                tab->scroll_to_find = true;
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
        /* Read-only view: line numbers + syntax highlighting, rendered
         * through an ImGuiListClipper over a prebuilt line index so large
         * files (multi-MB scene JSONs) only pay for the visible rows.  The
         * index also gives O(log n) offset→line for find/goto scrolling. */
        if (!tab->line_offs)
            fv_code_build_line_index(tab);

        ImGui::BeginChild("##code", ImVec2(0, 0), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        const int line_count = (tab->line_count > 0) ? tab->line_count : 1;
        int digits = 1;
        { int tmp = line_count; while (tmp >= 10) { digits++; tmp /= 10; } }
        char num_fmt[16];
        snprintf(num_fmt, sizeof(num_fmt), "%%%dd", digits);

        ImVec4 code_col = fv_code_keyword_color(tab->ext);
        const char **kw_list = fv_keyword_list(tab->ext);
        const bool is_json = (strcmp(tab->ext, ".json") == 0)
                          || tab->type == JCE_FV_SCENE;

        /* Precompute find highlight offset (if searching) */
        int hl_offset = -1;
        int hl_len = 0;
        if (tab->find_buf[0]) {
            hl_offset = fv_find_nth(tab->content, tab->find_buf, tab->find_index);
            hl_len = (int)strlen(tab->find_buf);
        }

        const float line_h = ImGui::GetTextLineHeightWithSpacing();

        /* Deferred scrolls: jump-to-line (hierarchy "View in JSON") and
         * find navigation both center the target ~1/3 down the view. */
        if (tab->goto_scroll_pending && tab->goto_line > 0) {
            float y = (float)(tab->goto_line - 1) * line_h
                    - ImGui::GetWindowHeight() * 0.35f;
            ImGui::SetScrollY(y < 0.0f ? 0.0f : y);
            tab->goto_scroll_pending = false;
        }
        if (tab->scroll_to_find) {
            if (hl_offset >= 0 && tab->line_offs) {
                int fl = fv_code_line_of_offset(tab, hl_offset);
                float y = (float)fl * line_h - ImGui::GetWindowHeight() * 0.35f;
                ImGui::SetScrollY(y < 0.0f ? 0.0f : y);
            }
            tab->scroll_to_find = false;
        }

        if (tab->goto_flash > 0.0f)
            tab->goto_flash -= ImGui::GetIO().DeltaTime;

        ImGuiListClipper clipper;
        clipper.Begin(line_count, line_h);
        while (clipper.Step())
        for (int li = clipper.DisplayStart; li < clipper.DisplayEnd; li++) {
            int char_offset = tab->line_offs ? tab->line_offs[li] : 0;
            const char *line_start = tab->content + char_offset;
            const char *line_end;
            if (tab->line_offs) {
                line_end = tab->content + tab->line_offs[li + 1];
                /* Strip the newline (and a CR) from the rendered span. */
                if (line_end > line_start && line_end[-1] == '\n') line_end--;
                if (line_end > line_start && line_end[-1] == '\r') line_end--;
            } else {
                line_end = line_start;
            }

            /* Jump-target row: pulsing wash + steady left accent bar. */
            if (tab->goto_line - 1 == li) {
                ImVec2 rp = ImGui::GetCursorScreenPos();
                float x0 = ImGui::GetWindowPos().x;
                float x1 = x0 + ImGui::GetWindowWidth();
                ImDrawList *dl = ImGui::GetWindowDrawList();
                float pulse = (tab->goto_flash > 0.0f)
                            ? 0.5f + 0.5f * sinf(tab->goto_flash * 6.0f)
                            : 0.0f;
                int alpha = (int)(40.0f + 90.0f * pulse);
                dl->AddRectFilled(ImVec2(x0, rp.y), ImVec2(x1, rp.y + line_h),
                                  IM_COL32(90, 140, 200, alpha));
                dl->AddRectFilled(ImVec2(x0, rp.y),
                                  ImVec2(x0 + 3.0f, rp.y + line_h),
                                  IM_COL32(110, 170, 240, 220));
            }

            char num_buf[16];
            snprintf(num_buf, sizeof(num_buf), num_fmt, li + 1);
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
                if (is_json) {
                    fv_render_json_line(line_start, line_end);
                } else if (kw_list) {
                    fv_render_syntax_line(line_start, line_end, kw_list, code_col);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, code_col);
                    ImGui::TextUnformatted(line_start, line_end);
                    ImGui::PopStyleColor();
                }
            } else {
                ImGui::TextUnformatted("");
            }
        }
        clipper.End();

        ImGui::EndChild();
    }
}
