/*
 * jce_panel_vfx_graph.cpp  VFX Graph (node-based particle authoring)
 *                          Sprint 3 #13 / 0.8.25
 *
 * A node-graph editor for composing particle effect chains, modeled
 * after Material Graph (Sprint 3 #11). The graph is a front-end for the
 * standard particle pipeline: Export compiles it to a `*.particles.json`
 * document (the canonical JceParticleEmitterDesc key set parsed by
 * jce_particles_desc_load_json) that a Particle Emitter component
 * consumes via its asset path.
 *
 * Consolidation v0.9.9: the panel previously compiled to an orphaned
 * `*.vfx.json` schema read by a dead engine-side interpreter
 * (jce_vfx_graph.c, zero callers). Both were removed; this panel now
 * targets the one real runtime.
 *
 * Node types:
 *   - Output      : sink for one Emitter chain.
 *   - Emitter     : core spawn config (rate, lifetime, speed, size).
 *   - ColorRamp   : start/end RGBA over particle life.
 *   - Velocity    : direction + magnitude bias.
 *   - SubEmitter  : burst on death/birth   (lossy: dropped on export).
 *   - Trail       : per-particle trail     (lossy: dropped on export).
 *
 * Interaction: drag bodies; right-click canvas to add nodes; right-click
 * a node to delete; output pin -> compatible input pin to link; Export
 * button writes .particles.json next to the loaded source.
 */

#include "io/jce_editor_file_util.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_theme_palette.h"

#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/os/core/jce_json.h>
}

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

enum NType {
    NT_OUTPUT      = 0,
    NT_EMITTER     = 1,
    NT_COLOR_RAMP  = 2,
    NT_VELOCITY    = 3,
    NT_SUB_EMITTER = 4,
    NT_TRAIL       = 5,
};

enum SKind { SK_IN = 0, SK_OUT = 1 };

struct Sock {
    SKind       kind;
    const char *name;
};

struct Node {
    int    id;
    NType  type;
    ImVec2 pos;
    /* Payloads. */
    float  rate       = 50.0f;   /* Emitter */
    float  lifetime   = 2.0f;
    float  speed      = 1.0f;
    float  size       = 0.1f;
    float  start_col[4] = {1, 1, 1, 1};  /* ColorRamp */
    float  end_col[4]   = {1, 1, 1, 0};
    float  vel_dir[3]   = {0, 1, 0};     /* Velocity */
    float  vel_mag      = 1.0f;
    int    sub_burst    = 8;             /* SubEmitter */
    char   sub_event[16] = "death";
    float  trail_len    = 0.5f;          /* Trail */
    float  trail_width  = 0.05f;
};

struct Link {
    int from_node, from_socket;
    int to_node,   to_socket;
};

static const Sock *node_sockets(NType t, int *count)
{
    static const Sock out_s[]      = {{SK_IN, "Emitter"}};
    static const Sock emit_s[]     = {{SK_OUT, "Out"}, {SK_IN, "Color"}, {SK_IN, "Velocity"}, {SK_IN, "Sub"}, {SK_IN, "Trail"}};
    static const Sock color_s[]    = {{SK_OUT, "Out"}};
    static const Sock vel_s[]      = {{SK_OUT, "Out"}};
    static const Sock sub_s[]      = {{SK_OUT, "Out"}};
    static const Sock trail_s[]    = {{SK_OUT, "Out"}};
    switch (t) {
        case NT_OUTPUT:      *count = (int)(sizeof(out_s)/sizeof(out_s[0]));     return out_s;
        case NT_EMITTER:     *count = (int)(sizeof(emit_s)/sizeof(emit_s[0]));   return emit_s;
        case NT_COLOR_RAMP:  *count = (int)(sizeof(color_s)/sizeof(color_s[0])); return color_s;
        case NT_VELOCITY:    *count = (int)(sizeof(vel_s)/sizeof(vel_s[0]));     return vel_s;
        case NT_SUB_EMITTER: *count = (int)(sizeof(sub_s)/sizeof(sub_s[0]));     return sub_s;
        case NT_TRAIL:       *count = (int)(sizeof(trail_s)/sizeof(trail_s[0])); return trail_s;
    }
    *count = 0;
    return nullptr;
}

static const char *node_title(NType t)
{
    switch (t) {
        case NT_OUTPUT:      return "Output";
        case NT_EMITTER:     return "Emitter";
        case NT_COLOR_RAMP:  return "ColorRamp";
        case NT_VELOCITY:    return "Velocity";
        case NT_SUB_EMITTER: return "SubEmitter";
        case NT_TRAIL:       return "Trail";
    }
    return "Node";
}

struct Graph {
    std::vector<Node> nodes;
    std::vector<Link> links;
    int               next_id = 1;
    char              path[260] = {0};
    bool              dirty   = false;
};

static Graph s_g;

static void graph_init_default()
{
    if (!s_g.nodes.empty()) return;
    Node out{}; out.id = s_g.next_id++; out.type = NT_OUTPUT;  out.pos = ImVec2(420, 80);
    Node em{};  em.id  = s_g.next_id++; em.type  = NT_EMITTER; em.pos  = ImVec2(80, 80);
    s_g.nodes.push_back(out);
    s_g.nodes.push_back(em);
    Link l{}; l.from_node = em.id; l.from_socket = 0; l.to_node = out.id; l.to_socket = 0;
    s_g.links.push_back(l);
}

static Node *find_node(int id)
{
    for (auto &n : s_g.nodes) if (n.id == id) return &n;
    return nullptr;
}

static int find_input_link(int node_id, int sock_idx)
{
    for (size_t i = 0; i < s_g.links.size(); ++i) {
        const Link &L = s_g.links[i];
        if (L.to_node == node_id && L.to_socket == sock_idx) return (int)i;
    }
    return -1;
}

/* ── JSON I/O ──────────────────────────────────────────────────────── */

static void graph_save()
{
    if (!s_g.path[0]) return;
    JceJson *root = jce_json_object();
    JceJson *nodes = jce_json_array();
    for (const auto &n : s_g.nodes) {
        JceJson *no = jce_json_object();
        jce_json_set_number(no, "id",   n.id);
        jce_json_set_number(no, "type", (double)n.type);
        jce_json_set_number(no, "x",    n.pos.x);
        jce_json_set_number(no, "y",    n.pos.y);
        jce_json_set_number(no, "rate",     n.rate);
        jce_json_set_number(no, "lifetime", n.lifetime);
        jce_json_set_number(no, "speed",    n.speed);
        jce_json_set_number(no, "size",     n.size);
        jce_json_set_float_array(no, "start_col", n.start_col, 4);
        jce_json_set_float_array(no, "end_col",   n.end_col,   4);
        jce_json_set_float_array(no, "vel_dir",   n.vel_dir,   3);
        jce_json_set_number(no, "vel_mag",   n.vel_mag);
        jce_json_set_number(no, "sub_burst", n.sub_burst);
        jce_json_set_string(no, "sub_event", n.sub_event);
        jce_json_set_number(no, "trail_len",   n.trail_len);
        jce_json_set_number(no, "trail_width", n.trail_width);
        jce_json_array_push(nodes, no);
    }
    jce_json_set_child(root, "nodes", nodes);
    JceJson *links = jce_json_array();
    for (const auto &L : s_g.links) {
        JceJson *li = jce_json_object();
        jce_json_set_number(li, "fn", L.from_node);
        jce_json_set_number(li, "fs", L.from_socket);
        jce_json_set_number(li, "tn", L.to_node);
        jce_json_set_number(li, "ts", L.to_socket);
        jce_json_array_push(links, li);
    }
    jce_json_set_child(root, "links", links);
    jce_json_set_number(root, "next_id", s_g.next_id);
    char p[300]; snprintf(p, sizeof(p), "%s.vfxgraph.json", s_g.path);
    ed_write_json_to_file(p, root);
    jce_json_free(root);
    s_g.dirty = false;
    jce_editor_console_log("[VFX Graph] saved %s", p);
}

static void graph_load()
{
    if (!s_g.path[0]) return;
    char p[300]; snprintf(p, sizeof(p), "%s.vfxgraph.json", s_g.path);
    char *buf = nullptr; size_t n = 0;
    buf = (char *)ed_read_file(p, &n);
    if (!buf) return;
    JceJson *root = jce_json_parse(buf, (int)n);
    ED_FREE(buf);
    if (!root) return;
    s_g.nodes.clear(); s_g.links.clear();
    JceJson *nodes = jce_json_get(root, "nodes");
    if (jce_json_is_array(nodes)) {
        int nn = jce_json_array_size(nodes);
        for (int i = 0; i < nn; ++i) {
            JceJson *no = jce_json_array_at(nodes, i);
            Node n{};
            n.id   = jce_json_get_int(no, "id", 0);
            n.type = (NType)jce_json_get_int(no, "type", 0);
            n.pos  = ImVec2((float)jce_json_get_number(no, "x", 0), (float)jce_json_get_number(no, "y", 0));
            n.rate     = (float)jce_json_get_number(no, "rate", 50);
            n.lifetime = (float)jce_json_get_number(no, "lifetime", 2);
            n.speed    = (float)jce_json_get_number(no, "speed", 1);
            n.size     = (float)jce_json_get_number(no, "size", 0);
            jce_json_get_floats(no, "start_col", n.start_col, 4, NULL);
            jce_json_get_floats(no, "end_col",   n.end_col,   4, NULL);
            jce_json_get_floats(no, "vel_dir",   n.vel_dir,   3, NULL);
            n.vel_mag   = (float)jce_json_get_number(no, "vel_mag", 1);
            n.sub_burst = jce_json_get_int(no, "sub_burst", 8);
            const char *se = jce_json_get_string(no, "sub_event", "death");
            strncpy(n.sub_event, se ? se : "death", sizeof(n.sub_event) - 1);
            n.trail_len   = (float)jce_json_get_number(no, "trail_len", 0);
            n.trail_width = (float)jce_json_get_number(no, "trail_width", 0);
            s_g.nodes.push_back(n);
        }
    }
    JceJson *links = jce_json_get(root, "links");
    if (jce_json_is_array(links)) {
        int ln = jce_json_array_size(links);
        for (int i = 0; i < ln; ++i) {
            JceJson *li = jce_json_array_at(links, i);
            Link L{};
            L.from_node   = jce_json_get_int(li, "fn", 0);
            L.from_socket = jce_json_get_int(li, "fs", 0);
            L.to_node     = jce_json_get_int(li, "tn", 0);
            L.to_socket   = jce_json_get_int(li, "ts", 0);
            s_g.links.push_back(L);
        }
    }
    s_g.next_id = jce_json_get_int(root, "next_id", 100);
    jce_json_free(root);
    s_g.dirty = false;
    jce_editor_console_log("[VFX Graph] loaded %s", p);
}

/* ── Export to .particles.json (JceParticleEmitterDesc key set). ─────
 *
 * Consolidation v0.9.9: the graph compiles onto the canonical key set
 * read by jce_particles_desc_load_json() — emitRate, lifetimeMin/Max,
 * velocityMin/Max, gravity, sizeStart/End, colorStart/End, maxParticles
 * — so the exported document plugs straight into a Particle Emitter
 * component's asset path.  Mappings:
 *   - Emitter.lifetime  -> lifetimeMin == lifetimeMax (single author).
 *   - Emitter.size      -> sizeStart; sizeEnd = 0 (shrink to death).
 *   - Velocity node     -> velocityMin == velocityMax =
 *                          normalize(dir) * (Emitter.speed * mag);
 *                          without the node, +Y * Emitter.speed.
 *   - maxParticles      -> steady-state rate*lifetime + 25% headroom.
 *   - SubEmitter, Trail -> NOT representable; dropped with a warning.
 * Keys the graph does not author (gravity, emitBurst, worldSpace,
 * texture) are omitted so the runtime loader's defaults apply. */

static void graph_compile()
{
    /* Find Output, then walk back through Emitter inputs. */
    Node *out = nullptr;
    for (auto &n : s_g.nodes) if (n.type == NT_OUTPUT) { out = &n; break; }
    if (!out) { jce_editor_console_log_level(JCE_CONSOLE_ERROR, "[VFX Graph] no Output node"); return; }
    int link_to_em = find_input_link(out->id, 0);
    if (link_to_em < 0) { jce_editor_console_log_level(JCE_CONSOLE_ERROR, "[VFX Graph] Output has no Emitter"); return; }
    Node *em = find_node(s_g.links[link_to_em].from_node);
    if (!em || em->type != NT_EMITTER) { jce_editor_console_log_level(JCE_CONSOLE_ERROR, "[VFX Graph] invalid Emitter wiring"); return; }

    JceJson *root = jce_json_object();
    jce_json_set_number(root, "emitRate",    em->rate);
    jce_json_set_number(root, "lifetimeMin", em->lifetime);
    jce_json_set_number(root, "lifetimeMax", em->lifetime);
    jce_json_set_number(root, "sizeStart",   em->size);
    jce_json_set_number(root, "sizeEnd",     0.0);

    /* Pool capacity: enough for the steady-state alive count plus headroom. */
    int max_particles = (int)(em->rate * em->lifetime * 1.25f) + 1;
    if (max_particles < 64) max_particles = 64;
    jce_json_set_number(root, "maxParticles", max_particles);

    /* Initial velocity (min == max: the graph has no spread author). */
    float vel[3] = { 0.0f, em->speed, 0.0f };
    int li_vel = find_input_link(em->id, 2);
    if (li_vel >= 0) {
        Node *v = find_node(s_g.links[li_vel].from_node);
        if (v && v->type == NT_VELOCITY) {
            float len = sqrtf(v->vel_dir[0] * v->vel_dir[0] +
                              v->vel_dir[1] * v->vel_dir[1] +
                              v->vel_dir[2] * v->vel_dir[2]);
            if (len > 1e-6f) {
                float k = (em->speed * v->vel_mag) / len;
                vel[0] = v->vel_dir[0] * k;
                vel[1] = v->vel_dir[1] * k;
                vel[2] = v->vel_dir[2] * k;
            } else {
                jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                    "[VFX Graph] Velocity direction is zero-length; exporting +Y default");
            }
        }
    }
    jce_json_set_float_array(root, "velocityMin", vel, 3);
    jce_json_set_float_array(root, "velocityMax", vel, 3);

    int li_color = find_input_link(em->id, 1);
    if (li_color >= 0) {
        Node *c = find_node(s_g.links[li_color].from_node);
        if (c && c->type == NT_COLOR_RAMP) {
            jce_json_set_float_array(root, "colorStart", c->start_col, 4);
            jce_json_set_float_array(root, "colorEnd",   c->end_col,   4);
        }
    }

    /* Lossy branches: .particles.json cannot express these — warn, drop. */
    if (find_input_link(em->id, 3) >= 0)
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "[VFX Graph] SubEmitter node is not supported by .particles.json — dropped");
    if (find_input_link(em->id, 4) >= 0)
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "[VFX Graph] Trail node is not supported by .particles.json — dropped "
            "(use a Trail Renderer component instead)");

    char p[300]; snprintf(p, sizeof(p), "%s.particles.json", s_g.path[0] ? s_g.path : "untitled");
    ed_write_json_to_file(p, root);
    jce_json_free(root);
    jce_editor_console_log("[VFX Graph] exported -> %s (assign it to a Particle Emitter's asset path)", p);
}

/* ── Drawing ───────────────────────────────────────────────────────── */

static struct {
    int  drag_id   = -1;
    bool linking   = false;
    int  link_from_node = 0, link_from_sock = 0;
} s_state;

static void draw_node(Node &n)
{
    ImGui::PushID(n.id);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 nm = ImVec2(origin.x + n.pos.x, origin.y + n.pos.y);
    ImVec2 sz = ImVec2(180, 26);
    int sc = 0; const Sock *socks = node_sockets(n.type, &sc);
    sz.y += sc * 18.0f + 6.0f;

    ImU32 head = jce_theme::is_light() ? IM_COL32(150, 180, 220, 255)
                                       : IM_COL32(60, 90, 140, 255);
    if (n.type == NT_OUTPUT) head = jce_theme::is_light() ? IM_COL32(220, 160, 140, 255)
                                                          : IM_COL32(140, 80, 60, 255);
    dl->AddRectFilled(nm, ImVec2(nm.x + sz.x, nm.y + sz.y), jce_theme::canvas_bg(), 4.0f);
    dl->AddRect(nm, ImVec2(nm.x + sz.x, nm.y + sz.y), jce_theme::node_outline(), 4.0f);
    dl->AddRectFilled(nm, ImVec2(nm.x + sz.x, nm.y + 22), head, 4.0f);
    dl->AddText(ImVec2(nm.x + 8, nm.y + 4), IM_COL32_WHITE, node_title(n.type));

    /* Drag header. */
    ImGui::SetCursorScreenPos(nm);
    ImGui::InvisibleButton("hdr", ImVec2(sz.x, 22));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
        n.pos.x += ImGui::GetIO().MouseDelta.x;
        n.pos.y += ImGui::GetIO().MouseDelta.y;
        s_g.dirty = true;
    }
    if (ImGui::BeginPopupContextItem("ctx")) {
        if (ImGui::MenuItem(jce_editor_i18n("common.delete"))) {
            int id = n.id;
            for (auto it = s_g.links.begin(); it != s_g.links.end();) {
                if (it->from_node == id || it->to_node == id) it = s_g.links.erase(it); else ++it;
            }
            for (auto it = s_g.nodes.begin(); it != s_g.nodes.end(); ++it)
                if (it->id == id) { s_g.nodes.erase(it); break; }
            ImGui::EndPopup(); ImGui::PopID(); return;
        }
        ImGui::EndPopup();
    }

    /* Sockets. */
    for (int i = 0; i < sc; ++i) {
        float y = nm.y + 28.0f + i * 18.0f;
        bool out_pin = (socks[i].kind == SK_OUT);
        float px = out_pin ? (nm.x + sz.x - 6) : (nm.x + 6);
        dl->AddCircleFilled(ImVec2(px, y + 6), 5.0f,
                            out_pin ? IM_COL32(220, 200, 80, 255) : IM_COL32(80, 200, 220, 255));
        ImVec2 lp = out_pin ? ImVec2(nm.x + sz.x - 60, y) : ImVec2(nm.x + 14, y);
        dl->AddText(lp, IM_COL32_WHITE, socks[i].name);
        ImGui::SetCursorScreenPos(ImVec2(px - 6, y));
        ImGui::PushID(i);
        ImGui::InvisibleButton("pin", ImVec2(12, 12));
        if (ImGui::IsItemClicked(0)) {
            if (!s_state.linking && out_pin) {
                s_state.linking = true;
                s_state.link_from_node = n.id;
                s_state.link_from_sock = i;
            } else if (s_state.linking && !out_pin) {
                /* Remove existing link to this input. */
                int ex = find_input_link(n.id, i);
                if (ex >= 0) s_g.links.erase(s_g.links.begin() + ex);
                Link L{}; L.from_node = s_state.link_from_node; L.from_socket = s_state.link_from_sock;
                L.to_node = n.id; L.to_socket = i;
                s_g.links.push_back(L);
                s_state.linking = false;
                s_g.dirty = true;
            }
        }
        ImGui::PopID();
    }

    /* Inline payload editors (compact). */
    ImGui::SetCursorScreenPos(ImVec2(nm.x + 6, nm.y + sz.y + 4));
    ImGui::PushItemWidth(sz.x - 12);
    if (n.type == NT_EMITTER) {
        if (ImGui::DragFloat("rate",     &n.rate,     0.5f, 0, 1000)) s_g.dirty = true;
        if (ImGui::DragFloat("lifetime", &n.lifetime, 0.05f, 0, 60))  s_g.dirty = true;
        if (ImGui::DragFloat("speed",    &n.speed,    0.05f, 0, 100)) s_g.dirty = true;
        if (ImGui::DragFloat("size",     &n.size,     0.01f, 0, 10))  s_g.dirty = true;
    } else if (n.type == NT_COLOR_RAMP) {
        if (ImGui::ColorEdit4("start", n.start_col)) s_g.dirty = true;
        if (ImGui::ColorEdit4("end",   n.end_col))   s_g.dirty = true;
    } else if (n.type == NT_VELOCITY) {
        if (ImGui::DragFloat3("dir", n.vel_dir, 0.05f, -1, 1)) s_g.dirty = true;
        if (ImGui::DragFloat("mag",  &n.vel_mag, 0.05f, 0, 100)) s_g.dirty = true;
    } else if (n.type == NT_SUB_EMITTER) {
        if (ImGui::DragInt("burst", &n.sub_burst, 1, 0, 1000)) s_g.dirty = true;
        if (ImGui::InputText("event", n.sub_event, sizeof(n.sub_event))) s_g.dirty = true;
    } else if (n.type == NT_TRAIL) {
        if (ImGui::DragFloat("len",   &n.trail_len,   0.01f, 0, 10)) s_g.dirty = true;
        if (ImGui::DragFloat("width", &n.trail_width, 0.005f, 0, 5)) s_g.dirty = true;
    }
    ImGui::PopItemWidth();

    ImGui::PopID();
}

static void draw_links(ImVec2 origin)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (const auto &L : s_g.links) {
        Node *a = find_node(L.from_node);
        Node *b = find_node(L.to_node);
        if (!a || !b) continue;
        int ac = 0, bc = 0;
        node_sockets(a->type, &ac);
        node_sockets(b->type, &bc);
        ImVec2 p1(origin.x + a->pos.x + 180 - 6, origin.y + a->pos.y + 28 + L.from_socket * 18 + 6);
        ImVec2 p2(origin.x + b->pos.x + 6,       origin.y + b->pos.y + 28 + L.to_socket   * 18 + 6);
        ImVec2 c1(p1.x + 60, p1.y), c2(p2.x - 60, p2.y);
        dl->AddBezierCubic(p1, c1, c2, p2, IM_COL32(220, 200, 80, 220), 2.0f);
    }
}

} /* anonymous namespace */

extern "C" void jce_editor_panel_vfx_graph_content(void)
{
    graph_init_default();

    ImGui::TextUnformatted(jce_editor_i18n("vfxGraph.path"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-220);
    if (jce_draw_path_input("##vfxpath", s_g.path, sizeof(s_g.path), JcePathKind::FileAbs)) {}
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("vfxGraph.save")))    graph_save();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("vfxGraph.load")))    graph_load();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("vfxGraph.export"))) graph_compile();

    ImGui::Separator();
    ImGui::Text("%s: %d %s   %s: %d %s   %s",
                jce_editor_i18n("vfxGraph.nodeCount"), (int)s_g.nodes.size(), "",
                jce_editor_i18n("vfxGraph.linkCount"), (int)s_g.links.size(), "",
                s_g.dirty ? "*" : "");

    ImVec2 canvas_origin = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    if (canvas_size.y < 200) canvas_size.y = 200;
    ImGui::BeginChild("##vfxcanvas", canvas_size, true,
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 origin = ImGui::GetCursorScreenPos();

    /* Background grid. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 ws = ImGui::GetWindowSize();
    for (float x = 0; x < ws.x; x += 32)
        dl->AddLine(ImVec2(wp.x + x, wp.y), ImVec2(wp.x + x, wp.y + ws.y), jce_theme::grid_minor());
    for (float y = 0; y < ws.y; y += 32)
        dl->AddLine(ImVec2(wp.x, wp.y + y), ImVec2(wp.x + ws.x, wp.y + y), jce_theme::grid_minor());

    draw_links(origin);
    for (auto &n : s_g.nodes) draw_node(n);

    /* Pending link rubber-band. */
    if (s_state.linking) {
        Node *a = find_node(s_state.link_from_node);
        if (a) {
            ImVec2 p1(origin.x + a->pos.x + 180 - 6, origin.y + a->pos.y + 28 + s_state.link_from_sock * 18 + 6);
            ImVec2 p2 = ImGui::GetIO().MousePos;
            dl->AddBezierCubic(p1, ImVec2(p1.x + 60, p1.y), ImVec2(p2.x - 60, p2.y), p2, IM_COL32(255, 255, 100, 200), 2.0f);
        }
        if (ImGui::IsMouseClicked(1)) s_state.linking = false;
    }

    /* Canvas right-click add menu. */
    ImGui::SetCursorScreenPos(canvas_origin);
    ImGui::InvisibleButton("##bg", canvas_size);
    if (ImGui::BeginPopupContextItem("##addmenu")) {
        ImVec2 mp = ImGui::GetMousePosOnOpeningCurrentPopup();
        ImVec2 lp(mp.x - origin.x, mp.y - origin.y);
        const struct { const char *n; NType t; } kAdd[] = {
            {"Emitter",     NT_EMITTER},
            {"ColorRamp",   NT_COLOR_RAMP},
            {"Velocity",    NT_VELOCITY},
            {"SubEmitter",  NT_SUB_EMITTER},
            {"Trail",       NT_TRAIL},
            {"Output",      NT_OUTPUT},
        };
        for (size_t i = 0; i < sizeof(kAdd)/sizeof(kAdd[0]); ++i) {
            if (ImGui::MenuItem(kAdd[i].n)) {
                Node n{}; n.id = s_g.next_id++; n.type = kAdd[i].t; n.pos = lp;
                s_g.nodes.push_back(n);
                s_g.dirty = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
}

/* Shim: VFX Graph has been merged into the Material Graph
 * "Graph Authoring" workbench as a tab.  Activating this panel now
 * redirects to that workbench and requests the VFX tab.  Symbol kept
 * so menu/hotkey entries registered against JCE_PANEL_VFX_GRAPH keep
 * working. */
extern "C" void jce_editor_panel_vfx_graph(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_VFX_GRAPH);
    if (!vis || !*vis) return;
    *vis = false;

    bool *mg_vis = jce_editor_panel_visible_ptr(JCE_PANEL_MATERIAL_GRAPH);
    if (mg_vis) *mg_vis = true;

    char title[96];
    snprintf(title, sizeof(title), "%s###jce_material_graph",
             jce_editor_i18n("materialGraph.title"));
    ImGui::SetWindowFocus(title);
    jce_panel_material_graph_request_tab(2);
}
