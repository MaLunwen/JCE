# 构建与验证

Windows 桌面依赖的 API 下限是独立编译输入。既有 CMake 策略 hook 为 PhysFS
选择 WINVER/_WIN32_WINNT=0x0601，并将策略写入包身份。通过共享原存储的受控
配置目录刷新；不能改写旧包或只重新链接旧归档。核验实际编译定义和新程序
必需导入的符号。共享 PE 解析器记录普通/延迟符号和未知序号；四名称兼容性
拒绝规则仅是有限检查，不是完整目标系统验证。

受控原生 embedded SDK 配置可通过 `sdk` 或 `package sdk` 使用配对的
`--build-dir <build/独立缓存>`、`--install-dir <dist/sdk/独立安装>`，以及
`--embed-engine-shaders auto|on|off`。自定义路径必须是对应根目录下的独立
严格子目录，不能与默认 SDK、共享 desktop/web/host/锁缓存重叠；自定义输出
拒绝 `--clean`。显式 OFF 必须使用独立路径，表示缺少内嵌 stock shader 能力，
消费方需提供所需 shader。原生 auto/on 显式恢复发布默认 ON。
authoring、Web 与跨架构目标拒绝自定义路径或非 auto 选择。继续复用共享静态
Conan/CMake/安装流程与内核锁。配置前在锁内原子记录有限的请求及失败状态，
同 owner 可重试首次普通失败；已有 CMake 缓存必须完整匹配源码、输出、
toolchain、variant、delivery 与 shader 身份。已有安装还须通过原有编译器生成的
目标元数据、archive 存在性和 `sdk-build-options.json` 请求身份校验。
请求标记及计数命令端口测试不证明 archive ABI、原生运行或画面。

使用 CLI 支持的目标和参数；scripts/jce.py 转发到 tools/build/jce.py。

选定独立 SDK 缓存后，后续构建继续明确传入配对的 `--build-dir` 与
`--install-dir`。省略参数会选择兼容默认值，可能重配桌面 Editor 缓存并发布到
另一 SDK 安装目录。先核对真实 configure/install 路径，再将该次运行计入目标
产物的验证，不能仅凭命令退出成功认领另一前缀的资格。

```bash
python scripts/jce.py --help
python scripts/jce.py targets
python scripts/jce.py lint
python scripts/jce.py test --arch x64 --jobs 8
python scripts/jce.py test --configure --variant debug --arch x64 --jobs 2
```

lint 命令执行源码检查与架构审计。缺失的私有检查属于不可用，不算通过。迭代时执行针对性检查，交付前执行规定的完整检查。

公开头改动需要刷新 SDK 并验证消费项目。语言绑定改动需要以检查模式运行两个生成器。已提交 ABI 检查看的是 HEAD；工作区中待提交的基线不能使旧 HEAD 通过。

当前 SDK/工具发现边界及完整宿主/目标载荷要求见 [工具链发现](toolchain-reference.md)。源码检查、库准入、真实目标构建和零环境交付分别保留证据。

第三方原件只从 contracts/vendor-sources.json 的固定版本获取。核验已有原件，不修补第三方源码树或 Conan 包。仅修改配置的 Conan hook 遵循 CONAN_HOME。缓存二进制构建不能证明源码纯净或跨平台兼容。

原生 MSVC 的干净激活与已激活分支均保留已有 JCE_SHADERC_EXECUTABLE、
BGFX_SHADERC 宿主工具选择项。可用受支持的选择项，在目标运行时构建类型之外
选定经过验证的宿主 shader 编译器。保留编译器、参数及 include 路径/内容的
完整缓存键；更换工具或 include 根可能需要首次编译。环境透传、实际工具选择
与目标产物验证分别保留证据。

bgfx CPU profiler 属于依赖编译选择，默认关闭。诊断构建须显式设置
`JCE_BGFX_CPU_PROFILER=on`；显式 `off` 恢复默认。空值及其他值会被拒绝。
共享 bgfx Conan 参数传入 `user.jce:bgfx_cpu_profiler=off|on`，同时进入包
identity 与既有生成复用指纹。配置 hook 合并真实的 `tools.build:cxxflags`
定义 `-DBGFX_CONFIG_PROFILER=0|1`，保留调用者标志并拒绝冲突定义。
相同定义还通过 Conan 的 `tools.build:defines` 目录通道写入，避免被上游
Apple/Ninja 对全局 C++ flags 的重置清除。启用包的 identity 增加配置版本，
off 不增加 CPU profiler 配置版本；下文独立的 annotation 策略也会划分包 identity。
仅设置 CMake 缓存变量、运行时 debug 标志或注册回调，不能启用这些编译期
作用域。生成的包元数据导出 `JCE_BGFX_CPU_PROFILER_COMPILED=0|1`；宏缺失
表示未知，未观测到回调也不能证明该包编译时关闭了 profiler。

GPU annotation 通过 `JCE_BGFX_DEBUG_ANNOTATION=auto|on|off` 选择，默认
`auto`。共享 driver 将 `user.jce:bgfx_debug_annotation` 传入原 Conan、生成
复用指纹及干净 MSVC 环境流程。auto 保留调用者的上游 CMake 选项；未指定时
沿用上游 OFF。显式 on/off 选择该选项并拒绝冲突，不覆盖上游 target 表达式：
目标非 WindowsStore，且配置为 Debug 或选项为 ON 时才编译 annotation。
因此 Debug 加 off 仍会编译，WindowsStore 加 on 仍被排除。Hook 保留无关
flags 与调用者的带类型选项，拒绝冲突的保留宏定义、取消定义及目录定义。
只有实际编译变化或输入未知时，annotation 选项、编译状态与策略版本才增加
bgfx 包 identity 维度。默认 auto、等价 off、Debug 强制开启及 WindowsStore
排除均保持历史包 ID；不会只因新增请求元数据而重编 bgfx。等价启用选择共享
同一包 ID，不修补既有第三方源码或包。元数据导出 `JCE_BGFX_DEBUG_ANNOTATION_REQUEST`
（-1 auto、0 off、1 on）、`_OPTION` 与 `_COMPILED`（-1 未知、0、1）。
宏缺失、未解析的自定义 toolchain 或配置保持未知。原 renderer attempt
日志与 trace 发布这些字段；运行时 debug 标志、CPU 回调或编译支持均不能
证明实际 GPU marker 或时间戳。各后端须分别核验真实编译定义及具名 pass
capture；诊断产物不能静默替换正式性能基线。

已安装的第一方 hook 与当前版本不同时，保留原 Conan home 与 hook。
未显式选择 CONAN_HOME 的原生 editor、SDK 与宿主工具构建会针对该冲突自动
选择小型受控 `build/conan-config/native-<identity>` overlay，无需用户设置环境。
身份来自原 home 与当前 hook/策略字节；相同组合复用相同目录与实际包存储。
显式指定 home 时仍拒绝未知 hook。配置、profile 与临时文件路径重定向或
不安全输入须在发布前拒绝，不复制包、源码、认证或数据库目录。
已安装 hook 门禁复用同一个只读 `select_hook_home` 策略，核验实际选定的
配置目录，不创建或修复任何文件。缺失 hook 仅表示尚未安装，不证明构建
已经执行；显式 home 或选定 overlay 中的过期 hook 及已退役写源码 hook
仍须失败。
通过 `tools/build/conan_config.py` 既有的
`prepare_configuration_home(base_home, overlay_home)` 准备独立且受所有权
检查的 Native 配置 home，仅将返回的 `home` 作为本次选定构建的
`CONAN_HOME` 传给原 hook/export/install 流程。Native 原样保留实际
settings/settings_user 字节，不添加任何编译器版本。Overlay 共享原配置
实际生效的 `core.cache:storage_path`，不复制认证数据、包、源码或数据库
内容；新 hook 只写入 overlay，并保留既有凭证、大小、目录重叠与所有权
检查。Web 则使用 `prepare_toolchain_home` 与探测得到的真实 Clang major；
禁止为准备 Native overlay 而伪造 major。

Conan 生成复用会自动计入实际生效 `CONAN_HOME` 的配置 digest，包括默认
home。Native 在同一 home 刷新配置即可使旧缓存失效，无需手动传入 digest；
Web 保留原有工具链身份输入。有界扫描只读取 `settings.yml`、
`settings_user.yml`、`global.conf` 和 `profiles` 内的文件：每文件最多
1 MiB，profile 内容合计最多 4 MiB，profile 目录与文件合计最多 4096 项。
缺失输入明确编码；权限、目录扫描、类型、路径重定向或预算错误均拒绝
复用。不读取包、认证或数据库内容，也不覆盖上述选定范围外的 profile
include。Digest 描述扫描期间实际读取的文件，不保证其他进程并发改配置
时的原子快照；生成期间须保持该配置不变。

保留真实依赖编译和最终链接产物身份，再关联实际观测到的运行时回调。
SDK 消费者须通过 `app --sdk <选定前缀>` 重建以链接诊断包；环境开关不能
改变旧 EXE。嵌入式 Editor 走既有源码闭包构建，不是切换 SDK 前缀。保留
编译输入，并核验最终程序实际发布的能力及回调观察后，再归因后端 CPU 时间。
编译选择与运行时采集开关分别处理；配置元数据、CPU 夹具通过和回调接线
不能证明已经执行采样，也不能证明达到真实性能目标。

全新测试目录使用 `test --configure --variant debug|release`。CLI 默认仅检查
系统依赖；CI 明确设置 JCE_SYSTEM_PACKAGE_MODE=install 与
JCE_SYSTEM_PACKAGE_SUDO=1。原生工具名称和 Debug CRT 策略遵循目标平台。
Linux 原生依赖元数据需要可执行的 pkg-config。应检查实际执行，特别是 WSL
继承 Windows 工具目录时，不能仅凭发现同名文件判定可用。
CMAKE_BUILD_PARALLEL_LEVEL 同时控制编译构建和 Conan 宿主、构建上下文的
依赖并行度，共用默认值为八。原生宿主资产工具刷新复用同一 Conan 预算，
清理 MSVC 环境时保留该变量。CTest 单独使用 --jobs/CTEST_PARALLEL_LEVEL。
资源受限的主机应保留这些限制；正式性能测量应排除同时运行的构建任务。

受支持的构建入口位于 scripts/。编译器与依赖命令由通用 tools/build 实现生成，
不手写第二套构建配方。复用兼容的目标/变体/工具链缓存，仅隔离真实 ABI 或
配置差异，避免每次试验复制整套源码和构建树。检查磁盘余量，区分出货应用
大小与 PDB、静态 SDK 库、依赖缓存和验收证据。生成的测试产物放被忽略的
build/，测试源码仍在 tests/。第三方原件和引用的原始证据保持不变。GPU 验证
不能抢焦点；仅在需要渲染时使用隐藏或非聚焦窗口。

Web 应用策略须先于预生成 OBJ/ASM/C 资产分支的所有提前返回。release 和 dist 均
交付单 HTML；交付前单文件审计须拒绝外部 JS、wasm 和 data。验证真实构建策略，
不能只检查手动资产分支的标志。消费构建的变体须与所选已安装 SDK 的变体一致。

Web DIST 预打包资产通过 `JCE_PROJECT_PREBUILT_ASSETS_PAK` 使用现有
`/game_assets.pak` MEMFS 启动路径；单独链接 ASM 数据不能满足 Web 启动。
原生预打包 ASM 必须启用 ASM 语言。每次 app 配置都清除旧预打包选项，
包括从 DIST 切回 release。

每次预打包前，显式编写的根目录 `Settings/RenderPipeline.rp.json` 都须按原始
字节同步到 cooked `settings/render_pipeline.rp.json`，与正常 CMake 路径一致。
源文件变更须进入下一份归档；重复打包保留未变内容。没有根目录 RP 时，保留
独立提供的 cooked 规范配置。不能在 boot 根目录重复添加同一规范资源键。
验证实际 PAK 条目的大小、CRC 与唯一性；期望的 JSON 不能证明执行中的 GPU
路径已启用相应功能。

`app --registry-proof <receipt>` 复核现有 embedded Web 产物而不重新构建；
否则 DIST 的新密钥份额会改变哈希。SDK/profile、当前闭包、实际 nonce/
请求/产物哈希验证仍保留。缺产物及 clean/configure-only/原生冲突均拒绝。

## 测试域与证据类型

CTest 注册按源码责任分类为 `domain:engine`、`domain:editor`、
`domain:consumer`，按证据类型标记 `kind:unit`、`kind:integration`、
`kind:graphical`、`kind:contract`。不能把 C++ 自动视为编辑器。
直接注册的测试、脚本和变体使用 tests/cmake/JceTestMetadata.cmake
同一元数据所有者；缺失或冲突应拒绝配置。独立 SDK 消费者保留本地声明，
不得增加仓库相邻辅助文件的依赖。

```bash
python scripts/jce.py test --suite all --arch x64 --jobs 2
python scripts/jce.py test --suite engine --arch x64 --jobs 2
python scripts/jce.py test --suite editor --arch x64 --jobs 2
python scripts/jce.py test --suite consumer --arch x64 --jobs 2
```

默认 `all` 选择完整的已配置注册集合。构建输入使用同一登记位置的 domain authority：`all` 构建 `jce_tests`，其余分别构建 `jce_tests_engine`、`jce_tests_editor`、`jce_tests_consumer`。共享/直接/脚本/工具输入通过 `jce_test_build_dependencies` 接线，延迟检查拒绝缺失或未分类的依赖。显式 `--target` 覆盖构建集合，`--filter` 仅选择已登记的 CTest 用例；域选择不得缩小完整登记或改变 SKIP。

注册数量不是通过数量，
可选后端不可用须单独保留。解析器、元数据、替身端口和 Noop 测试
不能证明图形实际执行。限制编译器和 Conan 并发时另设
CMAKE_BUILD_PARALLEL_LEVEL；`--jobs` 限制 CTest 并发。
硬件与界面验收应串行使用当前产物、隔离配置及保留的失败对照。

额外 cook/色彩链验收只属于未过滤的完整 all：显式 target、正则过滤、单域
suite 或 rerun-failed 在所选 CTest 后结束，不得隐式扩大构建范围。
完整 all 的重复运行保留验收。后置步骤失败或中断时，必须区分已通过的
CTest 子集与整个命令结果，不能把子集 PASS 当作命令 PASS。

安装 SDK 的目标准入使用真实编译器导出的元数据，放在选定配置库文件旁：
lib/<CONFIG>/jce-sdk-target.json。编辑器、构建 CLI 和 CMake 在 cook 或修改缓存/
产物前消费同一 authority。系统、架构、指针位宽、配置、variant 与实际 CRT
以及 delivery 必须一致；目录改名和 VERSION.txt 不是 ABI authority。
元数据严格限定为九个字段，解码前有 16 KiB 上限；选定库文件的元数据是 SDK
variant 的唯一 authority。schema 1 是生成器所有的固定 ASCII scalar JSON；
转义键值、嵌入 NUL、重复/未知字段及不完整读取均拒绝准入。此限制不适用于
普通用户项目 JSON。元数据缺失、过大或无效
时要求刷新 SDK。元数据准入与最终产物机器类型检查、实际运行分别验收。
当前原生编辑器 release/dist 产物采用 embedded，debug 采用 authoring，
不兼容的 SDK profile 明确拒绝。

正式安装 SDK 门禁为 `test_jce_sdk_consumer_c99` 与
`test_jce_sdk_consumer_scripting`，归类为 consumer/integration。通过统一入口
显式选择前缀，SDK 的 variant/profile 与原生 Debug 测试独立：

```bash
python scripts/jce.py test --configure --arch x64 --variant debug --suite consumer --sdk <installed-sdk> --sdk-variant dist --sdk-delivery-profile embedded --jobs 1 --filter "^test_jce_sdk_consumer_"
```

未选择 SDK 明确 SKIP（77），不计 PASS；显式无效 SDK 或已声明但不完整的
后端集合应当失败。门禁消费实际安装归档并要求原生 Noop 执行；协议端口和
交叉构建属于独立证据。编辑器自带构建环境仍须单独交付和验证。

脚本可用性由安装 roster 与实际 target 图生成：JCE_SCRIPT_AVAILABILITY_SCHEMA=1、
JCE_SCRIPT_CATALOG_LANGUAGES 和 JCE_SCRIPT_COMPILED_LANGUAGES 描述内置 Lua
之外的可选语言。安装目录声明不是后端已编译的证据。严格消费检查选定 Release
依赖闭包与 compiled 集合一致，不能借旁侧 Debug 库补齐缺失的 Release 产物。
未编译的可选语言标为 UNAVAILABLE；声明元数据缺失或无效时要求刷新 SDK。

可选测试派发开关通过 `test --configure --ai-dispatch on|off` 显式选择；省略时保留缓存设置。未带 configure 时，命令在修改缓存或开始构建前拒绝此参数。注册表显示可用与实际运行测试通过是两类独立证据。

原生 `sdk --smoke` 明确传递 SDK 的 variant 与 delivery profile，并要求
C99、Scripting 两个消费者实际运行。两者的 DIST 资产共用既有的认证
依赖图、PAK 与密钥份额构建所有者，保留生产预打包准入门禁。历史上未明确
请求 variant/profile 的原始资源冒烟仅为兼容性证据，不能证明 DIST 认证交付。
交叉/Web 目标仍须另行实际执行。

夹具只在消费构建目录的新暂存项目中声明 C99 PNG/WAV，或保留准确的
`tests/sdk_smoke_scripting/assets/scripts/smoke_py.py` 原字节。脚本公共类型为 `script`，表示为 `python.source`；
路径、字节和夹具种类必须匹配。所选 SDK 工具缺失，或源、类型、图、BOM
不一致时禁止执行，链接后再次检查当前证明。Release 冒烟保留原 cook/pack
路径；切换路径前清理缓存中的预打包选择。

`tests/sdk_smoke/test_stage_dist_fixture.py` 验证真实文件暂存及类型证明协议；
`tools/tests/test_sdk_consumer_gates.py` 通过计数编译、证明和运行端口消费实际
driver，验证各阶段顺序及失败阻止启动。这些 CPU 控制不证明实际认证、链接
或执行。原生消费者仍要求 cooked 解压检查、已声明后端的生命周期成功标记
与退出成功。测试夹具通过不等于发布最小闭包、编辑器界面、项目迁移或设备
呈现验收。

已声明 Bundle 的项目使用普通命令 `app <project> --variant dist --arch x64
--delivery-profile embedded --sdk <匹配的已安装SDK>`，无需伪造 loose 启动场景。
公共 inspector 从 Bundle 内置类型清单读取场景并深验 Archive；显式 startup 优先，
未指定时按运行时声明顺序选择首个 Bundle 场景。全部声明资产参与语言与原生 registry
准入；不支持的脚本、内容/类型冲突或不完整证明会拒绝产物验收。

最终认证 PAK 包含已验证 Bundle 条目与真实 boot 场景，本次构建清空独立明文 Bundle
嵌入。Sidecar 遵循既有可选规则，不是类型权威或交付必需文件：缺失合法，存在但错误
由公共 reader 拒绝，显式 legacy 兼容仅记 UNVERIFIED，不冒称最终归档 hash 认证。
源归档、sidecar 存在状态和 inspection 快照须保持到打包、链接结束。旧 SDK 工具缺少
类型 inspection 应刷新；CPU 计数端口控制不替代真实解包、认证、EXE 搬迁及场景输入/
画面验收。

原生 Dist 内容准备与准入复用同一公共 Bundle 图。编辑器将所选 SDK stock 根
传给私有图适配器，shader 回退保留既有挂载和项目资源优先级；相对 SDK 配置须经
现有主机绝对路径 owner 解析。终态 headless 结果保留真实 producer ERROR 与依赖链。
SDK 打包器成功、编辑器构建入口成功、最终程序可运行分别验证。

Dist 名称保密声明与项目载荷安全分别验证。`jce_project.json` 中可选的
`confidential_asset_paths` 原节点在准备图前经公共有界主机 IO 与严格 JSON 读取：
清单上限 1 MiB，最多 256 个规范相对地址，每地址最多 1023 字节，地址总预算
64 KiB（每地址额外计一个终止符）。重复政策 key/地址、空或非字符串项、NUL、
通配符、逃逸、非规范拼写以及不属于选中图的地址均拒绝准入。全部项目 Dist PAK
条目仍须加密、认证、解码深验和图一致性；GUI 与单文件要求保留。显式受保护名称
在该 PAK 外出现字面量仍属致命错误。普通选中 API 地址字面量单独记入
`address_literals_outside_project_pak`，其存在不证明载荷明文泄漏或运行时消费。
公共项目保存保留原可选节点，不增加公开 ABI 字段，也不验证私有路径 schema；
原字节读取拒绝原始/有效编码 NUL、重复政策 key 和非 JSON 尾部，并保留磁盘原字节。
JSON `"\\u0000"` 这类转义反斜杠文本仍可保存。

Bundle 与 Dist 通过公共 Bundle 的 `always_included` 根及 resolver 组合，共用私有
prepared-config 适配器。已有的规范 `render_settings.json`、`physics_layers.json`、
`settings/project-settings.json`、
`settings/render_pipeline.rp.json`、`settings/input_actions.json` 成为必需根，并保留
描述文件/LUT 依赖闭包；不存在的可选文件不凭空要求。公共 VFS -> source -> resolver
优先级保留，prepared 规范回退先于项目 Settings/.jce 回退，不能覆盖规范 source
字节。manager 传入实际 prepared 根，Dist 避免重复追加 input-actions。分别验证
source/prepared 冲突、prepared/项目原配置冲突、依赖闭包，以及绑定后所选源文件消失。
项目设置使用原 `.jce/project-settings.json` 回退；物理层使用 prepared 规范文件。
不存在的可选文件保持缺省；若已存在的项目设置根被遗漏，即使场景资产均已打包也须失败。
Graph/audit/project 夹具与实际 adapter 缺线/缺根负控仅证明其 CPU 范围；仍须检查
当前 EXE 的安全条目、重开当前 Bundle，再以对应产物哈希验证迁移运行、设置、
输入与画面。

DIST stock 着色器打包向既有 PAK producer 传入 `--strip-debug-paths`，移除可恢复的
stock debug 路径表并保留 hash 寻址。Stock 仍是公开归档；该参数不加密其载荷，也不
选择最小 stock 闭包或证明体积目标。检查实际重建归档与内嵌字节；源码政策或通过的
夹具不能将旧 EXE/Bundle 改记成新产物，也不能认证本轮发布验收。

终态 headless 结果增加本轮已接受构建的私有 `log_path`、`log_status`、
`log_complete`、截断/不完整标志及独立诊断错误。有界原子日志是 owner 实际
观测的规范化文本，不是逐字节子进程流或 EOF 收据。使用前检查实际文件与
字节；失败构建的产物/审计声明路径不代表文件已经存在。诊断失败保留真实
构建状态、退出码和错误；启动被拒时不能认证上一轮日志。

已安装 SDK 的延迟脚本接线保留目标已验证的语言/工厂声明，以及原目录中的
唯一生成注册源码。必需原生元数据不能凭空创建工厂 provider；声明缺失或
不匹配仍拒绝。配置控制和 SDK smoke 后仍须验证实际项目构建与运行。

原生 stock CMake 控制需要显式包装工具：执行
`python tools/lint/run_all.py --stock-wrapper-exe <原生-jce_bin2obj-路径>`，
再执行 `python tools/audit/run_architecture_audit.py`，覆盖完整门禁两部分。
聚合器不猜测 build/dist 产物；没有工具输入时该项是 NOT_MEASURED/SKIP，
不是原生包装工具通过。真实控制还需要 CMake。`scripts/jce.py lint` 仍不提供此显式输入。

Build Profiles 用户项目默认 Dist，并保留显式 Release 选择。SDK 项目通过
CMake/Ninja 烘焙资源、配置和构建；只有引擎工作区使用脚本/Conan 所有者。
Build 将可执行文件留在构建缓存；Pack 通过既有发布所有者把验证后的 Dist
单文件发布到 `dist/apps/<name>/<platform>-<arch>-dist/`。显式 Release 包可能
包含开发附带文件。所选 SDK 的 variant/target 合约必须匹配，禁止为复用
Release SDK 静默降级 Dist。空闲时拒绝当前请求统一显示 FAILED 并清旧输出；
busy 重入保留在途请求。Play 先消费派发接受 bool，再观测构建成功。
实际函数体的 CPU 准入及旧接线负控不证明 GUI 点击或产物搬迁。

annotation 状态未知时，包 identity 增加已消费配置请求的有界规范化
JSON/SHA256，包括相关覆盖值、flags 和 user-toolchain 请求路径。不同未知
请求分别隔离；已知且等价的默认配置仍保留历史包 ID。该指纹不读取或认证
外部 toolchain 文件内容，不能认证同路径文件被原地替换后的状态。

音频依赖选择原始 miniaudio0.11.25，包含上游已发布的 WASAPI COM 引用修复。
通过正常 Conan 解析刷新并重编译所有头文件消费者；不能修改缓存头文件或用
MA_COINIT_VALUE 掩盖 apartment 不匹配。Windows CPU 回归直接消费当前选中
上游完整函数，仅将外部调用接到类型化端口：
`python scripts/jce.py test --configure --suite engine --variant debug --arch x64
--jobs 2 --target test_jce_miniaudio_com --filter "^test_jce_miniaudio_com$"`。
S_OK/S_FALSE 各自拥有一个引用，初始化失败不拥有引用。保留旧实现的普通断言
失败；头文件/函数哈希与 CPU 控制不能替代原生音频、Engine 清理和编辑器实际退出。

### 仅构建区 SDK 暂存

需要生成当前原生 embedded SDK、且不发布 dist 时，可在 `sdk` 或 `package sdk`
入口指定 `--staging-dir <仓库绝对地址/build/sdk/自有前缀>`，选择单一 release/dist。
此模式复用兼容目标缓存及既有租约，拒绝独立输出地址对、clean、交叉/Web、
authoring 和 shader OFF；源、工具链、变体、配置与着色器身份仍须匹配。
拒绝别名、重解析点和无归属或未完整安装的非空前缀。实际目标/静态库准入与
输出配置元数据共同决定复用；失败安装应保留并改用新的自有前缀。默认 dist
和既有独立输出行为不变。这不提供离线编译器，也不认证消费应用运行；实际
SDK 构建及产品身份必须与受控测试证据分开。

### 生成物目录

遵循 contracts/build-output-layout.md。复用现有绝对路径下的 native、Conan、
host-tool 和 shader 缓存。新 benchmark 证据默认写入 build/validation/benchmark，
lint/audit 门禁日志默认写入 build/reports；显式输出与历史记录保留原有语义。
不要为了每轮验证搬移 CMake 树或复制 PDB、SDK、依赖树。最终交付继续归属
既有 dist/editor、dist/sdk 和项目 dist/apps。多个测试引用同一份经验证的不可变
EXE；不能把历史工件 hardlink 到可变链接输出。已删除的证据属于不可用，不能
用当前源码重造历史 PASS。

原生 Editor/应用默认交付一个嵌入式可执行文件。standalone 保留为兼容别名。
authoring 模式须显式选择，产物放在目标目录的 `authoring/`。
默认编辑器程序位于 `build/desktop/<target>/<variant>`，新建原生 Release
缓存使用目标目录内的 `obj/` 和 `conan/`。旧兼容树经验证后原位复用。
XP 目标采用 `windows-x86-xp` 和 `windows-x64-xp`。configure 固定实际
release/dist，skip-build 须匹配真实变体与静态交付闭包。Windows Editor
的 Dist 产物须验证 GUI PE 子系统 2，Release 允许控制台子系统。

未显式覆盖 shaderc 时，native configure 先读取当前已准入 CMake 缓存中的绝对 JCE_SHADERC_EXECUTABLE，并通过既有执行检查验证。缓存值缺失、歧义、相对或不可用时仍进入原 host-tool resolver；显式 JCE_SHADERC_EXECUTABLE/BGFX_SHADERC 的优先级及空值行为不变。编译器复用不保证 shader-cache 命中：工具字节、输入字节及声明的参数/输入地址仍参与 key。

现有缓存清理会先保护已声明的项目。Editor 和 SDK 的 `--clean` 共用有界、
不跟随子链接的所有权检查，全部待清理根目录通过后才允许删除。发现嵌套的
`jce_project.json`、根路径或遍历存在歧义、条目不可读、扫描预算耗尽时，拒绝
清理。不要删除清单或移动源码来绕过保护。历史缓存中现存项目仍留在原地受保护；
此保护不承担源码迁移，也不保证并发文件系统的原子性。准确边界见生成物目录契约。
