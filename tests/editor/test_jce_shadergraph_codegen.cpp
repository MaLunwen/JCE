/*
 * test_jce_shadergraph_codegen.cpp -- what the graph actually generates.
 *
 * The three defects this covers were all the same shape: a node that existed,
 * whose GLSL was correct, and whose result NOTHING could consume.  None of
 * them failed loudly, and none of them is visible in the editor -- the node
 * sits on the canvas, the link snaps into place, and the generated shader
 * quietly says something else.  The generated TEXT is the artefact, so the
 * text is what is asserted:
 *
 *   the Normal socket has a reader ... codegen emitted `vec3 mat_normal_ts =
 *                                     vec3(0,0,1);` unconditionally, with no
 *                                     case that ever read a link.  The Normal
 *                                     Map node sampled s_normalMap perfectly
 *                                     and had nowhere to put the answer.
 *   an unconnected Normal is (0,0,1) . the identity tangent-space normal, so
 *                                     a graph that does not drive it shades
 *                                     exactly as it did before the socket
 *                                     existed.
 *   the UV node is a vec2 .......... it was a single FLOAT called "Tile",
 *                                     emitted as a bare constant, and no node
 *                                     had a UV input to consume it.
 *   an unconnected UV is mat_uv .... not vec2(0,0).  Every Texture node
 *                                     hard-coded mat_uv before the socket
 *                                     existed; a zero default would turn an
 *                                     untouched graph into one that samples a
 *                                     single texel.
 *   the texture SLOT is per node ... Node::text was serialised, reloaded, and
 *                                     read by nothing: every Texture node
 *                                     emitted the literal `s_albedo`, so two
 *                                     nodes aimed at different maps sampled
 *                                     the same one.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "shadergraph/jce_shadergraph_codegen.h"
#include "shadergraph/jce_shadergraph_graph.h"
#include "shadergraph/jce_shadergraph_registry.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace jce_sg;

namespace {

/* The template path arrives as a DEFINE from CMake.  A relative-path search
 * would be a test that passes or fails on where ctest was launched from. */
#ifndef JCE_SG_TEMPLATE
#error "JCE_SG_TEMPLATE must be defined by the build"
#endif
const char *template_path()
{
    std::FILE *f = std::fopen(JCE_SG_TEMPLATE, "rb");
    if (!f) return nullptr;
    std::fclose(f);
    return JCE_SG_TEMPLATE;
}

int add(Graph &g, NodeType t)
{
    Node n;
    n.id = g.next_id++;
    n.type = t;
    g.nodes.push_back(n);
    return n.id;
}

Node *node(Graph &g, int id)
{
    for (Node &n : g.nodes) if (n.id == id) return &n;
    return nullptr;
}

void link(Graph &g, int from, int fsock, int to, int tsock)
{
    Link l;
    l.from_node = from; l.from_sock = fsock;
    l.to_node = to;     l.to_sock = tsock;
    g.links.push_back(l);
}

/* Strip comments before searching.
 *
 * The first version of the TBN assertion looked for "mat_normal_ts * 0.0001"
 * and found it -- in the COMMENT that explains what that line used to be.
 * A substring search over source is satisfied by prose about the source,
 * which is the same way a check for a symbol name has been satisfied by an
 * include line in this tree before.  Search the CODE. */
std::string code_only(const std::string &src)
{
    std::string out;
    for (size_t i = 0; i < src.size(); ) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            size_t e = src.find("*/", i + 2);
            i = (e == std::string::npos) ? src.size() : e + 2;
            out += ' ';
        } else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            size_t e = src.find('\n', i);
            i = (e == std::string::npos) ? src.size() : e;
        } else {
            out += src[i++];
        }
    }
    return out;
}

/* The PBR Output's socket indices, in registry order. */
enum { OUT_BASECOLOR = 0, OUT_METALLIC, OUT_ROUGHNESS, OUT_EMISSIVE, OUT_NORMAL };

std::string gen(Graph &g)
{
    const char *tpl = template_path();
    REQUIRE(tpl != nullptr);
    CodegenResult r = codegen(g, tpl, "test_graph", nullptr);
    if (!r.ok) {
        for (const CodegenDiag &d : r.errors)
            MESSAGE("codegen error: " << d.message);
    }
    REQUIRE(r.ok);
    /* Comments stripped: every assertion below is about the CODE. */
    return code_only(r.source);
}

} /* namespace */

TEST_CASE("the PBR Output has a Normal socket and codegen reads it")
{
    /* The socket has to EXIST before anything can be connected to it, and its
     * type has to match the Normal Map node's output exactly -- the type
     * checker requires equality, so a vec4-wrapped normal would have been
     * unconnectable even after the socket appeared. */
    const NodeMeta *om = meta_for(NT_OUTPUT);
    REQUIRE(om != nullptr);
    REQUIRE(om->socket_count == 5);
    CHECK(std::strcmp(om->sockets[OUT_NORMAL].name, "Normal") == 0);
    CHECK(om->sockets[OUT_NORMAL].dtype == DT_VEC3);

    const NodeMeta *nm = meta_for(NT_NORMAL_MAP);
    REQUIRE(nm != nullptr);
    bool found_out = false;
    for (int i = 0; i < nm->socket_count; ++i)
        if (nm->sockets[i].kind == SK_OUTPUT) {
            found_out = true;
            CHECK(nm->sockets[i].dtype == om->sockets[OUT_NORMAL].dtype);
        }
    CHECK(found_out);
}

TEST_CASE("an unconnected Normal stays the identity, a connected one is read")
{
    Graph g;
    int out = add(g, NT_OUTPUT);
    std::string src = gen(g);
    CHECK(src.find("vec3 mat_normal_ts = vec3(0.0, 0.0, 1.0);") != std::string::npos);

    int nmap = add(g, NT_NORMAL_MAP);
    link(g, nmap, /*sock*/1, out, OUT_NORMAL);   /* socket 1 is the output */
    src = gen(g);
    char want[64];
    std::snprintf(want, sizeof want, "vec3 mat_normal_ts = n%d_o1;", nmap);
    INFO("generated:\n" << src);
    CHECK(src.find(want) != std::string::npos);
    /* And the node itself has to have been emitted, sampling the normal map. */
    CHECK(src.find("texture2D(s_normalMap,") != std::string::npos);
}

TEST_CASE("the UV node is a vec2 and a Texture node samples with it")
{
    const NodeMeta *um = meta_for(NT_UV);
    REQUIRE(um != nullptr);
    REQUIRE(um->socket_count == 1);
    CHECK(um->sockets[0].dtype == DT_VEC2);

    Graph g;
    int out = add(g, NT_OUTPUT);
    int tex = add(g, NT_TEXTURE);
    link(g, tex, /*sock*/1, out, OUT_BASECOLOR);   /* socket 1 = RGBA out */

    /* Unconnected UV: the interpolated coordinate, not vec2(0,0). */
    std::string src = gen(g);
    INFO("generated:\n" << src);
    CHECK(src.find("texture2D(s_albedo, mat_uv)") != std::string::npos);
    CHECK(src.find("vec2_splat(0.0)") == std::string::npos);

    /* Connected: tile 3 x 2, offset 0.25 x 0.5. */
    int uv = add(g, NT_UV);
    Node *un = node(g, uv);
    REQUIRE(un != nullptr);
    un->color[0] = 3.0f;  un->color[1] = 2.0f;
    un->color[2] = 0.25f; un->color[3] = 0.5f;
    link(g, uv, 0, tex, 0);
    src = gen(g);
    INFO("generated:\n" << src);
    char want[128];
    std::snprintf(want, sizeof want,
                  "vec2 n%d_o0 = mat_uv * vec2(3.000000, 2.000000) + "
                  "vec2(0.250000, 0.500000);", uv);
    CHECK(src.find(want) != std::string::npos);
    std::snprintf(want, sizeof want, "texture2D(s_albedo, n%d_o0)", uv);
    CHECK(src.find(want) != std::string::npos);
}

TEST_CASE("two Texture nodes on different maps sample different samplers")
{
    Graph g;
    int out = add(g, NT_OUTPUT);
    int a = add(g, NT_TEXTURE);
    int b = add(g, NT_TEXTURE);
    std::snprintf(node(g, a)->text, 256, "%s", "albedo");
    std::snprintf(node(g, b)->text, 256, "%s", "emissive");
    link(g, a, 1, out, OUT_BASECOLOR);
    link(g, b, 1, out, OUT_EMISSIVE);

    std::string src = gen(g);
    INFO("generated:\n" << src);
    CHECK(src.find("texture2D(s_albedo,")   != std::string::npos);
    CHECK(src.find("texture2D(s_emissive,") != std::string::npos);

    /* Every slot name the canvas offers must reach a distinct sampler; an
     * unrecognised one falls back to albedo, which is what every graph
     * authored before the picker existed did. */
    static const char *kSlot[5] =
        { "albedo", "metalRough", "normalMap", "ao", "emissive" };
    static const char *kSampler[5] =
        { "s_albedo", "s_metalRough", "s_normalMap", "s_ao", "s_emissive" };
    for (int i = 0; i < 5; ++i) {
        Graph one;
        int o = add(one, NT_OUTPUT);
        int t = add(one, NT_TEXTURE);
        std::snprintf(node(one, t)->text, 256, "%s", kSlot[i]);
        link(one, t, 1, o, OUT_BASECOLOR);
        std::string s1 = gen(one);
        char want[64];
        std::snprintf(want, sizeof want, "texture2D(%s,", kSampler[i]);
        INFO("slot " << kSlot[i] << " generated:\n" << s1);
        CHECK(s1.find(want) != std::string::npos);
    }
}

TEST_CASE("the tangent-space normal reaches the lighting, and so does the rest")
{
    /* The generated shader is only half the story: something has to CONSUME
     * mat_normal_ts.  The template used to spend it on `v_normal +
     * mat_normal_ts * 0.0001` -- a reference that keeps the variable alive and
     * moves the shading normal by about a thousandth of a degree, so a graph
     * could have driven it perfectly and the picture would not have changed.
     *
     * The template then applied a TBN basis of its own, which was correct and
     * is now gone for a better reason: the graph hands its five values to
     * fs_pbr_main.sh, the SAME lighting fs_pbr.sc and fs_pbr_fwdplus.sc use,
     * and that file already has a TBN.  So what this case guards moved with
     * it -- from "the template applies the normal" to "the template hands the
     * normal to the shader that applies it, and asks for that shader". */
    Graph g;
    add(g, NT_OUTPUT);
    std::string src = gen(g);
    INFO("generated:\n" << src);
    CHECK(src.find("mat_normal_ts * 0.0001") == std::string::npos);

    /* All five outputs are handed over, not just the ones a graph happens to
     * drive: an output the template drops is a socket that silently does
     * nothing, which is the defect this whole family keeps having. */
    CHECK(src.find("out_normal_ts  = mat_normal_ts;")  != std::string::npos);
    CHECK(src.find("out_base_color = mat_base_color;") != std::string::npos);
    CHECK(src.find("out_metallic   = mat_metallic;")   != std::string::npos);
    CHECK(src.find("out_roughness  = mat_roughness;")  != std::string::npos);
    CHECK(src.find("out_emissive   = mat_emissive;")   != std::string::npos);

    /* And it asks for the shared lighting rather than carrying its own.  The
     * define is what switches fs_pbr_main.sh from fetching the material out of
     * textures to taking the graph's five values; without it the include
     * compiles the stock material path and the graph is inert. */
    CHECK(src.find("#define JCE_GRAPH_MATERIAL") != std::string::npos);
    CHECK(src.find("fs_pbr_decl.sh") != std::string::npos);
    CHECK(src.find("fs_pbr_main.sh") != std::string::npos);

    /* The varyings the lighting needs have to be declared, or none of it
     * compiles -- and the failure would land in shaderc, in the editor, on
     * somebody else's afternoon.  fs_pbr_main.sh reads all seven vs_pbr.sc
     * writes, and a SUBSET does not merely lose a value: bgfx links a program
     * by comparing the two varying hashes, so a short list fails to link. */
    for (const char *v : { "v_texcoord0", "v_worldpos", "v_normal", "v_tangent",
                           "v_bitangent", "v_viewdepth", "v_localpos" })
        CHECK(src.find(v) != std::string::npos);
}

TEST_CASE("every node in the palette emits code for every one of its outputs")
{
    /* THE PALETTE IS A PROMISE.  A type in addable_types() is one the user can
     * drop on the canvas; if its snippet is missing, misspelt, or produces
     * nothing for a second output socket, the node is in the menu and does
     * nothing -- which is the exact shape this feature kept failing in (a
     * Normal Map node with no destination, a Fresnel with no N and no V).
     *
     * This walks the whole palette rather than a chosen few: it wires one of
     * each into the PBR Output so codegen cannot prune it, and requires a
     * declaration line for EACH output socket the registry says it has.  Split
     * has four; a snippet without $O would silently declare the same
     * expression four times, and this case would still pass -- which is why
     * the all-node graph is compiled by shaderc separately.  What this guards
     * is the cheaper half: that nothing in the menu emits nothing. */
    int n_addable = 0;
    const NodeType *addable = jce_sg::addable_types(&n_addable);
    REQUIRE(addable != nullptr);
    REQUIRE(n_addable > 0);

    for (int i = 0; i < n_addable; ++i) {
        const NodeType t = addable[i];
        const jce_sg::NodeMeta *meta = jce_sg::meta_for(t);
        INFO("node type " << (int)t);
        REQUIRE(meta != nullptr);
        REQUIRE(meta->label != nullptr);

        /* Find its first output socket and its dtype. */
        int   out_sock = -1;
        for (int s = 0; s < meta->socket_count; ++s)
            if (meta->sockets[s].kind == SK_OUTPUT) { out_sock = s; break; }
        INFO("node " << std::string(meta->label) << " has no output socket");
        REQUIRE(out_sock >= 0);

        /* Wire it into whichever PBR Output input takes its dtype, so the
         * topological sort keeps it. */
        Graph g;
        int out = add(g, NT_OUTPUT);
        int n   = add(g, t);
        const DataType dt = meta->sockets[out_sock].dtype;
        int dst_sock = (dt == DT_FLOAT) ? 1        /* Metallic  */
                     : (dt == DT_VEC3)  ? 4        /* Normal    */
                                        : 0;       /* BaseColor */
        if (dt == DT_VEC2) continue;   /* no vec2 input on the output node */
        g.links.push_back(Link{ n, out_sock, out, dst_sock });

        std::string src = gen(g);
        INFO("node " << meta->label << " generated:\n" << src);

        /* One declaration per output socket, named n<id>_o<socket>. */
        for (int s = 0; s < meta->socket_count; ++s) {
            if (meta->sockets[s].kind != SK_OUTPUT) continue;
            char want[64];
            std::snprintf(want, sizeof(want), " n%d_o%d = ", n, s);
                INFO("node " << std::string(meta->label)
                 << " emitted nothing for output socket " << s);
            CHECK(src.find(want) != std::string::npos);
        }
    }
}
