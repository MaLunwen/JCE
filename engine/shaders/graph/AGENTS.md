# engine/shaders/graph — Shader Graph Codegen Templates

Templates consumed by `editor/src/shadergraph/jce_shadergraph_codegen.cpp`
(P2-② Phase C). **NOT** registered with `jce_compile_shaders()` — these are
codegen inputs, not runtime shaders.

## Files

| File | Purpose |
|---|---|
| `fs_graph_template.sc`  | Fragment-shader template. Contains a Lambert + ambient lighting body that consumes 5 `mat_*` locals. Codegen splices the graph-derived material body between `/*JCE_BEGIN_MATERIAL*/` and `/*JCE_END_MATERIAL*/`. Pairs with `engine/shaders/pbr/vs_pbr.sc` at link time (subset of vs_pbr outputs). |

## Varying

The template declares `$input v_texcoord0, v_worldpos, v_normal` — a
subset of `engine/shaders/pbr/varying_pbr.def.sc`.  Shader graph fragment
shaders are compiled with `--varyingdef engine/shaders/pbr/varying_pbr.def.sc`
so they can link against the existing `vs_pbr` program at runtime
(Phase D).  This avoids generating + compiling a parallel vertex shader.

## Contract

Codegen must populate **all five** locals between the hook markers:

```glsl
vec4  mat_base_color;
vec3  mat_normal_ts;   // tangent-space normal; default vec3(0,0,1)
float mat_metallic;
float mat_roughness;   // clamped to [0.04, 1.0] by post-hook code
vec3  mat_emissive;
```

## Path resolution at runtime

The editor finds the template via (first hit wins):

1. `$JCE_SHADER_TEMPLATE` — absolute path
2. `$JCE_SHADER_DEV_DIR/shaders/graph/fs_graph_template.sc`
3. `./engine/shaders/graph/fs_graph_template.sc` (in-tree dev fallback)

## Roadmap

- Phase C (current): template + codegen MVP — manual `shaderc` invocation
- Phase D: editor-driven `shaderc` compile + Material hot-rebind
