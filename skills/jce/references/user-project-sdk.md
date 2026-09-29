# JCE 用户项目与 SDK

用户项目是 L7 消费方：只 `#include <jce/api.h>`（或细粒度 `api_*.h`），
不引入任何第三方依赖，通过 SDK 构建。编辑器自己也是这样一个消费方（dogfooding）。

## 0. 最贵的一条：改了引擎头必须重装 SDK

RED 基线里 **4/10** 的场景改了 `engine/include/**` 却没有这一步。
它们不是跳过，是**不知道这条边存在**。

```
改 engine/include/**  →  python scripts/jce.py sdk  →  重建消费方
```

不做的话，消费方拿到的是**安装树**里的旧头：
- `dist/sdk/<triple>/include/` 下有一份副本；
- 各项目的 `*/build/sdk-*/` 下还有副本。

**这些副本是构建产物，不要手改**——但「不该手改」不等于「不需要处理」。
处理方式是重装 SDK。

**触发条件是路径，不是改动的性质。** 只要
`git diff --cached --name-only | grep '^engine/include/'` 非空——
**包括只改注释、只改空行、只改 include 顺序、只改 doc 注释里的文件名**——就必须走这条链：

```bash
python scripts/jce.py sdk
python scripts/jce.py smoke
diff -r engine/include/jce dist/sdk/<triple>/include/jce      # 应当零差异
```

不要按「这个改动会不会影响 ABI」自行豁免：安装树里那份头是消费方**真正编译的东西**，
注释不同步就是两侧契约只更新一侧。顺手修的注释也算改动——
要么跟着跑这条链，要么从本次提交里拿掉、单独立项。

**这条对候选表、选项清单、「届时会落到的路径」同样生效。**
只要某个候选在任何一个文件路径上落在 `engine/include/**`
（含仅改注释、仅改空白、仅改 include 顺序、仅改文档示例），
该候选就必须**在自己那一行里带上代价**；不满足的候选不得列出。
并且 `diff -r` 要对 `dist/sdk/` 下**每一个 triple 各跑一次**——本机有 6 个。

**只刷新其中一部分 triple、把其余的差异标注为 pre-existing drift，不构成完成这条链。**
若 6 个无法全部刷新，本次改动的状态是 **blocked**，按 本文件（`SKILL.md`） §8b 收敛：
`DONE CRITERIA` 写 `不适用（N 个 triple 无法本机刷新，见证据表）`，
**不得**写出只覆盖宿主 triple 的 DONE CRITERIA。
（实测形态：一份转录完整承认了 6-triple 判据、量出 6 个全部失同步、
然后仍只刷新 `--arch x64` 并把差额推给 owner。那一次是诚实的——差异被量化并公开——
但「引用完整链 → 把不方便的部分归类为 pre-existing → 只跑一段」是可被滥用的路径。）

**不得**用「是否公共 API 形状变更 / 是否新行为 / 是否只是注释」当触发判据——
那是改动性质，不是路径。

**结论为「待拍板 / 零改动」也必须加载本 skill**：判断一个候选是否落在
`engine/include/**` 不需要先定形态。「形态未定所以先不加载」是本 skill 已知的失效路径。

失效形态最阴的是：**编辑器里效果对了，standalone exe 是错的**。
编辑器直接吃 `engine/include`，用户项目吃安装树。

### 判据必须证明「库与头出自同一次编译」

「跑起来没报错」不构成 ABI 一致性证明。头说结构体是 N+4 字节而库里的代码按
N 字节访问，会**链接成功**，然后在运行时静默写坏相邻字段。
存活类信号（退出码 0、`JCE_MAX_FRAMES` 到达、日志无 ERROR）对这种污染全部免疫。

可用判据：安装树头文件与 `engine/include` 全树逐文件哈希比对；
以及 `dist/sdk/<triple>/VERSION.txt` 里的 `commit:` 字段与当前 HEAD 一致。
（**没有 `VERIFY` 这个文件**——六个 triple 里一个都没有，`scripts/` 与 `cmake/`
里也没有任何代码写它或读它。）

**逐文件比对时注意方向：安装树可能比 `engine/include` 多。**
`install(DIRECTORY)` 是追加语义，从不删除上一次配置留下的副本，所以一个
**已经被排除**的私有模块的头会一直留在 SDK 里。实测 2026-08-27：
`jce_ai_dispatch.h`（36 个 `JCE_API` 声明、8 月 1 日）与 8 月 27 日的库共存，
而库里 `jce_aid_` 符号 **0 个**——消费方 include 它编得过、链接时一串
unresolved external，读起来像「我的项目坏了」。
根因已修（`jce.py sdk` 装之前先清 `<sdk>/include/jce`），但**旧的安装树不会自愈**：
比对时「安装树里有、引擎树里没有」和反过来一样是缺陷。

## 1. SDK 布局契约

```
dist/sdk/<triple>/
├── VERSION.txt      commit / host / variant / tools / scripts backends
├── bin/             host 工具（jce_pak, jce_cook, jce_bin2obj, ...）
├── include/         公共头（engine/include/jce 的安装副本）
├── lib/             库 + lib/cmake/JCE（find_package 入口）
└── share/           引擎自带资源、shader PAK
```

已有 triple：`win32-x86_64`、`win32-x86_64-debug`、`win32-x86_64-dist`、
`win32-aarch64`、`wasm`、`wasm-dist`。

`VERSION.txt` 的 `variant:` 是门禁：**不要用 release SDK 去造 dist 产物**。

## 2. 正典流程

```bash
# ① SDK（改过引擎头就必须重跑）
python scripts/jce.py sdk --arch x64 --variant release --no-debug

# ② 烹饪资产
python scripts/jce.py cook <project>

# ③ 构建消费方
python scripts/jce.py app <project> --arch x64 --variant release

# ④ 打包成单文件产物（默认就是单文件）
python scripts/jce.py package game <project>
```

`package game` **默认不拷松散资源树**；要松散树才加 `--with-loose`。

`cook` 子命令**不暴露 `--platform`**：它把**宿主平台写死**传给 `jce_cook`
（实测 `--dry-run` 输出里是 `--platform windows`），用户无法选择目标平台。
⇒ 移动端 ASTC 烹饪从这个子命令不可达，需要时走项目自己的 CMake 路径。

## 2b. `--variant dist` 从命令行怎么走（2026-09-14 打通）

```bash
python scripts/jce.py sdk --arch x64  --variant dist      # 或 --arch wasm
python scripts/jce.py app <project> --arch x64  --variant dist
python scripts/jce.py app <project> --arch wasm --variant dist
```

**在 2026-09-14 之前这两条 app 命令在 configure 期就死**，报的是
`cmake/JCESDKHelpers.cmake` 里一道故意的门：

```
jce_target_embed_pak: dist requires the editor's authenticated prebuilt PAK
and key-share source. Build/package the project through JCE Editor; ...
```

它只认三个预制变量之一 `JCE_PROJECT_PREBUILT_ASSETS_OBJ/_ASM/_C`
（符号前缀必须是 `assets_pak_data`）外加 `JCE_PROJECT_PREBUILT_PAK_KEY_C`，
而当时只有 `editor/src/core/jce_build_manager.cpp:2458-2462` 写它们。
`jce.py package game` 也一样死——它就是 `cmd_cook + cmd_app`。

### 补法：**没有重写打包器**

`tools/jce_pak.c` 早就能做整件事，四步检索的结果是只差两小块：

| 步骤 | 谁做 |
|---|---|
| 加密 PAK + 嵌入对象 | `jce_pak`：`--encrypt-key-file` 设的是**与编辑器逐字相同**的 `JceBundlePackOptions`（`encrypt` / `encryption_key` / `encrypt_label="project_assets"`，jce_pak.c:1482-1486）；`--symbol-prefix` 默认就是 `assets_pak_data` |
| web 的 `.S` incbin 包装 | `jce_bin2obj --format asm`（`jce_pak` 只有 coff\|c-array） |
| `.jce/pak_key.hex` 生成/读取 | **新增** `scripts/jce.py` 的 `_project_pak_key` <!-- skills-lint: allow 每项目自己的密钥落点，不是仓库文件 --> |
| 份额 TU | **新增** `_write_pak_key_shares_c` |

三处（引擎默认 / 编辑器发射器 / CLI 发射器）由
`tools/lint/check_pak_key_shares_contract.py` 钉住——任何一处的符号名或
那个 64 漂了都不会报错，只会让加密产物在客户机上解不开自己的档案。

### 三个实测踩过的坑

1. **要打三个资源根，不是一个。** 只打项目 cooked 树时，exe 链接成功、
   启动即死：`app: jce_project.json and packed runtime boot manifest are both
   missing`。boot manifest 的虚拟路径是 `jce/runtime_boot.json`，由
   `_jce_prepare_runtime_boot_manifest` 从 `jce_project.json` 的
   `startup_scene` 生成；另外还要 SDK 的 `share/jce/engine_ui`
   （以及存在时的 `engine_resources`）。
2. **`.jce/input_actions.json` 是 fallback，不能无条件铺。**
   项目自带一份时 `jce_pak` 直接拒收整个档案：
   `add: duplicate path 'settings/input_actions.json'`。
3. **web 不要用 c-array。** 13 MB 的 blob 做成 C 数组是约 80 MB 源码；
   编辑器在非 Windows 上用的就是 `.incbin`，照做。

### 实测产物（space，2026-09-14）

```
win  x86_64  space_demo.exe   34.0 MB   119 files packed -> pak 12.6 MB
             PAK 标志 ENCRYPTED SECURE_INDEX AUTHENTICATED
             600 帧无头跑 exit 0、零 ERROR（证明份额解密端到端可用）
wasm32       space_demo.wasm  13.5 MB（= pak 12.6 MB 内嵌 + 引擎）
             space_demo.js 254 KB · space_demo.html 0.9 KB
```

## 3. 项目 CMake 形状

```cmake
find_package(JCE REQUIRED)

add_executable(MyGame src/main.c ...)

if(COMMAND jce_configure_application_target)
    jce_configure_application_target(MyGame)      # dist 策略、运行时装配
endif()

jce_add_pak(MyGame RESOURCE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/resources/assets")
```

SDK 提供的函数（定义在 `cmake/JCESDKHelpers.cmake`）：

| 函数 | 作用 |
|---|---|
| `jce_add_pak(TARGET RESOURCE_DIRS ...)` | 烹饪 + 打包 + 嵌入 PAK |
| `jce_configure_application_target(TARGET)` | 应用目标策略（dist 下强制免版税等） |
| `jce_target_embed_pak(TARGET)` | 把 PAK 嵌进 exe |
| `jce_target_embed_bundle(TARGET)` | 嵌入 `.jbundle` |
| `jce_add_project_shaders(...)` | 项目自带 shader 编译 |
| `jce_shader_profiles(OUT_VAR)` | 当前目标的 shader profile 列表 |

单文件机制链：`jce_add_pak` → cook → `jce_pak` → `jce_bin2obj` →
生成的 `assets_pak_data[]` 数组直接链进 exe。

**改一行 Lua 也必须重链 exe**——脚本在 PAK 里，PAK 在 exe 里。

## 4. `jce_project.json`（项目清单）

`scripts/jce.py` 读它取 `target` / `name` / `exe` / `version` / `sdk_path`。
缺失时回退到目录名。`examples/caged_kingdom/jce_project.json` 是可抄的最小样例（12 行）。

## 5. 单文件可搬动性 —— 验收判据（本轮实测通过）

要求：**exe 可以被任意搬动，不依赖 cwd、不依赖同级目录。**

验收方法（不要用别的）：

```bash
# 只把 exe 一个文件拷进一个随机空目录，然后从那里跑
mkdir /tmp/probe && cp <out>/MyGame.exe /tmp/probe/ && cd /tmp/probe
JCE_MULTI_INSTANCE=1 JCE_WINDOW_HIDDEN=1 JCE_MAX_FRAMES=400 ./MyGame.exe
```

**判据是日志里这一行：**

```
app: runtime assets mounted: pak=yes loose=no
```

`pak=yes loose=no` 才算通过。另外应能看到
`engine: input actions loaded from embedded PAK`。

实测记录（当时的交付目录 `space/dist/delivery/windows-x64/` 下**只有一个** `space_demo.exe`，  <!-- skills-lint: allow 该路径是一次历史实测的现场，交付目录不入库、现在两个 worktree 上都不存在 -->
39.7 MB）：拷进随机临时目录后 exit 0、上述两行俱全、运行后目录里**没有新增任何文件**。

`JCE_MULTI_INSTANCE=1` 是必须的：见 `references/evidence-verification.md` §4 ——
早于 2026-08-17 构建的产物不认 `JCE_WINDOW_HIDDEN=1` 的绕锁。

## 5a. 「单文件」说的是**资产**，不是**运行时**（2026-09-21 实测）

上面那条判据是真的，但它的作用域比字面窄。`pak=yes loose=no` 证明的是
**资产**都在 exe 里；它对「这个 exe 自己 import 了哪些共享库」**一个字都没说**。

一个开了脚本的游戏链接的后端运行时**不在它自己的二进制里**：

| 必须与 exe 同级 | 谁需要它 |
|---|---|
| `jce_script_api.dll` | C ABI，所有 REFERENCE 形态的语言 |
| `jce_script_java.dll` + `jce_script_java_classes/` | java |
| `python312.dll` + `jce_script_python/` | python |
| `nethost.dll` + `JceScript.dll` + `JceScript.runtimeconfig.json` | csharp（runtimeconfig 由 hostfxr **按名字**在旁边找） |
| `<项目>Scripts.dll` | 该项目自己的托管脚本程序集 |

少任何一个共享库，**进程在 `main()` 之前就死**：
`error while loading shared libraries: nethost.dll: cannot open shared object file`，
exit **127**，日志只有一行——**看起来像机器坏了，不像打包漏了**。

少两个**目录**更坏：exe 照常起、照常 exit 0，只是 java 与 python
**按名字拒绝**。在装了 SDK 的开发机上**完全看不出来**，因为注册 shim
会退回去找 SDK 里的那份。

⇒ **§5 的「拷一个 exe 进空目录」不足以验收一个带脚本的游戏。**
带脚本时判据要多一条：**把 `PATH` 刮到只剩系统目录**，再逐个文件移除重跑，
看哪些是真的承重：

```bash
# Windows 上不刮 PATH，Windows 会从机器上别处找到 python312.dll，
# 于是你量到的是「这台机器」，不是「这个包」——结论恰好反过来。
PATH="$SystemRoot/System32:$SystemRoot"
```

判据是装载日志那一列语言（见 `references/scripting-bindings.md`
「花名册说『有』，不等于它会跑」），**不是 exit code**：
少语言的那一档退出码也是 0。

**谁负责把它们放过去，两条路不一样：**

| 命令 | 产出 |
|---|---|
| `jce.py package game <项目>` | 完整可再分发包（exe + dll + cooked） |
| `jce.py build-project <项目>` | 走编辑器的 build manager |

`build-project` 在 2026-09-21 之前**一个共享库都不拷**，却照样打印
`[build] package staged at: <目录>` 并报成功——staged 的那个包**起不来**。
已修（按形状拷：共享库 + `*.runtimeconfig.json` + `jce_script_*` 目录，
拷贝失败直接判构建失败）。**两条命令都写 `dist/games/` 下，且都先
`remove_recursive` 清空目标目录**——用同一个 `--out` 指向同一处时，
后跑的那条会把前一条的完整包**静默换成**它自己的那一份。

### 同一份源码要建两次：exe 链进去，编辑器 dlopen

C / C++ 脚本类在**出货 exe** 里是链进去的（`JCE_SCRIPT_MODULE_NO_ENTRY`，
否则两个模块都定义入口符号 ⟹ `LNK2005`）。
**编辑器是另一个进程，不链接这个游戏**，它只能 dlopen
`jce_project.json` 的 `script_modules` 里点名的共享库。
托管程序集同理，走 `script_assemblies`（v5）。

所以两种产物都要出，而且**最好由同一条 `jce.py build-project` 出**——
一个只影响编辑器的手动步骤，是没人会发现自己漏掉的那一种：
出货游戏照样跑满，编辑器安静地少几种语言。

**两个已实测的坑：**

1. **`set_source_files_properties()` 挂在*源文件*上，不是 target 上。**
   一旦同一份源码又被建成 MODULE，那条属性也会跟过去——
   而 `JCE_SCRIPT_MODULE_NO_ENTRY` 正是**抑制入口符号**的那个定义。
   结果：模块编出来了、链上了、被 dlopen 了，然后引擎拒收
   「exports no `jce_cpp_script_module` — it is not a JCE script module」。
   **信息完全正确，病因却只是一个关于作用域的词。**
   放到 `target_compile_definitions(<exe> PRIVATE ...)` 上。

2. **CMake 默认库前缀跟工具链走。** 同一行 `add_library(... MODULE ...)`
   在 MSVC 下产出 `snake_c.dll`、在 MinGW 下产出 `libsnake_c.dll`，
   而 `jce_project.json` 是**按字面名字**写的 ⟹
   一个在这个编译器下正确的清单，在另一个下点名一个不存在的文件，
   而报错说的是「那个文件不在」——它确实在，只是换了名字。
   `set_target_properties(... PREFIX "")`。

## 5b. 验收协议 —— `package` 结束于「目录存在了」，那不是验收

```bash
python scripts/jce_accept.py --project <项目目录> --backend d3d11
python scripts/jce_accept.py --project <项目目录> --skip-package --frames 200
```

八个阶段，各自 PASS / FAIL / **SKIP**（SKIP 不算 PASS）：

| 阶段 | 它挡住的东西 |
|---|---|
| `package` | cook + build + stage，委托给 `jce.py` |
| `relocation` | 把包拷到**没有仓库在其上方**的目录里跑，并逐级向上核实没有 `.git`；同时从环境里摘掉 `JCE_SDK_DIR` / `JCE_SHADER_DEV_DIR` / `JCE_DEV_ASSETS` 等，否则「搬走了」这件事没被证明 |
| `boot` | 退出码是证据的一部分：打印完再崩掉的运行不是一次测量 |
| `scene` | 日志必须点名 manifest 的 `startup_scene`。找不到 `jce_project.json` 的运行时会启动**默认空场景**并且什么都不报错 |
| `backend` | 引擎自己打印的 `jce_renderer: renderer: …`，不是 `JCE_BACKEND` 这个**请求**——bgfx 会回落 |
| `screenshot` | 非空帧。两张空帧比起来完全相等 |
| `perf` | 稳态样本，用 `jce_determinism.PERF_RE` |
| `listener` | 见下 |

项目是参数，脚本里没有任何用户项目的名字。caged_kingdom 实测
**7 PASS / 0 FAIL / 1 SKIP**（2026-08-27）。

**旧包会拖垮这条协议**：一个 6 月构建的 bundle 在本机启动要 500 s 以上，
另一个项目的旧包 10 分钟没跑完。超时**不是通过**。

### 用户项目会伸手抓引擎内部符号，然后静默停止可打包

`elemental_serenity` 的 `package game` 在链接期挂掉，25 个未解析符号，两类：

| 类别 | 例子 | 性质 |
|---|---|---|
| 项目自己的 | `es_script_probe_init` / `es_script_langs_register` | 它自己的源文件没进目标，项目侧构建配置问题 |
| 引擎**内部**的 | private transport helpers | 声明在 the optional private AI transport header，**无 `JCE_API`**，`engine/include` 里 **0 处** |
| **公共 API，但整个模块没被编译** | optional extension entry points | 带 `JCE_API` 声明在 `engine/include/jce/middleware/ai_dispatch/jce_ai_dispatch.h`——而 `JCE_ENABLE_AI_DISPATCH` 默认 **OFF**，该模块 **0 个 .obj**，SDK 库里 `jce_aid_` 符号 **0 个** |

第一类**从来就不是公共 API**，SDK 不可能导出它。

第二类更阴：它**是**公共 API，头也确实躺在 SDK 里，但背后没有任何实现——
`JCE_ENABLE_AI_DISPATCH` 是默认 OFF 的私有模块选项，安装规则本该把它的头
排除出 SDK，而 `install(DIRECTORY)` 只增不删，某次开着装进去的副本就永远留下了
（已修：`jce.py sdk` 装之前先清 `<sdk>/include/jce`；**但旧的安装树不会自愈**）。
⇒ 看到 `JCE_API` 不等于「这个符号在库里」。**判据是库，不是头。**

**症状是沉默的**：`dist/games/` 里躺着一个 **2026-07-04** 的 exe，看起来一切正常——
自那以后没有一次打包成功过，而没有任何门禁会说出来。`jce_accept.py` 的
`package` 阶段是第一个把它喊出来的东西。

⇒ 看到一个用户项目引用 `engine/src/**` 里的符号，那不是「还没导出」，
是**它走的根本不是 SDK 那条路**。要么把该 API 提升进 `engine/include/jce/`
（通用面上补缺口，走 §0 的重装 SDK 流程），要么承认这个项目不走 SDK 消费路径。
**不要为了让某个项目链过去而把内部符号加上 `JCE_API`** ——
那是用户项目反向污染通用面。

## 5c. 出货件不该开着监听口（2026-08-27 实测并修）

`JCE_ENABLE_PROFILING` 从 Tracy 落地起就是 CMake 选项、默认 **ON**，而在
2026-08-27 之前**没有任何脚本暴露过它**。于是每个 release SDK 都带 Tracy，
每个用它链出来的游戏也带；Tracy 在**静态初始化期**就起服务端——在 `main()`
之前，且无论有没有人来连。

| 量 | 值 |
|---|---|
| release SDK 的 `jce_engine_deps.lib` | tracy 串 **1153** |
| dist SDK 的同一个库 | **0** |
| 当时新构建的游戏 / 编辑器 | 整轮 bind **TCP 127.0.0.1:8086** |
| `sdk --profiling off` 重建后 | **零监听** |

回环绑定**不会**触发 Windows 防火墙；触发弹窗的是链了早于
`TRACY_ONLY_LOCALHOST` 那道守卫的 Tracy 的旧包。但「没人连也一直占着口」
本身就不该出现在出货件里。

```bash
python scripts/jce.py sdk --profiling off --no-debug   # 三态：不传=不动缓存
python scripts/jce.py package game <项目>              # 落盘后自动扫 Tracy 标记
```

`package game` 的默认仍是 **release**。`dist` 在 CLI 这条路上走不通——
`jce_target_embed_pak` 要求编辑器出的**已认证 PAK**，原始资源打包会在
configure 期直接失败。别去改这个默认，理由写在参数定义处。

**反方向也验过**：要让它开公网口，必须**同时**做两件事——conan 重解析
`-o tracy/*:only_localhost=False -o tracy/*:no_broadcast=False`，**且**
`-DJCE_PROFILING_ALLOW_REMOTE=ON`。只做后者，`CMakeLists.txt:417` 会 FATAL_ERROR：

```
JCE_PROFILING_ALLOW_REMOTE=ON but the linked Tracy was built with
TRACY_ONLY_LOCALHOST.  Re-run conan install with -o "tracy/*:only_localhost=False"
```

（2026-08-27 在独立 scratch 目录实测，未触碰现有 build 目录。）
被链接的 Release Tracy 自己声明的定义是
`TRACY_ENABLE TRACY_ON_DEMAND TRACY_ONLY_LOCALHOST TRACY_NO_BROADCAST`，
读它的地方是 `build/*-conan/build/Release/generators/Tracy-release-*-data.cmake`
——**那是个多行 `set()`，只打一行会读成「只有 TRACY_ENABLE」**，我就这么误读过一次。

⇒ 弹窗**只可能出自一次明确的双重配置**，不会自己回来。

量它的办法（**不要靠推断**）：

```powershell
$p = Start-Process -FilePath <exe> -PassThru
Get-NetTCPConnection -State Listen | Where-Object { $_.OwningProcess -eq $p.Id }
Get-NetUDPEndpoint            | Where-Object { $_.OwningProcess -eq $p.Id }
```

## 5d. 出货游戏的日志：`JCE_LOG_FILE`

出货游戏是 **WIN32 子系统进程，没有控制台**，stdout 是断开的——引擎写的每一行
都去了不存在的地方。`JCE_LOG_FILE` 在 2026-08-27 之前**只有编辑器读**
（`editor/src/core/jce_editor.cpp`），引擎从不读，所以任何针对 bundle 的诊断
都拿不到一行输出。现在接在 `jce_engine_create()` 里 `jce_log_init()` 之后、
任何子系统开口之前——放晚了，最值得看的 renderer 与场景加载两行恰好是丢掉的
那两行。

```bash
JCE_LOG_FILE=<路径> JCE_PERF_LOG=1 JCE_MAX_FRAMES=200 <游戏exe>
```

## 6. 当前在用的用户项目

**`examples/caged_kingdom/`（ck）是当前的主用户项目**（owner 2026-08-26）。
它是根 `AGENTS.md` 指定的样例，也是仓库里**唯一入库**的消费方项目。
`space/` `science_lab/` `elemental_serenity/` `street_demo/` `meadow_valley/`
仍可作参考，但**不是当前工作目标**——除非任务明确点名它们，
否则「用户项目」默认指 ck。

## 6b. 模板与治理样例

| 用途 | 参考 |
|---|---|
| 代码形状的权威 | `engine/templates/` 下的空项目模板（编辑器 New Project 真正读的东西） |
| 最小可跑参考 | `science_lab/`（含 `jce_project.json`，CMakeLists 里注释了两个非显然陷阱） |
| 治理形状参考 | 曾是 space/（`AGENTS.md` 管代码/运行时、`CLAUDE.md` 管文档/证据链，互不覆盖）——**已于 2026-08-30 移出仓库** |
| 单 exe + 多语言脚本参考 | 曾是 elemental_serenity/——**已于 2026-08-30 移出仓库** |

> **2026-08-30：样例项目不在这棵树里了。** owner 把 space、street_demo、
> elemental_serenity、meadow_valley、science_lab 五个全量镜像到
> `<local-consumer-root>/`（另有一份 es 的 source-only
> zip），仓库里只保留主项目 `examples/caged_kingdom/`。上面两行的参考形状仍然成立，
> 但要去那个镜像里看，别在树里找。

**不要抄一份别的项目的 `AGENTS.md`。** 实测过的反例：street_demo 的那份是
`examples/caged_kingdom/AGENTS.md` 的逐字复制，项目名、exe 名、目录布局全错，还漏掉了它
自己的子目录——它会主动误导。（street_demo 现已移出，这条留作教训而非路径指引。）

## 7. 没有 CLI 建项目命令

脚手架只在编辑器里（New Project）。从命令行起一个新项目 = 复制
`science_lab/` 的形状再改 `jce_project.json`。

## 8. 收尾

```bash
python tools/lint/run_all.py                    # 判据：exit 0 且无 FAIL 行
python scripts/jce.py smoke                       # SDK C ABI 回归门禁
```

`smoke` 跑两个**独立**的 out-of-tree 消费者（纯引擎 / 带脚本），
故意分开：一个「声称有脚本后端却没装」的 SDK 会因此变红。

## 9. 红旗

| 念头 | 现实 |
|---|---|
| 「编辑器里看着对了」 | 编辑器吃 `engine/include`，用户项目吃安装树 |
| 「`dist/**` 是产物，我不用管」 | 不该手改，但必须重装 |
| 「exe 跑起来了、退出码 0，ABI 没问题」 | 字段错位会链接成功并静默写坏内存 |
| 「打包完把 exe 和 assets 目录一起给」 | 单文件要求是只给 exe 的**资产**；用 §5 的判据验。**带脚本的游戏另有共享库与 `jce_script_*` 目录必须同级**，见 §5a |
| 「`cook --platform android`」 | 该参数不存在 |
| 「照另一个项目的 `AGENTS.md` 布局」 | 逐字复制来的那份是错的，见 §上文 |
