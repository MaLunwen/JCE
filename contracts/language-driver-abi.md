# JCE 多语言驱动 C ABI 设计

> 计划:`.docs/way/JCE_DEPENDENCY_MULTILANGUAGE_CODE_AUDIT_PLAN_STRICT.md` §4.4 / §20–26
> 状态:描述**现状 ABI 契约**(经 2026-07-24 审计核实)+ 规范化规则。分「已成立」与「待补齐」。
> 权威事实来源:`engine/include/jce/**`(3014 个 `JCE_API` 函数)。

JCE 的公共边界是一套**扁平、稳定、可被任意语言调用的 C ABI**。C 是唯一底层事实来源;
Lua、C++、Java/JNI 及未来语言(C#/Rust/Python/Swift/Kotlin)都是它的**消费者**,不是核心所有者。
"无损兼容"的七维定义(功能/数据/生命周期/错误/线程/版本/性能)见计划 §20,本文档给出落地契约。

---

## 1. C ABI 类型规则(计划 §21.1)

**允许出现在公共头的类型**:
- 固定宽度整数(`uint8/16/32/64_t`、`int*_t`)、`float`/`double`、`bool`(见 §1.1 注意)。
- JCE 自有 POD 结构体(仅上述标量 + 嵌套 POD)。
- 不透明句柄(`typedef struct JceX JceX;` 的前置声明 + 指针)。
- UTF-8 字符串视图(`const char*` + 可选显式长度)。
- 明确长度的数组 / Blob(`const T*` + `size_t/uint64_t count`)。
- C 函数指针回调 + `void* userdata`。
- 稳定返回类型(`bool`、句柄、域枚举 —— 注意**不存在**统一的 `JceResult` 家族,见 §5)。

**禁止**(经 §5 类型级门禁 `check_public_abi.py` 强制):
- 任何第三方类型名:`ecs_world_t`(flecs)、`bgfx_*`、`bt*`/`b2*`、`ma_*`、`ENet*`、`lua_State`、
  `JNIEnv`、`FT_*`/`hb_*`、`PHYSFS_*`、`cJSON`、`Rml*`、`ozz::`、`dt*`/`rc*`。
- C++ 构造:`std::`、`namespace`、`template`、`class`。
- 非可移植平台类型:`HWND`/`HANDLE`/`Vk*`/`ID3D*`/`NSWindow`/`CAMetalLayer`。

**审计已修复的历史泄漏**:flecs `ecs_world_t*`(4 个 `*_ecs.h` 适配器头)→ 不透明 `void*`;
bgfx `bgfx_memory_s*`(`jce_views.h`)→ 下沉 `jce_renderer_internal.h`。类型级门禁现为绿。

### 1.1 已知需补齐(P2 系统性)
- **`bool` 在 ABI 结构体**:已量化 —— **480 个公共结构体中 163 个含 bool 字段,共 326 个**。C 未规定 `_Bool`
  宽度(主流 ABI 均为 1B),但 **C# P/Invoke 的 `bool` 默认为 4 字节** —— 猜错即污染该字段之后的全部偏移。
  **不做全量替换**(改 326 个字段=对每个消费者的破坏性变更且收益仅为"更显式"),改为**用编译期断言钉住**:
  `tests/os/core/test_jce_abi_layout.c` 中 `sizeof(bool)==1` 不成立即**编译失败**(负例已验证)。
  函数返回 `bool` 始终可接受(寄存器传递,无布局影响)。
- **enum 底层宽度**:已量化 —— 158 个公共 enum 中 **63 个被用作结构体字段**(其余仅作返回值/参数,无布局影响)。
  `-fshort-enums` 之类的编译选项会**一次性改变这 63 个结构体的尺寸,且没有任何源码 diff** 供评审或 ABI 快照发现。
  同样用编译期断言钉住(见上)。
  **唯一异常已查明**:`JcePhysicsDebugFlag` 含 `JCE_PHYS_DBG_ALL = 0xFFFFFFFFu`,超出 int 范围 → 底层类型为
  **无符号**,而其余全部有符号。**今天无布局影响**(公共 API 收 `uint32_t` 而非该 enum 类型),但把所有 JCE enum
  映射为有符号 int 的 FFI 生成器会为这一个常量产出错值。已在测试中钉住,避免被重新"发现"。
- **结构体版本化**:见下方 §1.2 —— 已从"建议"变为**已量化 + 已给出落地范式**。

---

### 1.2 输入 Desc 的"短结构体越界读" —— 已量化 + 范式已落地

**缺陷形态**:公共入口取 `const JceX *`,实现里 `dst = *src;` 或 `memcpy(..., sizeof(JceX))` ——
拷贝长度是**引擎侧的 sizeof**。用**旧 SDK 头**编译的消费者分配的是更小的结构体,于是引擎
**读过调用方对象的末尾**,尾部字段拿到的是内存里紧随其后的任意字节。

**这不是假想**:`JceAppDesc` 已增长两次(`window_width/window_height`,而后 `headless`)。
本次审计的无头服务器工作刚加了 `headless`,尚未取新头的消费者交来的结构体**就在该字段之前结束**
—— 而 `headless` 被随机置真的后果是游戏启动后无窗口、无 GPU 设备、无音频。SDK 的存在意义正是
分发给不重建引擎的人,所以**头文件版本错位是常态,不是边角**。

**落地范式**(已在 `jce_engine_set_app_desc` 实施,4fa6a957):

```c
/* 引擎侧:零填充后拷贝 min(调用方, 引擎) */
JCE_API void jce_engine_set_app_desc_sized(const JceAppDesc *desc, size_t desc_size);
/* 旧符号保留:已编译二进制不断链;引擎内部调用方本就同步 */
JCE_API void jce_engine_set_app_desc(const JceAppDesc *desc);
/* 头文件 shim:消费者重新编译即自动获得修复,无需改源码 */
#if !defined(JCE_BUILDING_ENGINE) && !defined(JCE_NO_APP_DESC_SIZE_SHIM)
#  define jce_engine_set_app_desc(desc)        jce_engine_set_app_desc_sized((desc), sizeof(JceAppDesc))
#endif
```

短结构体 → 未知字段保持文档默认值;长结构体(消费者比引擎新)→ 忽略未知尾部。

**同型清单(14 处 / 108 个描述符类公共入口)**:

  - `jce_archive_writer_create` — `JceArchiveWriterConfig`
  - `jce_audio_mixer_set_sidechain` — `JceAudioDuckParams`
  - `jce_audio_occlusion_tracker_set_params` — `JceAudioOcclusionParams`
  - `jce_light_cluster_create` — `JceLightClusterDesc`
  - `jce_nav_agent_set_avoidance` — `JceNavAvoidanceConfig`
  - `jce_postfx_set_params` — `JcePostFXParams`
  - `jce_rpc_register` — `JceRpcDesc`
  - `jce_scene_set_rendering_settings` — `JceSceneRenderingSettings`
  - `jce_spawn_manager_create` — `JceSpawnManagerDesc`
  - `jce_ssao_set_params` — `JceSsaoParams`
  - `jce_streaming_create` — `JceStreamingConfig`
  - `jce_trigger_add` / `jce_trigger_set_desc` — `JceTriggerDesc`
  - `jce_world_streamer_create` — `JceWorldStreamConfig`
  - `jce_script_create` — **`JceScriptHost`**(2026-07-25 补录)

> **对上述清单的更正**:`JceScriptHost` 起初**不在**表中,因为生成它的扫描按名字过滤
> (`…Desc/Config/Params/Options/Settings`),而 `Host` 不在其中。它同样是**消费者填写、
> 引擎整体拷贝**的结构体(`jce_script.c:567` 的 `memcpy(&s->host, host, n);` —— 2026-08-15 之前是
> 整体 `s->host = *host;`,短结构体 ABI 修复后改为 `min(caller, engine)` 拷贝),因此风险完全相同 ——
> 按名字筛选漏掉了一个真实条目,这条方法缺陷值得记下来。

> **且它比其余更脆**:游戏提供的是一张**函数指针表**。在中间插入成员会让其后每个槽位错位,
> 于是用旧头编译的游戏会让引擎**经错误的槽位调用函数指针** —— 不是读到垃圾数据,而是
> **跳转到错误的函数**。新成员必须**追加到末尾**,永远不要插在中间。

**为何不预先把这 14 处全改成 `_sized`**:潜在风险只在结构体**实际增长时**才兑现,而
`check_abi_snapshot.py` 已把每个公共结构体的字段列表冻结 —— **任何一个增长都会被判
CHANGED 并让门变红**,作者必须显式面对。预先翻倍 14 个 API 表面,是为一个门禁已经拦住的风险付
出确定的复杂度代价。

**因此规则是**:当 ABI 门因上表中某个结构体变更而变红时,**不要只是 `--update` 快照** ——
先判断该入口是否需要 `_sized` 变体;若需要,与结构体变更同一提交落地。`JceAppDesc` 即为范例。

---

## 2. 版本握手(计划 §21 / §35.4)

- `jce_api_version()` 返回打包 `uint32_t`(major<<24 | minor<<16 | patch),`jce_api_version_string()` 返回字符串。
- `jce_compat.h` 在编译期断言语言基线(C99 / C++20)、平台最低版本(Win7/macOS10.13/iOS12/Android21/glibc2.28/wasm+simd128)、
  SIMD 保证(SSE2/NEON/wasm-SIMD)、64 位指针。FFI 工具可 include 它做环境断言。
- **契约**:绑定编译期记录头版本,运行期比对 `jce_api_version()` major;不匹配应显式失败,不静默运行。
  `tests/sdk_smoke`(纯 C99)即以此为回归门。
- **更正**:原记「JNI 侧无版本握手(`JNI-04`)」—— **不成立**。`JceRuntime.java` 已有
  `EXPECTED_API_VERSION` 与 `nativeApiVersion()` 的 major/minor 比对并在不匹配时显式失败。
- **真实缺陷是那个常量为手写镜像**(其注释自称"与 `project(JCE VERSION ...)` 同步 bump")。
  漂移后果特别刁钻:握手会**拒绝一对本来正确配对的引擎与绑定**,报错信息把人引向错误方向。
  已机械化:`check_binding_parity.py --version-only` 解析根 `CMakeLists.txt` 的 `project(JCE VERSION)`
  打包成 `0xMMmmpp00` 与 Java 常量比对,**入 gating 门**(负例验证:改一个 patch 位即红,退出码 1)。
  parity **矩阵**仍为 report-only —— 覆盖度是判断题,版本镜像是事实题,两者门禁级别不同。
- **仍待补**:Lua 侧无版本握手(`jce_script.c` 属他人在飞文件,未动)。

---

## 3. 句柄与生命周期(计划 §21.2)

- **实体**:`typedef uint64_t JceEntity`(`jce_scene.h:2336`)。当前直通 flecs 64 位 id(低 32 index + 高 32 generation)。
  **已知问题**(P1,OPEN):`jce_scene_resolve_entity` 对高 32 位为 0 的首代句柄视为"无 generation"→
  首代实体防悬空被绕过(`C5-01`);公共 ABI **缺** liveness 谓词(`C5-04`)。规范目标:提供
  `jce_scene_entity_alive(scene, e)` 并令 generation 检查覆盖第 0 代。
- **不透明服务句柄**:`JceScene*`、`JceRenderer*`、`JceAudioEcs*` 等为不透明指针,消费者不得内省。
- **ECS 世界**:经 `jce_scene_get_world(): void*` 取得,以不透明 `void*` 传入适配器(审计已将 4 个
  `ecs_world_t*` 签名改为 `void*`)。
- **规范**:运行时句柄一般**不可直接持久化**;持久化用资产 GUID / 网络复制 ID(独立空间 + 映射所有者)。
  net 复制 ID 与 ECS ID 当前部分混用(P1,`C5-03`,OPEN)。

---

## 4. 字符串、数组、Blob(计划 §21.3)

- **约定**:UTF-8;输入为视图(`const char*`),输出所有权分离。
- **返回动态数据**:优先 (a) caller-provided buffer 两阶段查询,或 (b) 明确的 `jce_*_free` 配对。
  范例:`jce_json_print()` 返回的串必须 `jce_json_free_string()` 释放;
  新增 `jce_image_load_gray16_from_memory()` 配 `jce_image_free_gray16()`。
- **返回临时 `const char*` 的生命周期**:部分 getter(如 `jce_scene_entity_name`)返回内部指针,
  有效期至下次修改——**待补文档**(P3),FFI 绑定应即时拷贝。
- **二进制不伪装字符串**;大数组提供批量 API 避免逐元素 FFI。

---

## 5. 错误模型(计划 §21.4)

> **更正(2026-07-25)**:本节此前称"存在 `JceResult` / 错误码族(`jce_result.h`)"。
> **该陈述不实** —— `JceResult` 与 `jce_result.h` 在全仓库中**不存在**(`grep` 零命中)。这是审计
> 代理的一条未经核实的发现被直接写入文档。任何照此去找统一错误类型的绑定作者都会一无所获,
> 故在此显式更正而非静默改写。

**实测分布**(3052 个 `JCE_API` 声明,`scan` 见提交记录):

| 返回形态 | 数量 | 占比 | 能表达失败原因? |
|---|---|---|---|
| `void` | 1220 | 40.0% | **完全没有失败通道** |
| `bool` | 591 | 19.4% | 成败,无原因 |
| 其他值类型 | 585 | 19.2% | 视语境 |
| 指针(NULL=失败) | 462 | 15.1% | 成败,无原因 |
| `int` | 152 | 5.0% | 语义不统一 |
| 域枚举 | 42 | **1.4%** | 是,但**分散为 18 种互不相干的枚举** |

**结论与决策**:不存在统一错误模型,且**不打算通过审计批次去制造一个**。把 3052 个声明改造成
`JceResult` 是对每个消费者的源码破坏性变更;而新增一个几乎无人使用的 `JceResult` 枚举,恰好就是
本审计在别处批评的 "built but unwired"(参见 `jce_render_graph.h` 的 STATUS 更正)。

**改为按证据推进**:最贵的不是"枚举不统一",而是 **40% 的 `void` 里那些能失败却说不出口的函数** ——
`C6-FS-VOID-01` 正是此类,而且它掩盖了一个真实缺陷(mount 失败后 `has_loose` 仍被置真)。
已扫描:1220 个 void 公共函数中 **31 个的实现会记 error/warning 日志却无法把失败返回给调用方**。
本轮修正其中三个**注册类**(失败=功能静默失效,而非单帧异常):`jce_rpc_register`、
`jce_gas_replication_register`、`jce_net_var_register_all`。逐帧渲染类(如 `jce_renderer_end_frame`、
`jce_decals_render`)**刻意不改** —— 调用方对单帧失败无可行动作,返回码只是噪音。
- **不以日志代替返回**;**异常不得穿 C ABI**——审计已修 BT `registerBuilder/tickOnce/haltTree` 三处
  异常防火墙(C++ 模块入口 `catch(std::exception&)+catch(...)`→ 返回错误码)。C++ 模块(physics/renderer/
  video/ai/net)的 extern "C" 入口应普遍设异常防火墙(部分待补)。
- **绑定层映射同一错误码**,不重新发明;不得吞错或改成成功。JNI 当前将 3 态 `JceAppResult` 坍缩为
  `jboolean`(P1,`C6-JNI-DRIFT-01`,OPEN)。

---

## 6. 回调与异步(计划 §21.5)

- 每个回调 typedef 应文档化:调用线程、可重入性、`userdata` 生命周期、注册/注销竞态、回调期间可否销毁对象、
  VM/JNI attach 要求、shutdown 处理。范例:`jce_host_dialog.h` 明确"回调线程后端定义,消费者须自行 marshal"。
- **优先事件队列 / poll API / future-handle / 主线程分发**,避免外部库线程直接进入 Lua/Java 用户代码。
- **待补**:全面回调线程契约清单(P3)。

---

## 7. 线程模型(计划 §C4)

- 通用并行工作应统一走 JCE Job System。**现状**:os/core 并存两套(`jce_jobs` 手写 FIFO + enkiTS 支撑的
  `JceThreadPool`),P1(`A2-JOBS-DUAL-SYSTEM` / `C4-1`,OPEN)——待择一权威并迁移。
- 专用线程(音频设备回调、网络接收、文件监视、崩溃看门狗、WASAPI loopback)应有文档化理由 + 所有者 +
  启停顺序 + 线程亲和限制。
- C API 应声明每个函数可从哪些线程调用(**待补**,P3);脚本回调应统一回安全线程或显式标记。

---

## 8. Lua / C++ / Java 绑定(计划 §23–25)

### Lua(消费者)
- 78 个函数经 `register_binding(L, s, "name", fn)` 装入全局 `jce` 表,外加 1 个非函数键
  `jce.json_null` ——**表内共 79 项**。**装入点分属两个 TU**:71 个生成的在
  `jce_script_bindings.gen.c:1034`–`:1112`(`json_null` 在 `:1111`–`:1112`),7 个手写的在
  `jce_script.c:432`–`:438`。权威清单:`engine/src/middleware/script/script_exposure.json`
  (71 可生成 + 7 手写 + 1 常量);`tools/scriptgen/gen_script_bindings.py` 的条件 5 会**同时读这两个
  文件**并双向核对,`tests/middleware/script/test_jce_script_table_shape.c` 从活 VM 里用 `pairs()`
  读回这 79 个键。
- 只调 C ABI / 引擎服务;实体/资源对象包装 JCE 句柄。
- **已关闭**(原 P1 OPEN,"沙箱声明不成立"):`jce_script.c:527`–`:528` 把 `dofile` / `loadfile`
  置 nil,`l_sandbox_load`(`:459`)作为 `load` 安装于 `:531`(仅文本、仅字符串块),
  `tests/middleware/script/test_jce_script_sandbox.c` 以 6 个 `RUN_TEST` 从脚本内部证明。

### C++(用户,无特权)
- **现状**:无 header-only C++ 便利封装(编辑器/CK 直接调 C ABI)。编辑器纯经 `<jce/...>`
  (`check_editor_consumer_purity` 佐证),**无**私有 C++ 符号特权通道 —— 符合"C++ 非特权路径"原则。
- **规范目标**(P2/P3):可选提供薄 RAII 封装(owning/borrowed 区分,不隐藏昂贵拷贝,不抛异常穿模块),
  底层仍调稳定 C ABI,功能清单与 Lua/Java 同源。

### Java / JNI(消费者 + 桥接)
- `JCE_BUILD_JNI` 仅 CMake 开关(无虚构 Conan option,已核实)。
- **已知问题**(P1,OPEN):(a) 桥接编入 `jce_platform`(L2)却调 `jce_engine_*`(L6)= 层级倒置 +
  硬编码 `ck_app_get_desc` 消费者符号(`JNI-01/02`);(b) 3 态结果坍缩 bool(`C6-JNI-DRIFT-01`);
  (c) 无版本握手(`JNI-04`)。规范目标:桥接重定位到独立 consumer target、保真错误码、加版本握手。

### 绑定一致性(parity)
- `check_binding_parity.py`:C ABI = 事实来源;核心 6 概念(entity create/destroy、transform set/get、
  resource load、log)C/Lua parity **通过**,JNI 部分覆盖。契约冻结后以 `--strict` 入 CI 门。

---

## 9. 未来语言兼容性(计划 §26)

当前 ABI 对 C# P/Invoke、Rust FFI、Python ctypes/cffi、Swift、Kotlin/Native **基本友好**:扁平 C 函数、
不透明句柄、`void*` 世界、UTF-8 串。识别的**阻碍点**(需在扩展绑定前处理):
- ABI 结构体缺版本字段(§1.2)——新语言难做向前兼容。缓解已落地:`_sized` 入口范式
  + ABI 快照门冻结每个公共结构体的字段列表,任何增长都会变红并强制作者面对该 14 处清单。
- 裸 `bool`/enum 宽度未固定(§1.1)——需逐语言映射约定。
- 少量返回内部临时 `const char*` 的 getter(§4)——生命周期需文档化。
- 无 varargs / 无 union / 无需链接 C++ runtime 的公共符号(**已满足**,良好)。

---

## 10. 最小闭环(计划 §22)

计划要求的"API Schema → C ABI → Lua/JNI/C++ → 编辑器反射 → parity manifest"单一元数据来源:
- **现状**:C 头是事实来源;Lua/JNI 为手写绑定(存在漂移风险,正是 §8 若干 P1 的根源)。
  编辑器反射经组件注册表(`.docs/COMPONENT_REGISTRY.md` 4-处 checklist,手工同步 = P2)。
- **已落地的闭环验证**:`check_binding_parity.py` 对代表性 API 做 C/Lua/JNI 存在性比对并产出 parity 矩阵——
  即计划要求的最小闭环的**验证端**。
- **后续**:若引入 IDL/宏元数据自动生成绑定,可消除手写漂移;本审计不贸然重写全部 API,先以 parity 门锁定契约。
