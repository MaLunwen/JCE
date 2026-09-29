# editor/src/shadergraph/ — Shader Graph Model

可复用的节点图数据模型层。**仅 model + 序列化 + 节点元信息表**，零 ImGui 依赖。

## 范围与边界

- ✅ 数据结构：`JceShaderGraph` / `JceShaderNode` / `JceShaderLink` / `JceShaderSocket`
- ✅ 节点元信息查表（label / sockets / 类型 / codegen snippet 占位）
- ✅ 序列化 ↔ `.matgraph.json`（向后兼容现有面板写出的格式）
- ✅ 纯模型操作：find/connect/disconnect/topo
- ❌ 不渲染 UI（panel 层负责）
- ❌ 不调用 shaderc / bgfx（Phase D）
- ❌ 不生成 `.sc` 源码（Phase C）

## 文件

| 文件 | 内容 |
|---|---|
| `jce_shadergraph_types.h` | 枚举 + struct 定义 |
| `jce_shadergraph_registry.h/cpp` | `JceShaderNodeMeta` 表（含 `glsl_snippet`） |
| `jce_shadergraph_graph.h/cpp` | 纯模型操作 |
| `jce_shadergraph_io.h/cpp` | save/load/import |
| `jce_shadergraph_topo.h/cpp` | 反向 DFS 拓扑 + 环检测（Phase C） |
| `jce_shadergraph_typecheck.h/cpp` | dtype / arity / 重复输入校验（Phase C） |
| `jce_shadergraph_codegen.h/cpp` | Graph → GLSL 片段 + 模板 splice → `fs_<name>.sc`（Phase C） |

## 阶段路线图（P2-②）

- **A**: 抽 model 层 — done
- B: UI 重构（imnodes 决策点） — done
- **C**（当前）: Graph → `.sc` 源码 codegen — 通过 `jce_sg::codegen()` 入口
- D: shaderc 集成 + Material 热替换 + preview sphere
- E: Inspector + Asset Browser workflow

## 不变量

- `.matgraph.json` schema 永远向后兼容（新字段写入用 default，读取时缺失字段用 default 容错）
- model 头文件不能 include `imgui.h` / `bgfx.h`
- 节点元信息**完全声明式**（不写命令式 switch — Phase C/D 新增节点只改表）

## 关联模块

- `editor/src/panels/jce_panel_material_graph.cpp` — UI 消费者
- `engine/src/renderer/jce_pbr_material.{c,h}` — `.mat.json` 编译目标（material-graph 专属）
- `tools/compile_shaders.cmake` — Phase D 联动
