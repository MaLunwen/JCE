/*
 * jce_editor_json_reveal.cpp  Locate live editor objects in their JSON
 * source text and open the Code Viewer there.  See header.
 */

#include "jce_editor_json_reveal.h"

#include "core/jce_editor_alloc.h"
#include "core/jce_editor_state.h"
#include "io/jce_editor_file_util.h"
#include "scene/jce_editor_scene_render.h"
#include "ui/jce_editor_layout.h"
#include "viewers/jce_file_viewer.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ── entity block locator ─────────────────────────────────────────────
 *
 * Located by NAME first: entity ids REMAP on load (EntityRemap), so a live
 * flecs id only matches the on-disk text when the file was saved this
 * session.  Name hits are filtered to ENTITY-level "name" keys — an
 * entity's "name" is adjacent to its "id" key, while the EditorMeta
 * component duplicates the name next to a "type" key — then disambiguated
 * by the live id when several entities share a name.  Whitespace-tolerant
 * so it works on both editor-saved (tab-separated) and hand-authored
 * scene JSON. */
static bool locate_entity_in_scene_json(const char *scene_path,
                                        const char *entity_name,
                                        unsigned long long live_id,
                                        int *out_line)
{
    *out_line = 1;
    if (!scene_path || !entity_name || !entity_name[0]) return false;

    size_t got = 0, total = 0;
    char *text = (char *)ed_read_file_capped(scene_path,
                                             (size_t)8 * 1024 * 1024,
                                             &got, &total);
    if (!text) return false;

    /* JSON-escape quotes/backslashes in the needle value. */
    char esc[JCE_MAX_ENTITY_NAME * 2 + 2];
    {
        int j = 0;
        for (const char *p = entity_name; *p && j < (int)sizeof(esc) - 2; p++) {
            if (*p == '"' || *p == '\\') esc[j++] = '\\';
            esc[j++] = *p;
        }
        esc[j] = '\0';
    }
    const size_t esc_len = strlen(esc);

    char idbuf[32];
    snprintf(idbuf, sizeof idbuf, "%llu", live_id);
    const size_t id_len = strlen(idbuf);

    int id_line = -1;     /* name match whose sibling "id" equals live_id */
    int first_line = -1;  /* first entity-level name match */

    const char *p = text;
    while ((p = strstr(p, "\"name\"")) != NULL) {
        const char *after = p + 6;

        /* value must be : "<esc>"  (whitespace-tolerant) */
        const char *v = after;
        while (*v == ' ' || *v == '\t') v++;
        if (*v != ':') { p = after; continue; }
        v++;
        while (*v == ' ' || *v == '\t') v++;
        if (*v != '"') { p = after; continue; }
        v++;
        if (strncmp(v, esc, esc_len) != 0 || v[esc_len] != '"') { p = after; continue; }

        /* Entity-level check: scan back a short window; an "id" key before
         * any "type" key marks the entity header (EditorMeta's duplicated
         * name sits after "type": "EditorMeta"). */
        const char *back = (p - text > 200) ? p - 200 : text;
        bool entity_level = false, id_match = false;
        for (const char *q = p - 4; q >= back; q--) {
            if (q[0] != '"') continue;
            if (strncmp(q, "\"id\"", 4) == 0) {
                entity_level = true;
                const char *d = q + 4;
                while (*d == ' ' || *d == '\t' || *d == ':') d++;
                if (strncmp(d, idbuf, id_len) == 0
                    && !isdigit((unsigned char)d[id_len]))
                    id_match = true;
                break;
            }
            if (strncmp(q, "\"type\"", 6) == 0) break;
        }

        if (entity_level) {
            int line = 1;
            for (const char *c = text; c < p; c++)
                if (*c == '\n') line++;
            if (first_line < 0) first_line = line;
            if (id_match) { id_line = line; break; }
        }
        p = after;
    }

    ED_FREE(text);
    int line = (id_line > 0) ? id_line : first_line;
    if (line > 0) { *out_line = line; return true; }
    return false;
}

/* ── public API ───────────────────────────────────────────────────── */

bool jce_editor_reveal_entity_in_scene_json(uint32_t entity_id)
{
    const char *sp = jce_state_get_current_scene_path();
    if (!sp || !sp[0] || strncmp(sp, "bundle://", 9) == 0)
        return false;

    const char *name = jce_state_entity_name(entity_id);
    if (!name) name = "";

    int line = 1;
    JceEntity e = jce_state_to_ecs_entity(entity_id);
    bool found = locate_entity_in_scene_json(sp, name,
                                             (unsigned long long)e, &line);

    jce_file_viewer_open_text_at(sp, line);
    jce_file_viewer_request_focus();
    jce_editor_layout_request_focus_file_viewer();
    return found;
}

void jce_editor_reveal_json_source(const char *asset_path, int line)
{
    if (!asset_path || !asset_path[0]) return;

    char abs[1024];
    const char *open_path = asset_path;
    if (jce_editor_resolve_asset_path(asset_path, abs, (int)sizeof abs))
        open_path = abs;

    jce_file_viewer_open_text_at(open_path, line);
    jce_file_viewer_request_focus();
    jce_editor_layout_request_focus_file_viewer();
}
