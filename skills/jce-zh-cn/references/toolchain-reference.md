# 工具链发现

使用 scripts/jce.py --help 及子命令帮助查询支持参数。通用脚本不写死开发者的编译器、JDK、NDK、SDK、签名身份或资产位置。

Windows 构建驱动选择 MSVC 环境。Conan profile 维护依赖配置；生成工具链、配方和包身份需与目标变体及图形层级一致。不复用退役源码修补 hook 的图谱标记。

JAVA_HOME 等显式工具链输入由调用者提供。遵循 CONAN_HOME，使本地与 CI 依赖缓存可隔离。遇到未知的已安装 JCE hook 时停止同步，不能覆盖。

Android 使用已安装并核验的 Gradle；原始 SDL Java 从固定上游缓存获取，不复制进公开源码。自有平台打包实现位于 tools/build/，手动转发入口位于 scripts/。

源码导出、构建树、SDK 安装和最终分发包是不同目录。记录每条命令使用哪一个。发现编译器或通过源码检查，不能证明平台二进制已经生成。

## 当前编辑器与 SDK 构建接口

无需用户配置环境的完整内置或旁侧 builder 载荷目前尚未交付。公共 `jce_toolchain_refresh/get/set_override` 接口发现宿主已安装工具或接收显式路径；发现来源为 PATH、常见安装位置及平台 SDK 标记，Windows 还使用 Visual Studio 发现及其 CMake/Ninja 副本。Preferences > Toolchains 保存显式覆盖路径。Build Profiles 从 SDK Root 输入、项目 SDK 字段或 JCE_SDK_DIR 选择 SDK，尚不自动选择编辑器旁侧的匹配 SDK。独立编辑器内嵌运行资源，不包含编译器或 SDK。

发现查询由既有 helper 同时捕获并排空 stdout 与 stderr，避免窗口化调用者因没有标准 stderr 句柄而依赖继承句柄。stdout 使用 1024 字节的固定前缀缓冲区，最多保留 1023 字节及 NUL，另用 256 字节 scratch 丢弃超出前缀的 stdout 及全部 stderr；存储已满仍继续排空双 pipe。普通版本查询沿用 2 秒、Visual Studio 查询沿用 3 秒的协作超时。只有成功退出（exit 0）且完成排空的查询，才能提供 stdout 第一条非空行；超时、非零退出或空输出均拒绝。stderr 不能提供 root 或版本；可选版本查询失败仍保留已经核验 executable/SDK marker 的结果。此策略归属发现准入；公共 process owner 在 Windows 每次 spawn 分别解析输出流。capture 为 true 仍使用 APP；为 false 时有效父句柄（包括 file/pipe）保持继承，NULL/INVALID/stale 句柄交由 SDL NULL/NUL 丢弃。POSIX 继承不变。不改公开 struct/ABI，不创建 Console、不修改父句柄，不通过 BACKGROUND 改变监督或退出语义。缺失句柄诊断只记录有效策略及实际 spawn 状态，不包含完整参数或环境。`process.spawn` 及四个 stdio mode/reason 计数仅描述 CPU 尝试，不证明子进程完成。process owner 在 property/argv 清理前复制实际 spawn 错误；Shader Graph `compile_sc` 保留此上下文。

`test_jce_process_stdio` 使用真实 SDL property bag 及 typed Windows/process ports 编译完整 owner；无效继承必须拒绝 spawn。old-inheritance、missing-error-copy 负控须编译后普通断言失败，不作为原生 GUI 或编译器执行证据。仅包含公开 JCE 头及 Windows SDK 的 `test_jce_process_gui_stdio.c` 同时是 CMake Windows GUI target 和可复用的 installed-SDK 消费源码。默认执行要求父 stdout/stderr 真实缺失；若启动器提供句柄则明确 SKIP 77。可用 `--inherited` 模式对照启动器提供的真实句柄。实际 self-child 写双流、退出 23，覆盖四种 capture 组合、重复 poll/destroy 和 force-kill/reap；传入有明确归属的 receipt 路径，检查实际字节、退出码和状态。使用选定 installed SDK 重建此源码是独立门禁。刷新 SDK 后分别验证这些原生控制及真实 shaderc/compiler 使用；源码和计数 ports 不能证明 GUI 已修好或实际构建已经执行。

Editor Build 的私有 `jce_build_tool_probe` 继续使用公共 process/clock owner，
在每次 256 字节 pipe read 前、完成准入前及 cleanup 后检查同一 2 秒协作期限。
stdout/stderr 公平轮询，分别最多保留 1023 字节前缀；合计实际观察输出超过
64 KiB 明确拒绝准入。只有观察到 exit 0、完成排空且 stdout 有非空首行的查询
能提供版本；stderr 和复制的 spawn 错误是分别保留的有界失败诊断。
每个成功 spawn 的 handle 都经过公共 destroy/kill/reap，即使显式 kill 请求失败。
此期限不能硬实时约束同步 spawn 或操作系统 cleanup 调用。
静态名称的规范 CPU query span 记录 outcome、字节、截断、耗时及 kill 接受状态，
不证明编译器构建成功。`test_jce_build_tool_probe` 通过计数 process/clock ports
编译完整实际 owner，并消费真实 trace；同源 deadline/stderr/output/exit/cleanup/final-drain
接线负控须编译后普通断言失败。GUI 工具、完整 builder 及原生子进程清理分别验收。

GUI 与 headless ProjectBuild 复用公共发现的 Visual Studio root，核对 `VC/Auxiliary/Build/vcvarsall.bat`，并在同一子进程环境中初始化 MSVC、配置和构建。未取得有效 root 时的 PATH 回退仍依赖宿主已配置的安装环境，不能证明完整 builder 载荷已交付。发现控制与刷新后的 SDK/编辑器实际构建运行分别验收。

通过支持的测试入口验证实际发现 owner 与旧 stderr 接线负控；已有兼容测试树可复用：

```bash
python scripts/jce.py test --suite engine --target test_jce_toolchain_override --target test_jce_toolchain_override_old_stderr --filter "^test_jce_toolchain_override(_old_stderr)?$"
```

SDK 用户项目复用现有原生 build manager，需要 CMake/Ninja；Conan 用于引擎工作区的依赖构建，普通已安装 SDK 消费不依赖 Conan。Windows 项目构建使用 MSVC 兼容工具；x86_64、i686、aarch64 是可选目标，ARM32 明确拒绝。macOS 接受 x86_64/aarch64，原生 Linux 当前只接受宿主架构。可选项不证明对应编译器或载荷存在。安装 SDK 提供公开头、库、CMake helper 与构建宿主资产工具；资产工具不是完整编译环境。

SDK 准入在清理或烘焙前检查选定库旁的 `lib/<CONFIG>/jce-sdk-target.json`，核对真实编译器导出的 system、architecture、pointer size、configuration、variant、delivery、CRT 及库身份。GUI debug 使用 authoring，release/dist 使用 embedded；元数据缺失或冲突时要求刷新匹配 SDK。准入、编译器发现、源码门禁及 SDK 安装成功，都与生成并实际运行目标应用分别验收。参见 [SDK 消费与分发](user-project-sdk.md) 和 [构建门禁](build-and-gate.md)。

## 完整目标载荷与目录职责

零环境交付需要可在构建宿主运行的工具，以及每个承诺目标的完整工具链：包含模块的 CMake、Ninja、选定 C/C++ 编译器、链接器及平台资源工具、目标 headers/sysroot、系统导入库/静态库、CRT/C++ runtime、宿主 cook/pack/embed/shader 工具和 ABI 匹配 SDK。Windows MSVC 兼容候选需要 Windows 与 MSVC runtime/library 闭包；GCC/Clang 目标需要对应 sysroot/runtime；Android 使用 NDK，Web 使用 Emscripten，Apple 目标使用平台 SDK。已有 MSVC 库不能改名冒充 LLVM-MinGW/Zig 兼容库。核验实际载荷的编译器 ABI、目标架构、CRT、来源及再分发许可。缺组件或不支持的目标应明确不可用并提供具体构建错误；宿主 PATH 回退不能证明零环境交付。

继续复用现有 build/cook/package 所有者。原生项目缓存位于 `<project>/build/<platform>-<arch>/`，链接产物位于其 `<variant>/`，中间对象/生成内容位于 `build/<variant>/`，收据位于 `reports/`。最终应用使用 `<project>/dist/apps/<app>/<platform>-<arch>-<variant>/`；SDK 安装与编辑器交付仍是不同产品。现有 Dist publisher 先审计编译后的程序和归属明确的 reports 候选，再通过宿主原子 rename 发布；失败保留旧产物，未知输出内容拒绝覆盖。工具、缓存、临时候选和报告不进入最终应用目录。零环境交付须在仅保留操作系统环境的隔离条件下编译、链接、运行确切目标，并迁移验证发布的应用。

## Stock runtime 与显式密钥重定位

已发布的 stock runtime 可连同未改变的 manifest 及经过核验的相对文件一起移动。
`stock-app seal --pak-key-file` 仍要求调用者显式提供外部密钥；有界读取后必须由
恰好64个十六进制数字解码为32字节，并匹配已核验 producer/protection 元数据绑定的
SHA-256。记录中的原生产者密钥绝对路径只作来源追踪，不要求调用者的相同密钥仍在
该位置。工具执行后再次核验解码密钥身份。错误、格式无效或执行中改变的密钥仍拒绝
封装；不能把原始密钥内容写入日志或公开收据。

重定位不放宽 runtime/工具哈希、目标与语言准入、选定 Bundle 身份、认证加密外层
归档的 deep verification 或产物发布检查。使用未改变且已移动的 runtime/密钥完成
seal，与实际运行最终单可执行程序分别验收；两者都不代表完整编译器、目标 sysroot/CRT
或无需环境的原生项目 builder 已交付。

## 选定归档准入

Dist SDK 消费项目通过公共 Bundle 打包器的 cooked 场景图选取资源，包括显式动态根和递归依赖声明。`jce_pak --selected-archive` 读取一次不可变 blob，使用公共 Bundle/Archive owner 打开，跳过保留的 Bundle manifest，并深校验最终归档。同一逻辑地址只有字节相同才能合并；不同字节必须失败，不能任意覆盖。输出 BOM 记录选定来源及认证深校验。stock 资源暂为明确的安全超集，不能据此宣称全局最小资产或运行一致性。公共 loader 保留 legacy manifest 兼容，因此独立工具准入不等于现代 typed graph 证书。真实消费控制与编辑器/EXE/重新打开 Bundle 的验证分别验收。

离线选定 Bundle 检查必须使用明确、精确的构建目标，不能使用工具程序的宿主替代。共享 driver 将 delivery.bundle_platform 的结果作为 --bundle-target，同时传递内容类别 --platform。Web 默认精确 web；旧 desktop 默认保留编译宿主；typed mobile/console 缺少精确目标则拒绝。类别与目标冲突时拒绝。公共 jce_bundle_file_inspect_memory/file 不挂载文件系统，复用 manifest、依赖、payload 和 sidecar 校验；runtime/catalog API 永远按编译平台准入。请求 auto 只接受 portable auto 内容；即使内容 portable，也拒绝未知的 expected token。Windows 宿主为离线打包接受 Web 内容，不构成浏览器运行证明。

检查器借出的 Pak 会被不可逆地标记为仅检查。标准 SDK 的具名/旧 FS 挂载和 Pak fallback 接入均拒绝该同一句柄；通过旧 public setter 恢复 canonical 三函数 tuple 时仍保留 guard。未知 opaque 自定义 provider 保持原 ABI，不会被解释为原生 Pak 布局。自行解码或重新打开独立字节不属于此准入合同，不能宣称检查器提供安全沙箱。实际 Bundle 检查挂载 fixture 与遗漏 mark/mount/fallback 的同源普通断言负控分别验证这些 CPU 路径，不能代替浏览器运行证明。
NULL-provider 的具名/旧 FS 挂载同样拒绝，避免先挂载再恢复 canonical 后可读。Pak 准入使用有限循环检测；fallback push 在修改前以线性遍历拒绝相交/共享尾链。实际 no-provider/shared-tail 同源遗漏负控必须普通断言失败，超时或崩溃不算通过。
