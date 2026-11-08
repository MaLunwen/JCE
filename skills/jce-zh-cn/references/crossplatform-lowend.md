# 平台与低端要求

最低兼容性由架构、操作系统 API/ABI、CPU 指令集和图形能力共同决定。
明确区分声明下限、实际工具链与依赖下限，以及真实设备验证。更低配置必须
一起验证运行时、分配器、脚本 VM、资产和渲染回退；编译宏不能降低既有缓存
归档的要求，也不能证明应用能启动。Windows XP 及更旧 Linux/macOS 配置，
须在完整所选依赖图实际运行后才能认证。

默认 Win7+ 目标保留 MSVC；显式 XP 旧系统路径使用 LLVM-MinGW-XP
GNU/MSVCRT，x86 目标为 NT 5.1，x64 为 NT 5.2。两条路径都保留 C99
引擎／运行时和 C++20 编辑器。完整 XP 编辑器、GPU 和物理低端设备验收仍在
进行中；实际核心、OS 和资源探针运行不能认证完整应用。参见仓库的
[Windows 目标策略](../../../contracts/windows-targets.md)。

在仓库根目录运行 `scripts/windows/build-editor.bat --xp` 构建 XP GUI 候选。
默认架构为 x64，面向 NT 5.1 时使用 `--arch x86`。工具链根目录按
`--toolchain-root PATH`、`JCE_XP_TOOLCHAIN_ROOT`、仓库同级的固定版本发行包
依次查找。脚本通过共用构建驱动查找 shaderc，并刷新本机构建用的 PAK 写入器
和二进制嵌入工具。候选程序位于
`build/desktop/windows-<arch>-xp/dist/jce_editor.exe`。
与目标配置一致、已登记所有权的旧构建中间产物可以原地复用。
发布时用相同架构和工具链根目录执行 `python scripts/jce.py xp-editor --phase audit`，
并通过 `--export-inventory` 提供目标系统实例的导出清单。

公开的图形兼容策略追加枚举值 4。stable、modern、current 和旧 COUNT 哨兵保持
0/1/2/3。现有 bgfx 兼容包继续使用内部编号 3。
GL 2.1 / GLES 2.0 版本准入与 GPU 画质层级分开，不能认证画面目标、uniform／
sampler 上限、shader 链接或编辑器可用性。shader-keyword 契约生成同一份
stock-profile 清单，由运行时加载与内置 shader cook 共用；普通完整模式保持
不受清单裁剪。兼容 GL/ES 使用仅静态的子集，排除 cookie 数组及 3D LUT 调色。
作者设置仍保留，请求不支持的效果时给出诊断；在真实回退得到验证之前，被
排除的 shader 家族需要更高 API 层级。须验证所选驱动上的 Scene／Game 实际
画面、输入及重复生命周期。CPU 所有权测试和 GLSL 编译是独立证据，均不能
证明整帧性能、真实低端设备验收或功能对等。Vulkan 兼容策略数据也不等于
已编译后端认证。

可选 Windows 线程描述从已加载的 KernelBase/Kernel32 动态查询，缺失时保留
线程 ID 名称。默认 Windows 桌面 PhysFS 明确选择 Win7 API 路径，并用包身份
区分旧缓存；配置变化必须产生新的依赖二进制。单 EXE 准入检查普通与延迟
符号导入中的四个已知较新 API。序号导入覆盖未知，此有限检查不认证全部
Windows API，也不能替代目标系统运行。
显式 XP producer 还要求精确目标系统的名称／序号导出及转发链覆盖，才能通过
静态交付准入；该审计仍不能替代完整编辑器运行或真实设备的渲染验证。

AGENTS.md 定义兼容目标；CMakePresets.json 与 conan/profiles/ 定义实际构建配置。声明某个目标不能证明工具链或设备已验证。

低端设计基线为图形桌面、单核 CPU、512 MiB 内存、2008/2010 年代的老旧集显，
无独立显卡。它覆盖可用的轻量编辑器及渲染运行时，也覆盖核心和无头服务。
PS2 是资源预算类比，不代表设备支持或硬件等价。复杂场景和高级功能按明确的
工作负载/层级预算衡量，不能默默将低端基线改为仅适用无头核心。记录实际高端
开发机配置，并区分真实低端设备验收与现代机器上强制低配置的结果。

桌面、移动、游戏主机及 x86/x64/ARM/ARM64 是兼容目标。默认驱动语言为 Java、
Python、Lua、C、C++、C# 和 JavaScript，通过同一原生 C ABI 接入。编译器、许可
主机工具链、设备和分发证据与清单声明分开；缺失的后端或设备仍属于缺口。

平台代码置于 OS 适配层后方，并通过公开能力查询使用。硬件加速、编解码器、脚本运行时或高级渲染功能不可用时，提供确定性的回退行为。

验证请求覆盖的平台，记录编译器和运行时最低版本、硬件、后端、内存与墙钟证据。缺失的交叉构建和设备单独报告；本机缓存构建不能证明源码可移植。

不能修改第三方代码来绕过平台构建问题。使用批准的上游版本、第三方树外的自有适配，或依赖支持的生成配置。

Windows 原生 D3D11/D3D12 Auto 先尝试 DXGI 高性能顺序中的首个硬件适配器。只有 LUID 与后端经典枚举中首个对应厂商和设备键精确匹配时才使用。接口缺失或候选无法安全表示时，转入显示适配器与驱动回退。首选设备初始化失败时先释放该次尝试，再按旧策略重试一次。

显示回退由私有平台实现通过 GetMonitorInfoW 将
SDL HMONITOR 转为 GDI name，再由活动 Windows CCD source-name packet 得到
source LUID，最终用固定 bgfx 的 classic 协议将唯一 source LUID 解析为选卡键：
CreateDXGIFactory1 使用 IID_IDXGIFactory，再经 EnumAdapters / GetDesc；GetDesc1
只提供 software flag。相同键的不同 LUID 保持独立；仅当 bgfx 首个匹配键正好是
CCD candidate 时接受，否则以 backend_key_mismatch 拒绝，不能盲选或去重。
混合显卡驱动上的 DXGI Output.HMONITOR 不能作为物理显示输出归属；生产 owner
不查询该字段。source 与 target LUID 保持独立；bgfx 的选卡输入仍是 vendor/device。
两次有界 path/mode 快照和重复 window/GDI identity 检查拒绝观测到的拓扑变化。
source LUID 歧义、错误或歧义名称、间接或未知输出、CCD 不可用、枚举不完整以及超出固定
bgfx `Caps::gpu` 可选范围时，保留原回退并记录明确原因。每个快照最多重试 4 次，
最多 1024 paths / 4096 modes，并检查 250 ms 实际经过时间；该检查不能中断 OS
调用。已取得的 buffer/library/COM 资源逐一释放。每个渲染器创建查询一次，后端
尝试复用结果，不每帧枚举或热迁移。显式 vendor、非 D3D 和无头路径保留原策略，
不增加依赖或公开 ABI。拒绝时仍保留已验证 CCD source 身份与计数，并独立标注
backend-expected LUID/index 预测；预测不等于实际 selected GPU，仍须单独验证
Native caps 与进程身份绑定的 GPU LUID 观测。

当前 Windows PE 策略仍导出 `NvOptimusEnablement=1` 和
`AmdPowerXpressRequestHighPerformance=1`。请分别核对启动日志中的首选渲染设备、显示输出设备和实际设备。Copy-engine 流量需单独采样。公开 `integrated` / `discrete`
目前分别是 Intel / NVIDIA vendor 别名，按硬件拓扑选择类别仍是待修缺口。
核对实际 selected caps 和目标 PE，不能把别名当作实际显卡类别。

`test_jce_window_adapter` 通过计数 SDL/CCD/library/COM/clock/heap ports 消费完整
私有观察 owner；`test_jce_renderer_display_adapter` 消费实际 init 策略、两个容量
边界索引以及 source / backend 预测启动诊断。wrong-LUID、旧 DXGI-output、
old-strict-key、blind-first-key、old-device 和 missing-wire
负控须编译后产生普通运行时断言失败。
这些 CPU 控制不能充当 Native GPU 正向证据。原生测量另行统一物理显示器、
后端、实际呈现策略与工作量；GPU Engine 计数本身不能确认 PCI 传输或物理
GPU 路由。

渲染器启动时的临时顶点／索引池预算：普通 Native 为 8/2 MiB，Native 低内存
和 Web 为 6/2 MiB。多线程 Native bgfx 拥有两组 Frame 池；这属于逻辑容量，
不能等同于后端缓冲、进程工作集或私有提交量。大型几何／UI 工作量可以通过
JCE_TRANSIENT_VB_MB / JCE_TRANSIENT_IB_MB 显式覆盖（整数 MiB，1..256）。
同一启动 owner 将 uniform 命令缓冲初始容量设为每 encoder、每 bgfx Frame
128 KiB，保留原有 4/16 encoder 并发策略和后端按需增长；这不是 uniform 数据
硬上限。16 encoder 与两个 Native Frame 的逻辑初始容量为 4 MiB，相比后端
每份 1 MiB 默认值理论减少 28 MiB。这一计算及启动时的
renderer.uniform_initial_encoder_frame_bytes 计数器均不测量总 heap、私有提交量、
RSS 或 GPU 驻留。实际 apply owner 与同源删除赋值负控属于 CPU 接线测试；
负控必须产生普通运行时断言失败，崩溃或编译失败不能充当验证。验收仍须检查
后端增长、容量耗尽和实际画面。

静态基础几何植被默认按参数变化更新持久根矩阵缓冲，CPU 与 compute 路径均适用，
避免每个视口重复上传矩阵。`JCE_PERSIST_FOLIAGE=0` 保留显式临时缓冲诊断选项；
compute 剔除仍要求持久根矩阵。数量、哈希和 compute-read 能力共同决定重用身份；
替换上传失败时保留旧资源所有权，但不把旧内容绑定为当前内容。CPU 矩阵发布和
原有植被投影路径均要求内容有效。可见、启用的植被投影字段必须在 CSM 缓存判断
前准备好；位置内容、几何、数量和就绪状态变化必须使旧深度失效。验证冷启动帧、
参数修改、禁用／删除字段和有界缓存流式提交；不完整提交不能形成可重用级联。
非分块模型深度必须保留节点变换和受支持 primitive 覆盖；分块驻留及同路径外部
资产更新仍遵循明确的既有策略。这些 CPU 所有权控制不能替代实际双视口 GPU
画面与容量耗尽验收。

仅在捕获时记录的渲染快照区分编码器请求容量、实际容量和逐帧已取得句柄数，
后者包含默认／API 槽。适配器 vendor/device ID 和原始 bgfx renderer type
属于后端报告元数据；原始类型不是公开 JCE backend 枚举。缺失 caps 保留 -1，
已报告的零值保留零。这些稀疏 CPU 观察不证明 GPU 完成、每帧峰值或瞬时线程并发。

画面目标退役按真正的中央提交调用次数推进，视口闲置时也推进。其私有计数与
用于 readback 的后端原始帧号分开。进程工作集可能包含可共享映射页，映射区域
大小不能当作驻留内存。用拥有明确进程身份的观察归因映射驻留页，同时保留整
进程总量；不得减掉驱动缓存页后宣称达成 512 MiB 物理设备目标。

引擎在发布默认任务预算前解析当前生命周期的有效低内存策略，
使用现有共享帧任务池，结构化异步执行器消费同一预算，不增加第二调度器或配置镜像。
LOW 默认一个 worker，普通自动档为核数减一且最多八个；显式 worker 设置保留
各 owner 原有上限，Web 保持协作执行。公开机器类别/worker getter 保留作者设置。
验证实际 worker 数、初始化失败重启和旧显式设置的清除；该预算不限制所有进程/
驱动线程，不绑定 CPU，也不能把未限速的隐藏渲染循环当成实际呈现 FPS。

Readback 队列绑定实际 renderer 的后端生命周期。析构经中央提交 owner 推进，
原始帧号、提交 ordinal、画面目标退役和实例上传推进必须保持一致。最多尝试
八次真实推进；停滞或恰好半范围歧义时保留目标，等后端停止后释放。共用的
uint32 序号判断处理 UINT32_MAX／零回绕；相关观察必须相隔少于 2^31 次推进。
停止前退役句柄并分离队列，bgfx shutdown 后释放借出的 CPU pixels，且不再调用
GPU API；分离的 wrapper 可以在重启后析构。仅在状态变化时输出的 trace 计数
描述服务、队列和拥有的 CPU 目标／字节，不能当作 GPU 驻留。不得把滞后 stats、
frame FLUSH、截图 CPU worker flush 或 D3D11 异步 Flush 当作整设备 fence。
分别保留 API submitted 帧、provider latched 源帧和特定 readback 就绪状态；
没有后端契约与实机证据时，presented／整设备 completed 仍不可用。旧 leakstats
明确是停止前的后端句柄快照，不能用非零直接判定泄漏。

共享纹理 cook 对 Web 与可移植 AUTO 默认使用 RGBA8；桌面 BC 与移动 ASTC
默认策略保持。ASTC 是可选 WebGL 扩展，不能假定所有浏览器设备支持。每资产
显式格式覆盖仍是作者请求，须由实际目标能力支持。CLI 缓存版本 3 与 Bundle
writer 版本 4 使旧默认格式产物失效；两实际 producer 的增量重建／重用须单独
验证，不能只测策略 helper。RGBA8 不做 GPU 块压缩；Archive 压缩与传输大小
不等于纹理驻留或上传成本，仍须重新测量实际绘制工作量。

实际 cooked 解码／上传 owner 记录拒绝阶段、作者路径哈希、cooked source hash、
格式、后端、能力是否可观察以及 payload 大小。CPU 对象把身份传给上传，worker
不查询 GPU caps。每次获准的失败请求增加一次警告及关联的 FAILED 服务 span；
既有 scene/UI 缓存负责重试准入。公开直接 cooked 上传与借用内存解码可能没有
作者路径身份；哈希仅供关联，不能当作认证。关闭 trace 时不写 ring，不增加
第二缓存或逐帧诊断循环。CPU 控制通过完整纹理 TU、真实 cooked/Pak 字节及
同源删除 cap 日志／身份接线负控验证 API 行为；它们不能证明实机格式支持或画面。

Web 样本查询兼容由私有平台 C／JS 边界负责，随 SDK 安装且仅链接 Web 目标。
实际 Emscripten proc-address 指针经已验证 linker wrap 进入适配，保持上游原样。
合法 renderbuffer SAMPLES 查询仍使用浏览器实际数量；空列表使拥有的首项保持零。
非法请求保留 GL 错误语义，不发出浏览器非法查询。RGB／RGBA 查询别名及已启用的
float／half／norm16 扩展不能误拒；存储别名不等于查询别名。单个当前 context
缓存在查询观察到丢失时清除，下次有效观察或对象替换时重新观察。VERSION／RENDERER 字符串是实际观察，
不能替代 API 最低版本准入或 GPU 完成。完整 C／JS CPU 控制及真实指针探针与
installed-SDK Web 构建、实机浏览器能力／画面验收分开。不得通过过滤 console、
修改浏览器 prototype、伪造 caps 或改写上游源码来消除警告。

Web 版本准入复用既有私有 interop 缓存读取真实当前 context。后端初始化后的
准入须取得实际 API/GLSL 及一致的 begin/end epoch；WebGL 字符串的首个版本
不是 GLES 版本，应解析其中真实报告的 OpenGL ES 值，不能提升功能 caps。
未知/缺失、混合 epoch 或低于下限时沿原 cleanup 拒绝，并清除 verified 标记。
公共 API 信息与后端名称消费同一 epoch 校验；恢复/替换须重建 renderer 后再
准入。同一状态由实际私有帧门控消费；Begin/View/End 将真实拒绝交给 CPU trace，
中央提交拒绝时不调用 bgfx_frame，也不推进 submission/retirement/instance ordinal。
公开 void ABI 保持不变。Engine 入口、事件、watcher 和回调后检查通过既有应用
失败/清理路径终止 loss 帧，独立于最小化与后台 CPU 许可。观察到 loss 后停止
新的自有帧工作；teardown 仍可退役资源。原生缺版本观测策略不变，自动恢复、
资源重建及 GPU 完成须独立验收；CPU provider/JS 控制与真实
Web context loss/rebuild、画面验收须分别记录。

当前 CPU 基线实测通过 11 项版本准入及 12 项样本查询案例。四个完整 owner
版本负控的 Unity 失败数为 5/5/1/3（版本命名空间/发布/epoch/初始化接线）；
查询两个负控为 5/7（未初始化输出/查询 gate）。CTest 通过 WILL_FAIL 接纳这些
普通断言失败，不能以崩溃或构建失败替代。完整 JS owner 的 CPU ports 正控通过；
错误传播/context 重置/epoch 发布三个负控各停在一个明确的 Node AssertionError。
该 JS harness 未输出案例总数。这些数量不证明真实浏览器 context loss、资源重建
或呈现。

```bash
python scripts/jce.py test --suite engine --filter "^test_jce_(web_api_admission(_old_namespace|_missing_publish|_old_epoch_carry|_missing_init_wire)?|webgl_interop_js(_missing_error|_missing_context_reset|_missing_epoch)?)$"
```
