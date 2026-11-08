# SDK 消费与分发

未封装的 stock runtime 是生产组件，不是应用交付物。固定消费构建缓存放在
build/，按架构分发的 runtime package 与最终 dist/apps 可执行文件分开。
免编译器 Lua/JavaScript 封装必须检查 typed Bundle 合同，拒绝 native 或外部 VM
需求，只消费经过验证的打包内容。普通应用构建仍要求合法 startup scene，不能
用假场景让未封装 runtime 通过。哈希与损坏检查不等于认证；构建成功和封装回执
不证明实际运行、迁移或零环境 C/C++ 工具链。

实际可执行映像路径使用 jce_fs_host_get_executable_path，不依赖 argv[0] 或当前
工作目录。不支持的平台返回 false；Windows/Linux 实现不证明其他平台支持。

Stock payload版本2将验过的精确Bundle字节放入现有公共secure PAK，唯一entry为
stock/project.jbundle。检查加密/认证flags、认证有效、唯一性、预算与源字节一致；
envelope的FNV仍只是损坏检测。原生reader保持解密字节有效直到所有runtime/Bundle
借用者销毁。显式producer target运行八项CPU控制；源码fixture与实际GUI运行另验。
当前seal必填--pak-key-file，绑定producer owned-cache路径与哈希。原始key不复制
到发布组件或应用目录；这还不是零配置builder分发。

消费端包含 JCE 公开头并链接匹配的安装 SDK，不包含 engine/src/ 或直接使用第三方头。引入引擎功能前检索公开 API 与组件配置。

```bash
python scripts/jce.py sdk --arch x64 --variant dist --no-debug
python scripts/jce.py smoke --arch x64 --variant dist
python scripts/jce.py package editor --arch x64 --variant dist
python scripts/jce.py package editor --arch x64 --variant dist --standalone
```

受控原生 embedded SDK 构建可使用独立缓存与安装目录：

```bash
python scripts/jce.py sdk --arch x64 --variant dist --build-dir build/fixtures/sdk-without-stock --install-dir dist/sdk/fixture-without-stock --embed-engine-shaders off
```

显式 OFF SDK 缺少内嵌 stock shader 能力；消费方提供所需 shader 原字节后，
可用于资产准入与恢复控制。默认 SDK 继续内嵌 stock shader（ON）。配对的
自定义路径位于 build/、dist/sdk/ 下，独立于所有共享/默认缓存及安装目录。
自定义 `--clean`、authoring/Web/跨架构选择与路径、owner、缓存、已安装目标
元数据冲突均在 producer 修改前拒绝。匹配的失败配置请求可重试。
请求标记不能代替原生运行证明。

用户项目使用受支持的 cook、app、package、accept 子命令，并显式传入项目目录。CMake、清单、图标和专用工具随项目放置。选择参数前阅读各子命令的 --help。

公开头改动需要刷新请求覆盖的 SDK 并重建消费端。已安装头属于构建产物，不能手工修补。库、头、语言适配、图形层级和变体应来自同一配置。

安装 SDK 和无需额外运行时即可启动的编辑器，并不提供零环境编译器。参见 [当前工具链发现与完整载荷要求](toolchain-reference.md)；目标/CRT 准入不证明已交付自带构建工具。

dist 使用 Release 编译，关闭 Tracy 和专利编解码路径。release 与 dist 是不同产品，不能将旧可执行文件改名为 dist。核验包内二进制和运行依赖，不能只看 preset 名称。

资产名称保密可在同一 `jce_project.json` 清单中选择性声明：

```json
{
  "confidential_asset_paths": ["materials/player.mat.json", "scripts/player.lua"]
}
```

每个声明地址必须已是选中 Bundle/Dist 图中的规范相对地址。私有 Dist 边界使用公共
主机 IO/JSON：清单上限 1 MiB，最多 256 个地址，每地址最多 1023 字节，全部地址
含各自终止符的总预算为 64 KiB。重复 key/地址、非字符串或空项、NUL、通配符、
逃逸及非规范拼写均拒绝；不展开模式，也不静默选择无关文件。省略节点或空数组
表示未声明保密名称。无论此节点如何，项目 Dist PAK 载荷仍须加密、认证、深验和
图一致性，GUI 与单文件检查也保留。显式受保护名称在项目 PAK 外出现字面量仍使
审计失败。公共字符串寻址继续支持；普通选中地址字面量属于单独观测，不能证明
载荷明文泄漏或运行时消费。公共项目保存会在修改无关字段时保留原可选节点，不
新增 ABI 字段或另建路径 schema；原始/有效编码 NUL、重复政策 key 与非 JSON 尾部
拒绝保存并保留原文件，转义反斜杠文本可保存。

Bundle 与 Dist 组合同一既有公共 Bundle 根/resolver，收录已存在的
`render_settings.json`、`physics_layers.json`、`settings/project-settings.json`、
`settings/render_pipeline.rp.json`、`settings/input_actions.json`。
这些选中根携带描述文件/LUT 闭包；可选配置不存在是合法状态。公共 VFS -> 规范
source -> resolver 优先级保留；prepared 规范回退先于项目 Settings/.jce 回退，source
字节优先于 prepared 字节。Dist manager 传入实际 prepared 根并避免单独重复添加
input 条目。核验实际 EXE 与 Bundle 中内嵌配置字节及依赖资源，编辑器重开 Bundle，
并运行迁移后的应用；配置声明与 CPU 控制不能认证执行中的渲染/输入行为或本轮
发布验收。

DIST 引擎 stock 着色器 PAK 通过 `--strip-debug-paths` 去掉 debug 路径表并保留 hash
寻址。Stock 归档仍公开；去除名称不加密其载荷，也不表示已选择最小闭包。实际
重建 PAK/EXE/Bundle 的哈希与清单应与源码政策、SDK 体积及保留的构建证据分别记录。

默认交付是轻量、自包含的原生单应用：内嵌 Lua 与 JavaScript，并支持原生 C/C++ 应用代码。Python、Java、C# 保留为可选创作/SDK 集成，明确说明外部运行时要求。不能为了声称内嵌七种语言而将这些 VM 或 SDK 整包加入交付程序。项目必需脚本必须在目标宿主正确解析；驱动或模块缺失时明确失败，不能静默丢弃行为。

验收需将唯一交付程序搬到空目录，以全新用户状态启动，确保无源码树资源回退，并实际查看代表性截图。记录可执行文件哈希、源码身份、变体、平台及测试范围。核验实际内嵌资产/许可与常规、延迟导入。Windows 证据不能直接证明其他平台。

当前 Windows 原生单 EXE 配方使用 `--standalone`，选择静态 CRT 依赖图及内嵌资产，支持 Lua/JavaScript 与原生 ABI。原生 C/C++ ABI 可用不等于项目脚本类已静态注册。`.jcec` 或 `.jcecpp` 路径是类名引用；将描述/标记文件打入内容 bundle 不会产生类注册表。可选原生模块开发测试独立保留；验证实际静态注册，或使用保留行为且目标支持的可移植脚本变体。交付不能依赖旁置项目 DLL、外部 VM、SDK 安装或自解压包装。

运行时模型使用标准 glTF/GLB。烘焙/打包前转换并核验受支持的编辑器源格式及相关依赖资源。项目政策和工具随消费项目放置，不能将游戏专用规则加入通用引擎或编辑器。

编辑器与用户项目的单可执行交付是引擎宗旨。Web 出货为一个内嵌 wasm、
启动逻辑和资产的 HTML；交叉目标探测不能消费宿主 JNI 头文件。Windows
静态 SDK 复用编辑器静态依赖闭包，SDK 的 CRT 属于安装后二进制契约。
检查普通与延迟导入，并把唯一应用文件搬到空目录运行；运行时生成的设置
不是交付依赖。原生脚本入场配置不能仅凭源码声明认证，必须保留编译后的
控制器实际行为和成品身份。

## 构建与交付目录职责

中间对象、生成代码、链接输出及验收证据放在 build/；共享报告也可放在 reports/。SDK 安装目录 dist/sdk 属于开发产品。编辑器最终应用放在 dist/editor；消费应用使用明确的 dist/apps/<项目>/<平台-架构-变体>/ 目录。应用目录仅包含目标平台要求的程序或应用容器；原生单文件与 Web 单 HTML 交付只包含该文件。哈希、清单、日志和抓图保留在应用目录外。编译与交付整理是两个步骤；最终输出不能指向构建缓存，也不能递归复制整个链接目录。先查看帮助，再使用受支持的 --out/--artifact-dir 参数；显式整理步骤必须记录，并用空目录迁移启动验证整理后的确切字节。运行时生成的缓存与设置另行记录。并行维护者应随实现变更即时同步对应的英文、中文 skill 参考，修改共享段落前协调职责。


Bundle 交付使用一个 `.jbundle` 文件，包含选定场景、脚本与资源依赖闭包。它是内容包，不是引擎或 SDK 分发包，也不是独立可执行应用。app 构建工具将项目清单的 `bundles` 列表传给已安装 SDK 的嵌入工具；相对路径以项目为基准解析并保持列表顺序。因此标准项目模板可将 Bundle 嵌入最终单可执行程序。必须分别验证编辑器重新打开和仅含 EXE 的迁移目录；仅打包或构建成功不能证明完整玩法或所有目标平台。

Windows DIST 应用默认使用 GUI 子系统，启动不得创建控制台窗口。预打包 PAK/bundle 的提前返回前必须应用目标政策，不能因嵌入路径绕过。验证实际 PE 子系统值（2=GUI），不能只看 CMake 声明。CLI 开发工具保留明确的控制台政策。

内嵌交付将有类型的着色器程序名解析为实际编译的后端文件。内置依赖使用由内嵌着色器包校验的已安装 SDK stock 目录，项目自有文件保留优先级。缺失的用户程序仍须准入失败；一个可用变体只证明对应后端，不代表所有目标平台均已验证。

静态 C/C++ 场景脚本使用 `jce_script_enable(... NATIVE_FACTORIES provider)`。
项目清单声明 `native_script_factories` 与 `native_script_refs`；后者将选中资产的
规范地址绑定到原始类引用拼写，保留大小写。SDK 生成的注册器强链接模块工厂，并
检查实际类解析。SDK CLI 交付需要迁移后可执行程序产生的真实回执；Web 首次构建
写出 `build/native-registry-proof/web-request.json`，必须在实际浏览器执行后通过
`--registry-proof` 回填对应回执，才能通过准入。

塔防 SDK 消费源及专用验证工具位于 `tests/consumers/td_lab`；
`examples/td_lab` 仅保留 Bundle 目录、原作协议和来源。缓存和烘焙数据
继续忽略，最终 GUI EXE/单 HTML 单独放在 `dist/apps/td_lab`。迁移时
不得把历史产物或截图身份改写成最终源版本的证据。

原生 build-project 与兼容入口 package game 都委托编辑器既有 headless
构建/发布所有者。最终默认布局为项目自己的
dist/apps/<app>/<platform>-<arch>-<variant>，与构建缓存及结果记录分开。
启动所有者前 resolve 并拒绝受保护源码/build/SDK 路径及未知已有内容，
原生所有者在发布时再次检查。不得递归删除任意输出或自动迁移旧 dist/games。
旧 loose-file 交付和不一致 name/version 覆盖须明确拒绝，不能另建多文件发布链。

Windows schema-1 SDK 的生成与消费均要求 CMake 的 MSVC 兼容前端模式；
允许 cl/clang-cl，保留既有目标、配置和 CRT 校验。GNU/LLVM-MinGW 模式明确
以 SDK_ABI_MISMATCH 拒绝；架构相同不证明当前 MSVC 静态库可被消费。
这类工具链需要另行生成兼容 SDK。无编译器的前端身份模型控制不代表真实
编译、链接或运行验证。

原生与 Web SDK 在安装、裁剪、必需宿主工具及元数据完成后，沿用 SDK 既有租约
发布 `jce-sdk-product.json`。`tools/build/sdk_product.py` 将实际生成的版本/API、
schema-1 目标、语言配置和安装名单绑定到规范相对地址、文件大小与流式 SHA-256；
缺失文件、链接、路径逃逸及可观察的源变化须拒绝。`verify_product` 只读并支持
目录迁移。此原子发布且未签名的文件身份清单排除自身；失败后保留的旧清单必须
重新验证当前字节。`build_payload_available`、`compiled_abi_verified`、
`execution_verified` 均保持 false，不表示已附完整编译器/sysroot/CRT 环境，也
不能据此开放零环境 GUI 构建。CPU 受控命令端口测试与实际编译、运行证据分开。

Bundle Browser 默认生成便携内容（自动）。需要指定目标的纹理或其他烘焙表示时
才启用烘焙。实际打开使用当前编辑器平台，检查目标用于独立的兼容性检查。
打开资源包与打开场景都将独立 .jbundle 交给隔离加载器。浏览使用宿主原生文件
选择器并打开选中的独立资源包。在打开页粘贴绝对路径后可按 Enter 或点打开，
也支持从资源管理器复制的带引号路径。

Bundle Browser 的 Open 页提供明确目标的资源包检查和已保存内容快照检查，
独立于宿主运行时加载场景。资源包检查保留公共加载器对存在的附属清单的核验；
快照首次最多读取 64 MiB，之后复用持有的字节，只核验该内容而非后来的磁盘
附属清单。两者沿用异步所有者，不改变当前场景或 VFS。源码控制与界面可达性
证据分开；编辑器实际操作仍须使用重新构建的应用验证。

显式的仅构建区 SDK 暂存前缀可通过 `sdk` 或 `package sdk` 的
`--staging-dir <仓库/build/sdk/自有前缀>` 选择，复用兼容的原生 embedded
目标缓存并保留默认 dist 输出。消费应用必须显式选用该已验证前缀。这不嵌入
编译器/sysroot，也不表示编辑器已能零环境构建；准入和失败安装的保留行为
参见 build-and-gate。

公共宿主工具支持读取单个选定资产，无需解包整个项目：
`jce_pak --inspect <archive> --read-entry <logical-path> --out <fresh-file>`
沿用现有资源包目标/密钥准入、CRC 校验与独占输出创建。选定条目的解码输出
上限为16 MiB，这不是整个资源包的内存上限。拒绝不安全、缺失、歧义条目、
覆盖输出及不支持的零长度 codec/crypto 布局；普通空数据通过映射条目准入。
不能把 `jce_archive_read` 返回零当作成功，旧接口在错误时也返回零。
此检查不挂载场景、不修改资产，也不认证编辑器或运行时消费。

Bundle 来源记录应以字节哈希绑定选定的创作场景、脚本、渲染管线设置及暂存配方。
交付与创作结果不同时，应检查实际选定条目；仅有包哈希不能表明其中采用哪份
源码配置。重新打开 Bundle 与迁移后的程序消费须分别验证，保留先前失败，
区分手动退出和测试时限保护退出。创作清屏政策及抗锯齿变化属于内容决策，
不能据此认证同画质性能提升。
