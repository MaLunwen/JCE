# 图像与计时证据

canonical readback 可通过 `--readback-operations` 显式观察，按 capture 内的
精确 ID／context／ticket 关联已接受的操作，区分 provider-ready 观测、取消意图、
普通 CPU 释放及 backend-stop 后释放。未知／无关联操作和 outstanding 项保留
原貌，不能当作全局泄漏、设备空闲或 Present 完成。
`renderer.backend_shutdown_cpu` 只测实际同步调用区间。离线
`tracy-import-chrome` 可以显示 Core CPU zone 与已报告的 counter plot，不会
创建原生 GPU context 或 GPU 时间线；精确身份仍以 canonical JSON 为准。
PresentMon、PDH、GPU-view stats 与 ETW 需要各自的进程／时钟／context／队列包／
Present／DWM 关联。保留的 EXE 哈希及 sampler PID／出生时间不能单独证明这些
关联。固定动态 A/B 必须排除并发编译、采集与其他 GPU 验证。

新图形或系统采集协议先跑一次短时、所有权明确的对照，分别限定运行、
清理时长与证据体积。进程采样器发出终止请求后，最多等待所启动子进程
五秒。无法确认退出则判本轮失败。内核持有的 ETW 会话有独立的身份及
退出证据，不能凭进程退出推断会话已关闭。任一所有者清理未经确认时，
停止后续组、保留失败收据并转离线分析。系统冻结时软件看门狗依赖操作
系统调度，无法保证恢复。

输入回放证明记录的输入通道，不证明当前相机控制器消费了它。核对实际项目与
控制器绑定；渲染基准可能替换为包含静止区间的合成轨迹，应明确标注。宣称动态
相机验收前，需要实际改变的相机姿态或投影观测；backend、Engine 与模拟帧号
不能直接互换。

非前台图形对照超时必须保留失败。同一可执行文件下显式覆盖启动 VSync，可用于
区分阻塞的提交／呈现路径；收据须记录覆盖。隐藏对照完成不等于前台默认呈现
已修复，也不认证 GPU 完成或整帧吞吐。不能静默关闭 VSync 来通过验收。
Lifecycle probe 会清除继承的 JCE_*，包括父进程的 JCE_LAUNCH_VSYNC。
须使用显式 --vsync on|off（API vsync="on"|"off"），并核对收据里的实际
子进程环境含 JCE_LAUNCH_VSYNC=1|0。省略或 None 保持过滤且不请求覆盖；
请求及环境记录不证明 renderer 实际生效的 reset／呈现策略。

文件打开失败详情只描述某次候选路径尝试。宣称资产缺失前，应核对所属操作的
最终结果及受支持的回退。相对路径探测失败后从资产根成功加载，不是最终资产
缺失；没有成功证据时仍应保留未知状态。


已有 shader container 的离线 payload 导出使用
`python tools/shader_payload.py --inspector <native-jce_shader_inspect> --input <existing.bin> --out <new.dxbc>`。
原生可选 `--json --payload-hex` 复用唯一反射 parser；wrapper 限制并验证字节、
记录哈希且独占创建输出，不编译 shader、不初始化 GPU。已有输出会被拒绝。
再用平台工具反汇编导出的 payload；container、payload 与实际 draw 身份是不同证据。


固定模拟步长不会固定随机流。使用既有公开组件参数或受支持的测试控制为相关
随机流设置种子，并保留实际应用种子的证据。分析像素噪声前，应同时核对动态
几何、透明度与相机时钟。共享 Lua VM 的种子不能证明原生或 GPU 粒子乃至整个
应用都确定。不能把真实几何差异当作抗锯齿噪声扣除或遮罩。

纯着色器实验应固定同一个优化版可执行文件，仅通过启动时的
JCE_SHADER_DEV_DIR overlay 隔离被修改的编译 stage。重编基线的完整 container
必须与保留的同目标 tuple 参考字节一致，stage IO/uniform 元数据也必须保持。
已有覆盖文件加载失败时不能静默回退后认定成功。仅当可执行文件、输入、模拟
时钟、相机与实际画质仍一致时，才复用已有图像对照。eager-loader 的精确 Memory
SHA/create/link 收据证明 CPU 获取，不能证明 GPU draw 使用。短时带追踪的来源
见证应与无追踪的原生整帧 A/B 分开；减少 shader 指令或限制 FPS 不等于性能提升。
复用 shader-cache 和有界图像/trace 证据，避免重编或复制整棵应用构建树。

Shader优化需绑定实际编译payload及被消费的资源状态。源码分支或更少指令
本身不证明省时。同一capture的shader替换、native运行和跨backend编译分别
记账：replay像素全等及局部事件节省不认证动态画质、其他设备或整帧性能。
核对真实sample数与最终effective配置；修复配置层冲突属于正确性，不能据此
直接声称同画质性能提升。

Tracy 采集端与客户端的协议必须匹配。先检查用户明确提供的工具目录，避免
直接重编所谓缺失工具。核对真实产物的有效 profiling 配置：native `dist` 强制
关闭 Tracy，authoring Release/Debug 可启用。须实际生成新的非空 `.tracy`、
成功导出并读到真实 zone 行，不能仅凭端口开放认证。按需采集不包含连接前事件；
采集器时长从连接后开始，外部 owner 仍须有总时限。CPU zone 与延迟 bgfx 耗时
不等于 Tracy GPU 时间线；后者需要真实 GPU context 插桩及观测。诊断构建与
优化吞吐对照分组记录。

全线程 IP 采样前，先用现有不暂停线程的
`run_process(sample_threads=True, thread_limit=256)` 观测定位实际消耗 CPU 的 TID。
将查询线程数、截断状态与采样器的有界历史名单比较；漏掉忙 TID 的 sweep 不能解释
其 CPU 消耗。保留 PID／出生身份及 TID 复用、退出缺口；一次名称／创建时间快照
不能消除这些缺口。IP 驻留是墙钟观测，等待函数样本本身不证明线程正在消耗 CPU。

FrameLog context3 的可选 viewport 可用性互相独立。Game 面板选用独立 PostFX
输出时，`display_size` 可以不可用，而请求／分配尺寸、CPU 完成、image 和 submit
记录仍有效。保留该 null，不能用 bridge 尺寸补写。显式隐藏捕获可验证离屏 CPU
绘制，不能据此认证物理显示尺寸、Present、GPU 完成或产品可见 FPS。


编辑器生命周期 probe 可用 `--frames`、`--play-at`、`--stop-at`、
`--observe-at` 和 `--shot-at` 指定有界时序；默认仍为 240 帧恢复见证。
`--stop-at` 是 PLAYING tick 数，不是宿主绝对帧号。输出或创建子进程之前须
准入完整计划；during 快照及截图应留足项目初始化、相机运动的时间。保留实际
变换／脚本见证，不能仅凭请求了更晚帧便认定运动。固定步长的隐藏诊断不等于
普通输入、动态画质或 native FPS 对照。

生命周期探针可显式指定 --backend（d3d11、vulkan、opengl）、
--gpu-adapter（auto、intel、nvidia、amd）及 --gpu-tier
（low、medium、high、ultra）。省略时保留原默认行为；仅显式白名单请求进入
过滤后的环境及新建的独占 profile/session。请求与实际后端、设备、渲染尺寸、
有效画质分别记录；回退可能改变结果。厂商请求不是精确物理设备身份，相同
请求档位也不单独证明跨显卡画面或负载一致。

长生命周期验证可用 --trace-history-db、--disk-history-max-records 和
--disk-history-max-bytes 选择现有的精确 SQLite 历史存储。数据库须为父目录
已存在的全新绝对路径，并显式提供正数记录与字节上限。无效或占用路径在
启动前拒绝；ACK 与直接校验共用原规范读取器。仅请求历史存储也会执行追踪
校验，省略时仍使用原有界内存历史。保留原失败运行，离线复验另记结论，
不为提高读取器容量而重跑应用。实际关闭后的数据库体积、内存采样值分别
留证，不能把配置上限写成实际占用。

Tracy csvexport 的 -e／--self 输出独占 CPU zone 耗时，须保留此标签；
父区间的自身耗时可小于子区间。跨线程合计表示 CPU 工作归因，不等于整帧
经过时间、GPU 耗时或呈现延迟。

RenderDoc 的可选图像统计前，应保留每个已完成状态的完整字节哈希、绑定观测
以及重复／恢复比较。数值比较耗尽观测预算时，保留这些既有事实与原始试验失败；
不能据此诊断 Renderer 故障，也不应仅为追回已丢弃的内存统计而重开 capture。

检查 RenderDoc 常量缓冲时，descriptor 声明的范围可能超过实际 backing buffer。
核对 offset 和实际长度，以 reflection 及既定观测预算限制读取，并在可选读取前
保存这些边界。API 要求 reflection 数组序号时，不能改用绑定寄存器。后置短读
只能留下部分观测结果；保留先前有效的 counter/图像诊断和原始失败，不能单凭
这次读取就认定引擎缓冲存在错误。

重新构建的程序可用 `JCE_KPI_BACKEND_TIMINGS=1`，在显式请求的 FrameLog 中增加
`frame_backend_latched_*` 行。捕获 owner 每会话只读取一次；未设置、空值或 `0`
保持默认 CSV 字节，其他值在 IO 前拒绝捕获。编码器复用已有统计快照，不增加后端
查询。`benchmark_suite.read_frames` 将这些行单独放入 `backend_latched_ms`，
不得加进 CPU phase 或强关联当前引擎帧。缺失、无效值保留为不可用；记录的零值
不证明 timer 可用或没有等待。必须重新构建并验证真实 producer，旧产物不能继承
这项能力。

显式周期性能日志可独立于连续追踪消费有界 GPU 来源与视图名称诊断，且不增加
trace ring 事件；须用重新构建的 producer 验证。Provider 耗时属于延迟锁存数据，
不得假定各组或各视图共享来源帧，也不得强关联当前 CPU 帧。保留数据不可用、
空名称及有界名称清单截断／轮转的情况；Top-8 列表不等于完整 GPU 成本分解。
两个观察器都关闭时，仅表示快照准入路径不增加统计 getter，不能扩成整个
Renderer 从不读取统计。诊断开销与无诊断的吞吐对照分别记录；这些记录不证明
GPU 完成、显示呈现、画面质量或性能收益。

需要显式 Engine 尾部见证时，使用 `perf_bench.run(...,
require_terminal_ack=True)`（CLI `--require-terminal-ack`）。工具生成新 token，
验证原始子进程 stderr、owner 会话 ticket 和 cleanup scope。请求追踪时必须使用
新的连续流路径，并以未改变且完整的捕获中的 terminal-session counter 强关联。
确认记录位于 owned cleanup 返回后，覆盖导出 CPU prefix 无法观察的有限尾部；
不证明 GPU 完成、显示呈现、日志持久落盘或未注册的用户资源。默认运行行为不变，
旧可执行文件不能继承这项见证。

`tools/editor_lifecycle_probe.py --require-terminal-ack` 将同一 owned child
尾部验证用于隐藏窗口的 Play/Stop 场景恢复探针。配合 `--trace` 时同时验证连续
追踪及 terminal-session 绑定；`--resource-trace` 还要求资源观察证据。复用已经
验证的报告和产物 hash，默认行为保持。隐藏窗口、强制焦点的生命周期 fixture
不认证普通桌面输入或吞吐，CPU 尾部成功也不认证 GPU 退休或实际 Present。

通过 SDK helper 配置的 Web 目标，可用本次新生成的
`#jce_terminal_ack=<32位小写十六进制字符>` fragment，在 main 前显式启用同一
Engine terminal owner。fragment 不进入 HTTP 请求；未启用时启动行为不变，
非法、重复或冲突的 token owner 拒绝启动。保留页面原有 callbacks 和 console
路由。实际 fixture 检查之后，观察唯一且匹配的 `JCE_TERMINAL_ACK`，并绑定实际
HTML hash 与本次导航 token。这属于浏览器 console 证据，不是原生 owned child
的 stderr：不得伪造进程 metadata 来调用 `observe_terminal_ack`。未绑定 trace
须独立标明。canvas 归零、init OK、`EXITSTATUS=0` 或关闭标签页都不能证明
Engine 尾部、GPU 退休或浏览器资源完全回收。

使用 lifecycle_trace_report.py --renderer-submissions 显式启用 typed CPU
提交包校验。现有 streaming/B/E owner 检查四字段的活跃 service身份、线程、
类别、固定顺序和成功闭包。uint32 serial 的零值与回绕合法，generation=-1
表示未知。超过53位的精确整数以十进制字符串报告。默认兼容 scope 分析不证明
这些包已验收；完整提交包也不证明 GPU 完成、显示呈现或与 latched GPU timer
建立了关联。

可选的原始性能计数器字段分别记录后端 API 调用和更宽的引擎渲染帧尾。
仅在线程、嵌套提交所有者和计数器频率一致，并且两段原始区间满足包含关系时关联。
旧轨迹可以缺少这些字段。对齐系统轨迹前保留精确计数并绑定进程实例。
归因调度唤醒需要线程切换与就绪事件。RenderDoc 回放耗时、系统 GPU 引擎占用
和原生 Present 区间分别回答不同问题。

采用三步：定义可观察结论，证明测量路径执行，再检查并比较结果。日志不能单独证明画面、时序或用户交互正确。

隐藏窗口渲染与不创建渲染器的无头执行是不同模式。画面验证即使不显示窗口，也需要初始化渲染后端。

抓图和轨迹使用独立临时输出目录。比较时统一后端、视口、场景、有效设置、帧数和模拟时钟。按模拟时间触发截图，不同于按墙钟延迟抓图。

JCE_FRAME_DT_FIXED 控制确定性模拟步长；JCE_MAX_FRAMES 限定运行长度。JCE_WINDOW_HIDDEN 隐藏渲染窗口；JCE_MULTI_INSTANCE 避免已有编辑器锁使独立抓图无效。组合运行参数前核对当前源码中的含义。

真实时间吞吐测试将 `JCE_FRAME_DT_FIXED` 留空或完全移除。Engine 将非空
`0` 解释为默认固定步长，不能用它关闭固定时间步。普通 Editor 输入测试须
完全移除 `JCE_DBG_FOCUS_GAME`：该键存在时（包括 `0`）会持续强制焦点。
切换视口后先等待并重新观察新画面，再决定重试；输入调用返回不代表布局
已显示。保留未生效点击和后备快捷键的实际记录。

启用 `JCE_KPI_GAME_SHOTS` 且未指定抓图尺寸时，抓图目标默认为 1280x720。
图像 QA 须明确尺寸；自然视口性能运行移除 shots 与抓图尺寸覆盖，并分别
验证实际 Game 目标和可见视口，不能把覆盖后的纹理尺寸当作自然布局证明。



通用工具包括 tools/envshot.py、tools/visual_diff.py、tools/render_parity.py 和 tools/perf_bench.py。先读 --help，不虚构参数。查看真实截图，比较指标前排除空画面、旧文件或无关输出。

使用交付记录的版本、嵌入内容或源码上下文前，先核对其 SHA-256 与字节数是否匹配实际产物。
重新打包后，旁边的旧 manifest 可能仍存在；应标为过期证据，不能仅按文件名认作权威。
原生单 EXE 编辑器的交付记录位于 build/reports/delivery，与交付 EXE 分开。
打包记录中的 source_commit 只是打包上下文，不证明编译源码；还需保留构建输入哈希、
实际构建日志和可执行程序或 SDK 的版本观察。

播放与跳转结论使用墙钟测量。媒体需观察显示的视频时间、音频进度、暂停恢复和跳转完成；匹配画面尚不可用时推进时钟，不能称为同步。

新增检查需包含失败对照。未看到目标、静默跳过或资源耗尽的检查不能提供通过证据。分别报告真实失败、目标不可用和机器资源问题。

## Core-5 编辑器与 Player 基准

阅读 tools/BENCHMARK.md，运行 `python tools/benchmark_suite.py --help`。
显式选择 `--host editor` 或 `--host player`，并用 `--exe` 指向对应产物。
保留实际后端与可执行文件 SHA-256；编辑器与 Player 测量分组记录。
共享负载使用场景相机与固定模拟步长。截图使用独立视觉见证进程；
稳态指标使用完整墙钟 CSV，不使用 UI 滚动窗口。
`--profile publication` 执行七次重复，`--compare-backend` 配对比较渲染后端。
`--profile soak --soak-seconds 600` 仅提供短期诊断证据，不能替代协议要求的
最终 `--profile soak --soak-seconds 1800` 测试。
控制组、清单及原始采样保留在被忽略的 reports/ 中。
未经授权不得关闭用户并发编辑器，也不能把并发测量标为独占。
允许进行环境存在竞争时的稳定性观察，但须明确竞争条件与范围限制。
其他引擎、子系统及图像正确性的就绪结论仍需独立证据；缺少测量保持不可用。

显式请求全帧录制时，`JCE_KPI_WRITER_QUEUE=bytes,records` 在文件操作和分配前
选择有界 payload 队列及描述符条数。未设置或空值保持普通 1 MiB/1024、
LOW_MEMORY 256 KiB/256 默认值。两个字段必须是 ASCII 正十进制整数，最多
64 MiB/65536；非法值拒绝采集。比如 `33554432,16384` 可为观测到的
8秒写入区间，以目标700×3617字节/秒进行预算并提供余量；700是预算目标而非
实测帧率。这不修复 IO 延迟或保证任意长停顿。
描述符、复用编码 scratch 另行分配，payload 容量不等于捕获总内存。各对照臂
声明相同有效预算，保留峰值占用、失败及完整 writer/terminal 核算；不得关闭
严格 FULL 或将未写尾部认证成功来获得性能数字。

帧 CSV 导出按帧批量写入，`kpi_export` 阶段仍计入真实墙钟测量。
默认只在启动和退出时执行持久化 flush。仅在明确声明的诊断对照中使用
`JCE_KPI_DURABLE_FLUSH_INTERVAL`，避免把定期存储同步误认为渲染耗时。
保留所有尖峰；只有原始样本完整且进程正常退出的结果才有效。
配对计时前声明漂移及几何准入门限，控制预热与运行间隔。受污染或几何不合格
的整轮保留失败，不裁剪成通过。按实际墙钟与吞吐预算 UI 准备时间，计入
launcher/终端等待，使准备在声明的窗口之前完成。缺少对应观测时，不把漂移归因于
硬件时钟或功耗状态。

显式无上限对照可用 `JCE_LAUNCH_VSYNC=0` 与 `JCE_LAUNCH_TARGET_FPS=0`，
由共享 Engine owner 保持整个会话的启动覆盖。Vsync 仅接受精确0/1，FPS 接受
不超过 INT_MAX 的无符号十进制；已设置但为空、带符号、小数、空白或溢出均拒绝
启动。未设置保留普通项目/Editor 策略。检查重新构建 EXE 在 app 初始化后及销毁
前的 launch-policy 记录，不能只凭环境请求认定实际生效。FPS getter 仅代表引擎
限帧 owner，Renderer reset flags 仅为 CPU 状态，均不是后端/显示确认。
Noop/fallback 的呈现仍不可用；这些控制保留可见性、焦点、后台准入和场景画质。

自然 Editor 吞吐明确设置 `JCE_KPI_FRAME_LOG_VIEWPORT=natural`。该选择仅移除
FrameLog 对 clipped 或未聚焦视口的强制工作，保留现有 writer 和正常视口 gate。
未设置、空值、`capture` 及未知值仍保留原捕获负载；未知值不能认证为 natural。
active Game capture 和 DrawLog 仍请求原工作，不改变 Replay/Shot 准入、VSync、
上限、画质或模拟 dt。CSV 旁保留无观察器对照与实际视口尺寸。

请求窗口尺寸、缓存的视口请求与 PNG 尺寸是不同观察。离屏目标可能夹限请求，
该来源应读取目标所有者已提交的实际尺寸；后处理输出具有自己的尺寸。保留
生产帧及隐藏、prepare 失败状态；导出上一完整区间时读取当前状态可能贴错帧。
既有 phase 耗时与延迟 backend 统计不能单独证明持续前台或同帧渲染几何。
Game Stats 单独标注界面帧率，并区分已准入内容尺寸与实际 3D 目标尺寸及 X/Y
比例。缺失、过期或本次准备/绘制失败均保留为不可用；CPU 完成和目标元数据
不认证 GPU 执行或物理呈现。

内存日志区分不可用与已知零。保留原始 token、provider 和单位：按 MiB 取整的
已知零仍是零；可能由不可用值钳位而来的旧零仍属歧义。RSS 不可用不能认证
内存上限。进程工作集、私有提交、适配器报告量和跟踪的纹理／目标字节各有
不同所有权范围，不能相加冒充完整分配清单。

400/600 FPS 无上限目标分别要求指定硬件、尺寸与负载下完整帧间隔 2.5/1.67ms。
显示器刷新、呈现频率、CPU 工作与延迟 GPU 耗时分别记录。通过既有所有者同时
移除创作上限和 VSync，核验实际 reset/适配器/尺寸，保留 P95/P99、尖峰及长时间
覆盖。C/C++ 或请求帧率不能证明目标达到；正常空闲节能策略与吞吐协议分开。

## 媒体画面与跳转对照

比较画面前，先用独立解码器核对原生输出的 PTS。明确矩阵、信号范围和色度
重建方法；不同 RGB 重采样器属于不同参考策略。应以同一种策略重跑修复前后
对照，不能放宽阈值使结果通过。验证定点最近邻色度内核时，保留解码后的 YUV，
用标准浮点逆变换作独立参考。奇数裁剪尺寸的 4:2:0 平面需要向上取整，不能丢弃
最后一行、列。

当前代的画面与 PCM 都就绪前，固定播放时钟。非连续跳转前撤销流式声部，就绪后
重新创建；单纯暂停会保留音源与重采样器的预读缓冲。测量跳转后的实际画面、播放
时钟和已消费 PCM 的推进。已消费 PCM 不等于扬声器声学延迟，CPU 发布画面也不
等于 GPU 上传或编辑器呈现；不得据这些探针推断未测阶段。

### 交互画面与时钟证据

相机变量轨迹不足以证明实际视角改变。比较输入后的真实画面与射线选择投影，并覆盖编辑器正常事件桥接。JCE_KPI_GAME_EVENTS 是通过正常编辑器事件处理器及当前视口门控的显式归一化合成 QA 输入，不能称为物理鼠标测试。冻结 WebGL 探针后，先等待 requestAnimationFrame 呈现，再提交浏览器抓图；实际画面可见后才保存。输入画面对照应声明诊断覆盖层的排除区域。检查切换选格时建造卡片动画及首次放置的连续帧。性能运行使用真实 dt，不包含像素回读或 PNG 编码，并声明预热、采样数和独立重复；保留 CPU/GPU 可用性、原始尖峰、宿主身份、操作系统内存样本及竞争进程。原作按帧推进时，不同帧率会改变墙钟轨迹，必须报告。

编辑器在每次 GUI 宿主初始化时，对以下夹具选项仅取一次快照：

| 选项 | 含义 |
| --- | --- |
| `JCE_KPI_GAME_EVENTS` | 一次性读取的文本文件，上限 1 MiB。每行以空白分隔，格式为 `position M/B/W/K x y a b`；position 必须是 0..UINT_MAX 的整数。按文件顺序在到期后消费各行。 |
| `JCE_KPI_GAME_EVENTS_CLOCK` | 未设置、空值或 `frame` 保留可见 Playing 帧计数。显式 `elapsed_ms` 将 position 解释为单调经过毫秒数，不依赖模拟 dt 或呈现帧率。其他值拒绝该夹具。 |
| `JCE_KPI_GAME_EVENTS_START` | 仅适用于 `elapsed_ms` 的可选项：最多 4095 字节、初始不存在的完整限定宿主绝对路径。观察到新建普通文件时启动经过时钟原点；忽略文件内容，空文件即可。 |

M 的 x/y 是当前显示视口中的归一化位置，a/b 是归一化位移；B 的 x/y 是
归一化位置，a/b 是按钮与按下状态；W 的 a 是竖直滚轮步数；K 的 a/b 是
扫描码与按下状态，忽略 x/y。共享解码器保留有限值和整数范围检查。指针行
复用现有虚拟 Game 桥接及正常运行时步进前提交；键盘行经过普通编辑器事件
处理器。`JCE_INPUT_REPLAY` 及其按 CPU tick 推进的 JIRC 格式保持不变。

经过模式要求 Playing、可见且有效的运行时所属视口及当前运行时。未指定标记
时，在第一次满足门控的观察中启动。指定标记时，路径必须在初始化及第一次
满足时间与视口门控的轮询中均不存在。等待期间每秒最多轮询 10 次；无效门控
期间仍保留轮询时间限制，恢复有效门控后可能需要等到下一次可执行轮询。
起点是观察到文件的那次轮询，并非文件创建时间戳。先在标记不存在时完成 Play、布局和就绪
准备，再发布新文件。激活前允许从停靠视图切换至 Max 并改变几何。初始化
时已存在的文件或目录，或首次满足时间与视口门控的轮询及观察到无效区间后
恢复的首次可执行轮询中已存在的标记，均拒绝激活。

激活后，Stop/Pause、视口隐藏或无效、运行时或所属者变化，以及显示或渲染
几何的任何变化，都会取消剩余行并清空虚拟状态；恢复门控不会重启文件。
仅经过模式的活动状态验证也会在正常步进前提交之前执行。取消仅观察正常
宿主 update/draw 边界及现有运行时指针；没有 Play 代次标识。若 Stop 后重新
Play、且在两次观察之间完全复用了同一指针，该情况不在此证据覆盖范围内。
经过时间由公开性能计数器及频率接口负责：允许相等读数；零频率或观察到
倒退读数时拒绝该夹具。正常 EOF/错误仍延迟至下一次 draw 清空，使最后一行
先到达其正常运行时提交。无效选项仅拒绝夹具。默认帧模式不增加时钟读取或
标记轮询。来源及时钟记录标明合成输入；调度器及桥接 CPU 对照不能证明物理
鼠标运动、原生时序、GPU 完成或实际渲染运动。

## 诊断 SDK 配置与资源就绪

需要解析实际 INFO 记录的严格性能探针应使用 RELEASE SDK。DIST 编译时关闭 INFO 日志；其轻量单应用烟测仍是有效交付证据，但不能提供这些诊断记录。除非明确测试可选外部 VM 创作行为，否则使用 embedded 交付配置：

```bash
python scripts/jce.py sdk --variant release --delivery-profile embedded --profiling off --patented-codecs off --no-debug
```

分别保存交付 DIST 与诊断 RELEASE 的产物及哈希。不能放宽性能验证或更改生产日志政策，使缺失记录的运行冒充通过。原生 ABI 注册不能证明项目脚本类已内嵌。

异步模型加载应在明确有界超时内等待实际就绪。先预热 120 帧固定 dt，或至少 3 秒的实际连续真实帧间隔，再为每次声明的重复运行采集至少 600 帧测量。真实 dt 的帧数本身不能证明预热时长；单独排除启动延迟，并从原始 CSV 重算有界预热和测量边界。导入/准备耗时及超时结果与稳态性能分开报告；保留真实记录，不能将过早就绪探针或缺失性能行当作成功。

缓存模型就绪不能证明创作场景确实渲染了它。通过公开 SDK 观察接口验证实际场景实体、选定网格路径及变换，以及当前相机的位置与投影。默认宿主提供 jce_default_main_get_scene、jce_default_main_get_camera 和 jce_default_main_get_scene_renderer，均为所属线程上的借用观察；不得跨越关闭生命周期保留或销毁。jce_camera_get_fov 返回角度。实际提交拓扑和独立画面对照应与缓存数量分开记录。

现代复刻可以按用户要求替换原作依赖渲染帧的行为。区分旧式参考抓图和正常基于时间的玩法；记录已接受模拟时间、丢弃的挂起时间及余量，并用相同输入验证多种呈现帧率。卡顿后不要求无限补算墙钟时间。

GPU 查询和驱动追踪需要配套不含这些探针的帧率控制。围绕整个回调的耗时查询
包括命令间隙，不能称为独立着色器耗时；分别记录逐 draw 范围与查询可用性。
正式计时排除其他图形应用和 CPU 构建。图形存活验证需要有效目标、实际提交
及可见像素；阶段日志或没有错误本身都不能证明效果实际执行。

图形探针包装上下文创建 API 时，必须保留返回值及属性。每次运行都记录实际上下文版本与设备能力表，包含带探针的负控；另一次无探针运行的正确上下文不能证明本次探针上下文正确。

GPU 析构验证需要在关闭测试页面之前检查真实存活对象。移除 Emscripten
上下文记录不代表 WebGL 对象已经删除。保留 getContext 返回值，用程序及
着色器的有效性和删除记录识别残留，并按源码定位第一方所有者。
# 完整场景生命周期覆盖

仅效果探针不能认证场景资源清理。还应实际运行包含导入模型、解析材质纹理、
灯光和 UI 的真实场景。关闭浏览器页前，统计真实 GPU 对象创建与删除，并检查
剩余对象是否有效；即使 Emscripten 上下文表已空，也应记录残留。验证共享材质、
灯光绑定在设备重建后恢复，区分运行时缓存所拥有的上传纹理与回调借用纹理。
保留失败原始证据，将每项结果绑定到实际测试的产物。

所有宿主的渲染生命周期 CPU 跨度复用 core 的有界服务 scope，覆盖尝试创建、视图
配置、CPU 命令提交与销毁。空帧/销毁调用保留取消状态，创建失败保留失败状态。
以显式调用 seam 编译真实适配器，验证参数转发、配对跨度与父任务恢复。CPU 跨度
不能认证 GPU 执行、呈现或内存回收。WebGL 成本诊断应依据实际链接程序身份及
调用参数/上传字节量；不能仅按三角形或索引数量区分 UI 与世界。移除绘制的诊断
改变了负载，不能用作正式性能验收。

进程驻留测试通过 tools/memory_residency_probe.py 复用现有采样器，显式指定负载环境，
保证预热与观察窗口完整。分别记录 RSS/私有提交量、分配器可用性和自愿标签记账。
聚合标签峰值表示同时占用；旧总峰值是各标签峰值之和。两者都不能证明完整堆所有权
或 512 MiB 真机目标。保留原始样本和实际测试二进制身份。

连续 CPU 生命周期捕获通过 JCE_TRACE_STREAM=<output.json> 显式启用。
仅在测试指定消费者时设置 JCE_TRACE_STREAM_MODE 为 threaded 或 cooperative；
原生默认使用工作线程，Web 默认协作泵送。环境变量应在进程创建前设置。
捕获 IO 按线程抑制自身追踪事件，不关闭其他生产者。应用生产线程退出并完成
join 后，才停止消费者、排空记录并检查尾部元数据。有界环容量本身不能证明
捕获了整个应用生命周期。

使用 tools/lifecycle_trace_report.py 验证保存的 JSON，并显式指定 --out。
每个真实提交且自身拥有的 SERVICE 任务对应一个 `--require NAME`
（`cat=task`，提交时 `kind=2`，跨度 `id == parent`）。必需 SERVICE 须成功
完成；匹配的失败或取消结果均拒绝捕获。独立要求完整的 `cat=wait` B/E 配对时，
为每个名称指定 `--require-wait NAME`。两种控制复用同一个完整事件验证器。
CPU 等待结束不能满足 SERVICE 成功要求，也不能证明目标工作成功或 GPU 完成。
捕获前声明按服务名查询及必需阶段；category 总量不证明各服务覆盖。封存后，
未查询 owner 的覆盖保持不可用，不重扫 raw 补认证。

要求丢失及未导出记录为零、代次稳定、写入成功、所有者跨度配对，且活动线程/
任务/等待为零。非必需 IO 尝试的失败
保留为诊断结果；必需阶段必须成功。性能对比关闭追踪，启用追踪的覆盖运行
单独保留。CPU 生命周期证据不能证明 GPU 完成、呈现或设备内存回收。

CLI 使用有界状态验证完整 UTF-8 JSON 事件流及尾部，再完整读取一遍文件，
比较 SHA-256 和文件身份。报告输出计数及有界首末见证，不输出全部跨度或
计数器值；输出采样不会跳过验证。内存 API `analyze_trace` 保留原有完整列表。
`--max-history`（默认 200000）、`--max-active`（8192）、`--max-names`（4096）
是显式验证预算，超限必须 INVALID，不能把前缀写成 PASS。无效输入在可读时
仍保留完整字节哈希；`json_utf8_eof_validated=false` 明确语法验证中断。
前后指纹稳定不等于原子文件快照。

有界流模式下，`nonrequired_task_outcomes.aggregates` 按 canonical `(name, state)`
汇总每个失败或取消的诊断跨度，保留完整计数、首末端点见证及有限的耗时
总和/最小/最大值，不建立无界事件列表。首末表示导出顺序，不代表跨线程
happens-before；每个既有名称预算已准入的名称最多占两个结果条目。历史预算、
配对、必需阶段成功及完整 EOF 验证保持严格。内存 API 保留完整诊断跨度列表。
生产者未记录文件地址、失败阶段或可选性时，不能仅凭服务名称推断这些信息。

通用编辑器生命周期探针支持可选 `--trace`：
`python tools/editor_lifecycle_probe.py --exe <editor> --scene <scene-or-bundle>
--out <ignored-output> --trace`。默认关闭；探针在独立输出目录保存
`lifecycle-trace.json` 并记录哈希，沿用原生默认消费者。Play/Stop 恢复成功
不能代替 `tools/lifecycle_trace_report.py` 的显式阶段验证；必需阶段依据
实际宿主路径选择。开启追踪的覆盖验证与 GPU、性能验收分别保留。

生命周期报告分别保留任务提交时的父任务与跨度所有者。等待区间的名称可以与
所属 SERVICE 不同，但两端的身份、线程及所有者仍须一致。必需的自身拥有
SERVICE 阶段继续严格验证提交名称与成功结果。

显式关闭追踪的对照应设置 `JCE_TRACE=0`，并清空 `JCE_TRACE_STREAM` 和
`JCE_TRACE_EXPORT`。显式关闭优先于性能日志或 Debug 默认的自动追踪；非空
捕获路径仍表示主动请求捕获。布尔开关仅在值非空且首字符不是 `0` 时开启；
`JCE_PERF_LOG=0` 关闭可选帧日志。KPI 导出开销另行声明，并核验实际产物
生效的设置。

单线程协作式 Web 捕获应保留原始 `tid=0`，不能伪造操作系统线程 ID。验收
要求连续协作模式、无工作线程消费者、唯一明确的线程名称，并且所有事件
共享该零身份；有线程消费者、混合或未命名的零身份仍然拒绝。跨度配对、
必需阶段成功、精确计数及零丢失规则保持严格，原始 JSON 不得改写。

渲染 CPU 视图证据必须覆盖宿主实际执行路径：独立程序的 3D 配置与离屏
keep/overlay 配置均记录 renderer.view_configure；委托的完整 prepare 只记录
一次，结果遵循真实返回值并恢复父作用域。Noop API 对照验证所有权，但仍
须实际图形宿主取证；一个包装入口的名称不能证明未执行路径已覆盖。

截图完成状态由既有渲染回调与有界 PNG 写盘服务统一拥有。
`jce_renderer_screenshot_poll()` 与 `jce_screenshot_poll()` 非消费式查询
最新文件请求：-1 空闲、0 等待、1 已完成编码和写盘、2 失败；终态保持到下一
个请求成功受理。主线程调用方须协调单个所有者；pending 包括实际编码和
写盘，不能仅表示像素已回传或任务入队。忙时拒绝新请求，参数无效不覆盖
旧结果，默认宿主截图计划传播真实失败。旧纹理 PNG poll 仅消费一次终态，
只支持具备 BLIT/READBACK 能力的 RGBA16F 源；其他格式复用既有 typed
render-readback API。Noop 不具备文件捕获能力。退出须排空 CPU 写入、拒绝
迟到回调，并保留 GPU 借用的 CPU 缓冲直到后端停止；这不能证明持久化同步
或 GPU 空闲。验证阻塞写入、真实写盘失败、迟到身份、取消与后端停止时的
缓冲生命周期。文件成功完成、画面稳定和独立视觉对照分别验收。

经典 Blinn-Phong 高光指数复用同一个有限输入域 lobe 幂边界：指数为零时返回一，
底数为零且指数为正时返回零，其余正值保留原 pow。所有 shininess lobe 一致
处理，固定指数的 Fresnel 幂不变；这不能修复非有限输入向量。通过既有配方
编译 HLSL、SPIR-V、GLSL、ESSL 和 Metal，仅证明编译可移植性；实测前 GPU
数值验收保持 NOT_RUN，Metal 编译不等于 Apple 设备验证。

生产 NaN 的因果须由真实 RenderDoc 绘制事件与 shader 输出建立，同时保留
产物身份及渲染/深度状态；黑色 PNG 像素或 CPU 公式不能建立该因果。修正后
须实际 GPU 运行得到有限 shaderOut，才可认定边界修复。保留原作参考截图及
未定义 pow(0,0) 所造成的视觉差异，不能任选随机黑色结果充当一致性，也不能
改期望图或放宽原有比较门槛掩盖差异。

渲染管线质量证据应区分请求的材质光照与实际后处理成本预算。请求 MID/HIGH/ULTRA
时，即使 GLES 或硬件把 bloom 实际质量限制为 LOW，也须保留廉价的半球、wrap、rim
材质光照；显式请求 LOW 仍为中性值。复用既有 requested/active/pending 所有者，
延迟修改保留用户意图，帧末先清除快照再走唯一 apply 实现，保留观察者新排入的
修改。完整 apply 替换旧 pending，硬件层级重新解析则保留它。布尔
JCE_RP_FORCE=stylized_look=0/1 仍优先；numeric JCE_RP_FORCE=post_quality
不是已实现的强制参数。

记录实际挂载 RP 资产身份、解析后的 requested post_quality、active 预算与真实
feature query。场景渲染 JSON 描述的是创作值，不能直接叫作 GPU 生效画质；须核验
实际绘制程序及提交的 CB/GL uniform，不能只凭 active bloom LOW 认定材质光照
关闭。恢复原作光照前保留旧截图，新产物的画面与性能验收须关闭诊断后重新运行。

CPU 实证：最初 13 项生产路径控制有九项真实前测失败，修正后 13 项全通过，另有
三项既有硬件层级控制通过。随后完整 apply 替换旧 pending 的新增控制在中间实现
真实失败；最终 14 项及原三项全通过。这不能认证新候选的 GPU uniform、像素或
性能，实测前仍保持 NOT_RUN。

计时前单独观察实际链接程序与提交 uniform。将完整着色器字节绑定已审查的已安装 SDK payload，不能仅凭函数名或源码前缀；未知编译降低形式需要重审。保留失败或过期质量证据，拒绝只改状态的晋升。合成批准源码身份只允许存在于显式测试 mock 中。计时期间移除绘制和回读探针，并核对实际 GPU/上下文与先前质量见证一致。

JCE_SHADER_DEV_DIR 是启动快照，由 Engine 显式注入当前准入 Renderer；
grass/grass_shadow 与 foliage 家族复用已有逐阶段 overlay 所有者。缺少的阶段
走 PAK/内嵌回退，存在但无效的阶段拒绝。更换根通过重启消费；现有标准 Reload
命令有独立的缓存覆盖范围。保留显式 PAK 加载语义及程序所有权。宿主可能改变
CWD 时，跨目录对比采用绝对根，并从实际绑定 shader 证明选中的 payload；环境
值或源码编译成功本身不足以证明消费。

区分吞吐与呈现：吞吐要求关闭 VSync 与限帧、固定分辨率，并观察实际设置；请求设置仅表示意图。呈现测试保留实际节奏，不能据此推断无限帧吞吐。独立重复汇总使用各轮中位数的中位数、p95/p99 的中位数及离散度；保留 CPU/GPU 有效样本数、不可用阶段和所有原始尖峰。

正式呈现对比每个宿主使用七次独立重复、至少三秒实际预热及至少六百个连续测量间隔。慢端采样窗口有界，超时保留原始未完成数组；带标签的较短协议只作诊断。区分窗口标签、输入视口尺寸与解析后的 Game FBO 实际尺寸。没有活动截图计划时，截图尺寸不会设置普通编辑器 Game 目标。实际目标尺寸不可用时保留该缺口，各宿主按不同负载处理，不能推断相同渲染表面。

必需计数器须显式指定：`--require-counter-zero NAME` 要求所有匹配 C 事件的值为零；
`--require-counter-last-zero NAME` 要求导出流中最后一个匹配值为零。缺失计数器、
布尔/字符串/null 值及非有限数值均拒绝。保留原始索引和值，不能从通用 CPU 尾部
推断这些要求。资源覆盖还须匹配分配/token/context 身份、转交与释放或取消，以及
句柄销毁，并要求元数据不可用及跨捕获源缺口为零。作用域计数器 ID 不必为零。
后端所有权回调与配平的 CPU 资源账本不能证明 GPU 完成或物理内存回收。性能对照
继续关闭追踪。

共享实例所有者的资源身份须单独使用
`python tools/instance_upload_trace_report.py <trace.json> --out <report.json>` 验证。
预期负载应触发动态回退时加 `--require-dynamic`；仅经过瞬态路径的观察对动态路径
仍须标为 `NOT_EXERCISED`。匹配 token/分配代次与提交 epoch、已提交区间、绑定、
退休与销毁，以及 staging 转交、释放或取消。不能把配平的尾部或为零的聚合计数器
升格为身份闭合。明确记录捕获子集、元数据缺口与未执行路径；账本不代表整个渲染器、
GPU 完成或物理内存验收。

## 诊断 CSV 文件的实时观察

当前 Windows SDL 后端以独占共享模式打开可写文件，外部实时 CSV 读取器可能
在写入方关闭文件前一直收到共享冲突。`JCE_KPI_DURABLE_FLUSH_INTERVAL` 改变
存储持久化行为，不授予并发读取权限。实时标记缺失须保留为不可用，不能据此
认定渲染停止。进程正常退出后，核验完整 CSV 区间与时钟记账。依赖帧序的缩放
或捕获控制应使用真实已完成帧信号，并核验窗口所有者及实际目标尺寸。文件存在、
表头或退出时才可读取的记录不能证明实时帧进度。保留失败观察，不能改写上游
文件共享实现来迁就测试。

## 时钟与零耗时采样

耗时统一使用公开 jce_time_perf_* 单调时钟及其匹配频率。保留原生与浏览器
实际分辨率证据；纳秒单位不代表纳秒精度，浏览器隐私机制可能降低分辨率。
纪元时间属于独立的实时时钟域。存在且有限的零耗时 CPU 行是有效采样；
逐帧字段完整性与整段负载活动分别验收，不能将零误判为缺失或补零掩盖缺行。
提供者 seam 对照不能认证目标平台；还需实际执行已安装 SDK 的公开时钟探针，
保留原始输出与产物身份。保留前测失败，并重新验证变更后的生产者及消费者。

## 完成区间与后端观测

在同一引擎边界配对已完成的墙钟区间与 CPU 快照。“曾有可用快照”不能证明新鲜度。
初始、跳过或关闭计量的区间不具备 CPU 归因资格；保留缺失行，不能补零。实际采得
且有限的零墙时、零阶段耗时仍是有效值。保留原始墙时分布、所有尖峰和导出行数，
不能因阶段不可归因而删除慢帧。渲染边界导出不包含每次暂停主循环迭代；活动子集
需单独标明，保留剔除项与完整捕获范围。

原始捕获不记录初始化，每次已完成的渲染仅记录一次。正常迭代入口捕获上一渲染；
终止完成边界在 writer 结束前捕获当前最后一次渲染。迭代间退出及宿主显式销毁须
保留待导出的已完成工作。联合验证连续原始序号、实际最终渲染计数和原生时钟
覆盖。footer 证明已接受的记录全部写出，不能证明整个计时运行已被接受；保留
部分窗口诊断及失败。无头或未完成迭代不产生渲染行。显式捕获上限的前缀另行标明。

应用私有 reporter 按引擎会话重置历史环、窗口和日志预算，其序号独立于自动退出
限制推进。从所有已注册槽捕获自己拥有的阶段名称，后续快照旋转不能改写已捕获
报告。回调重入须在修改外层时钟或快照前拒绝。默认 SDL/JNI 事件泵位于引擎迭代
之外，普通外部模态服务保留既有渲染保护；用户递归回调不是安全的模态让出点。
CPU 门控制测试不能代替真实窗口拖拽、全屏或呈现验证。

`renderer_frame_advance_cpu` 与 `renderer.frame_advance_cpu` 仅表示主线程的后端
帧推进调用，嵌套于更宽的帧提交区间，不能与 `end_frame` 相加。它们可能包含同步
或操作系统调度，不是独立 GPU 执行或呈现等待耗时。

`wait_submit` 是 render 线程等待 API 提交，`wait_render` 是 API 线程等待
render 线程交接；两者不是 GPU 或 Present 耗时。后端字段可能来自不同缓冲帧，
不能统一平移一行强行对齐，也不能与嵌套 callback 耗时相加。


基准 SDK 观测使用消费者更新序号及 CPU 读取时钟。后端 CPU/GPU/等待计数属于
最新异步观测，没有观测到 GPU 帧身份配对。getter 成功不能证明计时器受支持；
有限零值、非有限值、负值和不可用分别保留。通过公开窗口 API 查询实际全屏状态，
drawable 与 logical 尺寸分别记录。用 `jce_window_get_flags` 查询当前支持的窗口位，
将查询成功与有效的零掩码分别保留。诊断可在创建前设置 JCE_WINDOW_NOT_FOCUSABLE=1，
并用 JCE_MULTI_INSTANCE=1 隔离宿主；核验实际不可聚焦位及未设置的输入焦点位。
平台窗口位不能认证操作系统前台所有权、物理可见性或呈现完成。显示与隐藏对照
属于不同工作负载。全屏、隐藏或聚焦的请求仅表示意图；实际隐藏
或聚焦状态不可用时须明确保留，不能从环境变量推断。

性能运行器默认要求周期 PERF 输出。明确关闭周期遥测的对照，仅可在显式
JCE_PERF_LOG=0、完整的新鲜 CPU CSV、写入记录数及字节数精确闭合时使用
require_perf_log=False。缺失的周期指标保持不可用。完整原始数据准入通过共享
解析器的显式 allow_zero_wall 选项保留有限零墙钟区间；负值、非有限值、重复或
缺失样本仍须失败。PERF_LOG 同时改变可选查询请求和 hitch/日志工作；观测到的
耗时差异本身不能单独归因于 GPU timer。

可选的私有指令指针采样器，须确认采集线程已 join 并处理自己取得的暂停义务，
才可释放状态。持有的线程句柄进入有信号状态可确认线程终止；退出时间字段本身
不能证明终止。汇总 PC 计数不具备逐卡顿时间戳及调用栈。假原生端口的 CPU
所有权测试不能证明真实宿主线程恢复，也不能保证原生暂停、读取上下文、符号
查询和报告 IO 的总耗时有界。

写入记录数及字节数闭合只证明声明的原始捕获窗口完整。到达时长限制的日志标记
本身不能证明 CSV 覆盖整个请求时段；配对对照须另行验证时段覆盖。原始数据模式
的无效参数须在删除旧证据前拒绝，即使环境值会覆盖这些参数。

JCE_MAX_SECONDS 从服务初始化起算，包含后续 app init；已完成渲染 CSV 则在初始化
之后开始。两窗口不能等同，不得移动 timer 起点、删除第零帧，或用异步 frame_cpu
补齐时段。显式 timed+KPI 捕获一次输出私有 `frame KPI clock v1` 见证，借同一引擎
monotonic frequency/counter，记录 timer_start/check、render_start/end、render ticks
总和/区间数、omitted ticks、valid/errors。仍须核验真实 writer 记录数/字节数闭合和
全部原始行。缺失、重复或无效见证、counter/frequency/序号不连续，以及未导出渲染
区间均不能认证连续窗口。旧捕获没有见证时保持不可用，不得因 helper 修复重标通过。

用真实 timer_checked-timer_start 验证应用 deadline，另行报告实际渲染长度。
frame_wall 与 render_ticks 配对只使用 encoder %.4f 毫秒格式的逐行半单位量化界限，
不能放宽 horizon 阈值。初始化前段与终止尾段须显式保留；不宣称 OS/trace 精确
时钟对齐或呈现 FPS。声明输入的 CPU 协议测试和 actual-header 运行时 mutant 只证明
各自边界；生产接线仍需新鲜 SDK/editor 实际执行验证。

monotonic counter 的零边界是合法值。真实 wall wrapper 与 iterate dt 共用私有
有序差值算术，不能由 counter!=0 推断是否初始化。频率/顺序准入与已完成渲染
资格独立，因此接受零边界不产生初始化帧。算术与旧零守卫 CPU 控制不证明物理
时钟或 GPU 的有效性。

显式 `JCE_BGFX_ALLOC_TRACE` 通过现有后端分配器及固定私有地址账本记录生命周期。
导出的 `{backend_epoch, allocation_id}` 与 revision 区分重分配和地址复用，
不导出原始地址或内容；请求不再止于固定事件前缀。必须同时验证配对转换、
终态静止、容量/身份/未知输入标志、事件历史缺口和现有捕获 loss/footer，
不能仅凭 live bytes 为零判定成功。暂停捕获时内部配对可以正确，而导出历史
仍不完整；请求字节闭合不能证明 OS 驻留闭合或 GPU 执行完成。

渲染器字体源 scope 的验收必须实际执行 `jce_font_open*` 路径。ImGui 与 RmlUI
保留各自字体 owner；空 UIText 和图片字形标签不会执行 `font.blob.acquire/release`。
完整捕获若没有这些事件，仍为 NOT_OBSERVED。复用真实文本消费项目，不能放宽
required-phase 验证或为通过验收虚构字体调用。共同验证源身份、引用转换、最后
释放及现有捕获 footer。避免重复持有的原始字节由实际同时存活引用和源大小推导，
不能据此宣称 RSS、IO 或 GPU 节省。

不能仅由 private commit 的差值归因进程 RSS 增长。工作集成员、后续区域类型及
映射文件查询是不同的非原子 OS 观察。保留未知或已退订页面、查询错误及采集窗口；
不能把不透明映射直接归给驱动，也不能由 OS 采样时刻宣称与原生阶段精确对齐。

`tools/memory_commit_probe.py --mapped-file-names` 在完整地址遍历后，可选查询最多
32 个 MEM_MAPPED 分配组。复用同一经身份核验的进程句柄，保留设备形式路径；
未查询、失败和截断与原遍历总量、状态分开报告。原有 QUERY_INFORMATION |
SYNCHRONIZE 权限足够，不读取进程内容。固定 32768 字符缓冲和 0.5 秒调用间预算
限制工作，但不能中断已进入的 OS 调用。缺少名称不能推断匿名堆或 GPU 分配；
API 不可用及实际 Win32 错误须明确保留。依据微软
[GetMappedFileNameW 契约](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getmappedfilenamew)。
纯 Python 计数端口控制仅验证协议行为，不证明实际存活映射归属、原子驻留或
物理设备内存预算。

真实 GUI 验收必须注明操作流程、可执行文件哈希和观察结果。菜单可见、
类型化 CPU 端口及完整 CTest 域标签，不能证明全部编辑器功能可达和被消费。
使用隔离配置与项目；焦点或尺寸改变后刷新窗口和像素状态，并保留被拒动作，
不能猜测其效果。

完整原生 PASS 仅适用于该次已配置登记集合。须从同轮 configure 日志提取缺前提
套件，从复制的 LastTest.log 提取 IGNORE 与条件提前返回；所在 CTest 的 PASS
不能升级为该功能证明。engine/editor/consumer 与 unit/integration/contract 分别计数。
Test Runner 的真实 GUI 验收保留选定 profile 内的 child 日志，分别核对 PASS、
显式退出失败、观察完成期限 TIMEOUT、用户 CANCELLED，以及字面 argv 和 child 身份。
准备时哈希不是执行哈希；后续原子发布或测试重链接属于另一产物批次。
工具焦点保护或陈旧像素在原生焦点与新像素一致前保持未定。暂停视口或无动画
single-step 不证明模拟时间；未保存图像的实时像素观察必须明确说明。

长时间交互追踪是独立压力控制。正常退出及观察器内部“全部已释放”标志
不能修复全局游标丢失。必须联合检查 recorded/captured/lost 和
capture_complete；大体积或慢速原始导出仍保留失败证据。不能扩张环、省略
阶段或减去插桩工作来把不完整追踪称为完整历史。

TAA 请求同步及目标格式资格须先于投影抖动。CPU/Noop 控制可证明首帧策略、
被拒请求保留干净投影及显式覆盖，但不能证明 shader/输入/目标准入、时域
画面一致性或物理 GPU 完成。

TAA 验收覆盖持续转动和移动相机、物体移动后停止、风动及朝向相机的草木、
遮挡显露和视口恢复。同一 host 帧内所有视口消费代际一致的 previous snapshot。
已验证的 resolve-on 标记只观察实际 CPU 提交，不证明速度覆盖完整或时域画质。
这项见证、latched GPU-view 计时、GPU 完成及呈现分别保留。时域画质不合格时，
可显式选择并验证空间抗锯齿默认策略，说明画质与性能取舍；不能继承 TAA 验收。

跨入口序列化工作流必须用同一请求标识确认实际应用结果。void 消息发送
端口不是激活回执。即使共用 UI 与回执协议，原生/AI provider 不可用也须
与明确命名的离线预设区分。

正式追踪输出是发布事务：独占创建的无效 PENDING 保留不是有效捕获；同侧 partial
仅为诊断证据。须同时确认事件闭合、footer 写入与关闭成功、owner 完成正式发布，
才接受正式 JSON。保留被拒保留文件与 partial；路径查询失败不能证明文件不存在。

用户指定的 Cherno gist 是设计参考材料，不是更换 JCE 技术栈、创建仓库或提交/推送
的授权。其独立导出与功能测试场景思路可纳入现有引擎/编辑器分层。记录功能 owner、
实际消费路径及缺失的运行覆盖，不能由组件列表推出完整工作流已经验证。

PresentMon 呈现观察与引擎帧循环、CPU trace 独立。固定官方工具哈希、精确进程及
swapchain、原始列单位、采集窗口与显示可用性。Present API 等待、GPU 事件时长与
OS 内存成员关系是不同成本，不能相互等同，或由另一示例的隔离 pilot 推出原作对齐。

显式 PresentMon v2.6.0 --qpc_time_ms CSV 与进程收据由
tools/presentation_report.py 验证精确 PID/名称、正常退出及工具原始字节，
并分别统计 swapchain。使用 CPUStartQPCTimeInMs；该模式下旧
TimeInSeconds 表头实际承载毫秒。行数与 swapchain 上限拒绝前缀认证，
NA 保持不可用。前后对比须保持工作负载和设置可比；降低呈现频率
不等于 GPU pass 更快，也不能单独证明 DWM 跨显卡复制成本下降。

显式 `presentmon-2.6-native-v1` 的 `QPCTime` 单位为秒，即使启动参数请求
QPC 毫秒；渲染开始/就绪偏移有符号。格式、时钟或单位不符时拒绝，未知显示
事实保留未知。显式传入一次独立的 `--no_track_input` 时，native-v1 CSV 省略
`msSinceInput` 并报告输入追踪关闭，其余必需列保持原样。WPA 导出退出为零也可能只有折叠汇总，须检查实际视图、展开规则
及聚合单位。按 context/sequence/packet 和 surface/present/bind 精确关联；
队列停止、GPU 执行、合成和扫描输出保持独立。
后端截图的 `renderer.screenshot.cpu_pixels` 记录编码前可读像素的回调时刻。
默认帧缓冲 65535 仍须关联实际提交帧和原生后端证据，才能确定完成上界。
限定 ETW provider 和时间段，
流式筛选后再写入，并在每次写入前限制保留字节；附带调用栈可能越过轮询预算。
正式 A/B 隔离动态壁纸等后台 GPU 负载，核实相机确实移动，并分别记录 3D
分配尺寸、实际显示纹理尺寸和窗口尺寸。

Windows 离线关联须核对同一 ETL 指纹，按唯一的进程、交换链和 Present
起止时间匹配，并记录工具的时间单位和量化误差。核对同一批调用后，可用
PresentMon native-v1 的相对和绝对 QPC 字段见证偏移。不能按行号关联，
也不能因显示记录吻合就推断 DWM 等待对象。

统一追踪读取器可选择按设备代际和源帧序号，将 D3D11 采样关联到已接受的
提交。不可用或无法关联的样本须分开报告。关联成功不等于 GPU fence，
也不等于关联到了系统 Present 或 DWM。

画质对比时，用已闭合的 `render_pipeline.publish` 追踪区间观察解析后的
CPU 策略，包括阴影滤波、后缓冲 MSAA、后处理质量、HDR、深度预通道和
SSGI。这不能证明实际附件采样数或格式、渲染通道执行或像素一致；
应独立保留目标尺寸与实际画面证据。

DIST 中显式用户脚本日志仍经既有运行时 logger，遵守等级、过滤器和 sink
策略；引擎内部 INFO 保持编译期裁剪。缺少 INFO 行不能证明角色未执行；
须独立核验实际绑定、start/update 探针和行为。

完整源码门禁失败须与后续定向修复分开。各自链接的普通运行时负控须核对精确失败结果，
定向 PASS 不能改写先前完整命令。原生域总数保留缺前提与 IGNORE；架构 report-only
项不能认证完整 binding 或发布。界面激活/权限失败后的操作者强制停止是 UI 不可用证据，
不能称为产品崩溃。

桌面最小化发出失焦并阻止 GPU 绘制；真实 OS/手动暂停保持独立状态。
run_in_background 控制 CPU hooks 准入，不能由服务 polling 或 drawable 准入
推出特定应用 update/模拟已推进，须另验实际事件。Quality vsync_count=2
经过 bool 呈现桥接，每第二次垂直同步仍未实现，此缺口须明确保留。逐脚本 JSON 成本候选须用现有运行时统计隔离稳定态、开场和转场。
PDH 逐引擎百分比、PresentMon 时间与 OS 内存采样窗口独立；有文件名的 MEM_MAPPED
缓存分配不等于 resident/private 或 GPU 所有权，也不能从 RSS 扣除。


编辑器启动配置以自身 profile 为作用域：在引擎创建前绑定经过验证的 editor-runtime.ini，并保留其中的显式内容。调用目录中项目的 .config/jce.ini 不能成为编辑器配置。性能证据同时记录实际配置路径、有效 Vsync、drawable 尺寸、窗口标志、节奏模式和 EXE 哈希。焦点、全屏、呈现间隔或显卡时钟不同的历史观察，不能用于公平的提升百分比。源码消费基线改善、内嵌 Bundle 成功重开、界面可达性和硬件性能验收分别报告。

最大化 Game View 不证明操作系统窗口已最大化。相同 native/drawable 尺寸不证明窗口标志相同；保留实际 MAXIMIZED、焦点和全屏观察。各对比窗口分别记录显卡时钟、温度与限频原因。热状态改变或重复序列不完整时，不能认证最终性能收益。

GPU 计时归属沿渲染器既有私有快照：API 帧、主机提交序号、设备代次，与提供者的全局及每视图 GPU 来源帧分别保留。原始 tick、频率和明确的可用状态以整数保存，包括超过浮点精度的值。相同来源帧和 tick 是重复返回的观察，不代表新增 GPU 工作。这项有界、按需启用的传输不能证明整个设备完成、呈现完成或资源退休；CPU trace 尾部不能替代缺失的 GPU fence。源码控制和真实原生 trace 分别验证。

PresentMon 的 CPU 事件区间字段不同于操作系统进程执行时间。保留原始字段；实际进程 CPU 使用固定身份的内核加用户时间与 QPC 时间区间计算。未确认整机归一化方式时，报告单核等效百分比。编辑器节奏观察必须标明真实窗口、焦点和 Play 状态；项目允许的不限帧 Play 或 benchmark 例外不能充当空闲基线。工作集、私有提交、映射分配尺寸和驻留归属分别保留。

显式启用的 GPU 视图名称映射是有界采样子集。保留实际/列出数量、轮转起点、截断和可用性，以及 API、设备和来源帧身份；比较名称前解码完整条目的百分号编码。截断行不能证明覆盖全部视图。原生 Main 和最终离屏合成的实际名称须独立于格式化单元控制验证。

选定 Windows 页面诊断复用 WindowsReader 的 working_set_pages 及同一身份固定的查询句柄。保留 QueryWorkingSetEx 原始标志、选定地址范围和数量、调用间预算及 QPC/身份窗口；仅在 Valid 置位时解释有效页字段，AWE/大页可能在普通工作集之外。映射文件名与有效/可共享页标志不能证明 JCE 私有堆或 GPU 归属，也不能据此扣减 RSS。选定页查询完成与全进程映射命名覆盖分别报告；有界命名未发现缓存不能证明不存在。Shared 表示可共享，有效页的 ShareCount 大于一是另一个有上限的观察。替换发布产物前保留实际测过的 EXE；后续页观察绑定新进程出生时间，不能沿用旧 cohort PID。

## 后端回调 CPU 证据

依赖已启用、桥接已注册与实际回调已发生是三项证据。结合程序哈希，核验
每次尝试边界的 compiled（-1未知/0关闭/1启用）、bridge support、实际 begin
计数与 saturation。完整 CPU 事件按真实 TID/category/id 及同线程栈配对；
保留不匹配事件、计数及捕获失败。第三方线程出现 scope 不能冒充生命周期注册。

固定版本 bgfx 的 `bgfx/API thread wait` 实际在渲染线程等待 API 提交；
`bgfx/Render thread wait` 实际在 API 线程等待后端。JCE 为这些名字添加
`bgfx.cpu.` 前缀。必须联合真实执行 TID 与外层 scope 解读。回调 scope
使用 task 类别，仅名称带 wait 不能满足带类型的等待账本要求。CPU 帧标记
可界定实际观察时间窗；跨线程 submit/flip 重叠不能建立后端 batch、GPU 或
呈现的一一对应。保留启动、全部超时帧、几何/焦点可用性与原始样本；短暂
隐藏窗口采样不能认证持续可见吞吐。

## Profiler 与内存证据边界

周期 `perf` 保留原有解析字段，但 `cpu_scope=bgfx_frame_interval` 表示后端墙钟
区间，可能包含此前引擎节奏等待；`gpu_scope=latched_backend` 表示返回的异步观察。
两者都不是 OS 进程执行时间，也不形成 CPU/GPU 帧配对。`app_update` 仍是兼容的
update+draw 聚合。`perf-pacing` 只报告 `scope=last_completed_interval` 对应的
序号、目标 FPS、计划/实际等待及有效时钟，不是周期窗口平均，也不能证明后端
计时器对应此帧。实际且时钟有效的节奏等待分支在既有 SERVICE 父项下发布平衡的
`cat=wait` 区间。只有确实发生等待的捕获才使用
`--require-wait engine.frame_pacing`；不限帧、首帧、超时、截止时间已过、无效时钟
及关闭追踪的分支没有 WAIT 事件。真实 owner 的类型化控制和遗漏结束事件的普通
断言失败负控，不能认证原生调度或 GPU 工作。

Profiler 的后端区间、API 线程 render wait、渲染线程 submission wait 和锁存 GPU
耗时属于不同观测。当前公共统计不提供精确引擎帧身份配对，也不提供计划节奏
归因。保留可用性及原始范围，不能相减跨线程等待来推导 CPU 工作量，也不能从
未配对计时器判定 CPU/GPU 瓶颈。提交等待和后端等待标签只描述观测到的等待；
其他情况保留未判定。getter 成功或把缺失计时器表示为零，不能证明 GPU 计时受
支持、成本低或工作已完成。

真实面板证据须绑定重建后的可执行文件、窗口状态、隔离 profile 和语言。检查 N/A、
等待/未判定标签及范围说明、剪贴板字段。解释 owner fixture 与旧等待相减负控只
提供 CPU 证据，不证明物理 GUI 送达或 GPU 帧归因。

内存证据区分当前进程 RSS、私有 commit、可用分配器计数及自愿标记记账。面板
512 MiB 目标条只使用 RSS，不代表设备容量或通过业内内存预算验收。标记用量为零
是局部记账，不是进程成本为零；分配器不可用须保留，不能用零冒充预算合格。

真实 Snapshot 操作保留报告路径，解码 profile 的 `reports/memory/` 下已发布的
`jce.editor.memory_snapshot.v2` 文件。要求 `complete: true`、显式计数可用性与
null、全部已捕获标记行、`complete_process_coverage: false` 及
`budget.scope: process_rss_only`，保留精确无符号字节值。验证 profile 拒绝不
降级 CWD，编码或原子写入失败不发布残缺报告，也不损坏此前报告。完整序列化与
同时刻采样不同；原子替换本身不能认证所有文件系统或断电持久性。

使用面板工作流中的 Profiler/Memory owner 目标及全部五项普通断言失败负控。
保留真实子进程退出码和命名断言；单独的 WILL_FAIL 结果不能区分预期断言、崩溃
或超时。这些控制覆盖统计解释、profile 解析和发布；完整 UI 可达性、OS 驻留
所有权与硬件性能须另行观测。

## 主机读取请求明细

`fs.host.read_all`、`fs.host.read_capped` 保持既有 service 返回策略。仅有效
scope 记录十五个固定 CORE counter，全部沿用该 scope 的 id/parent：
`fs.read.path_hash_hi/lo`、`total_hi/lo`、`requested_hi/lo`、`cap_hi/lo`、
`actual_hi/lo`，以及 `dispatch`、`known`、`failure_stage`、`close`、`short`；
所有字节和状态名均带 `fs.read.` 前缀。用整数运算组合无符号高低 32 位。
共享 FNV-1a-64 仅标识原请求字符串，不代表规范地址、VFS 来源或 inode；
NULL 请求用零，空串保留真实哈希。事件不写原路径或 SDL 错误文本。
关闭追踪或 consumer 线程 suppression 时，不计算哈希、不初始化明细。

`dispatch`：0 未分派，1 主机，2 VFS，3 VFS 后转主机。`known` 位：1 报告
总大小，2 请求字节，4 输入 cap，8 已观察实际字节。委托成功时使用返回
大小，委托失败不推测其字节数；转主机后的 actual 仅计本 owner 的主机读取。
`failure_stage`：0 无，1 无效请求，2 隔离委托拒绝，3 主机打开，4 大小查询，
5 长度边界，6 分配，7 全量短读。委托拒绝不能证明文件不存在。`close`：
0 未调用，1 SDL 接受，2 SDL 拒绝；`short`：0 未调用 SDL_ReadIO，1 返回
请求字节数，2 返回较少字节。清理不覆盖首个导致 NULL 的阶段。合法 cap、
既有 capped 短读与忽略 close 失败仍保持原成功返回，全量短读仍失败。
独立 close/short 观察不证明落盘持久性。

既有真实文件/VFS/ring fixture 与完整 owner 的普通断言负控消费这份合同。
重建后须采集新且完整的 trace；旧的仅 service 失败无法补出地址或用途。
字节与 counter 覆盖是 CPU IO 证据，不能代替 GPU 或完整工作流验收。

## 独立追踪线程的写入合并

私有持续追踪 owner 在现有有界 staging 中保留 threaded 模式的短读/空读批次。
达到 1MiB 或距首批待写入编码开始 16ms 时发起 flush；后续事件不延长期限。
毫秒 sleep 取整、调度和同步 IO 仍可能超过该期限，它不是完成时间保证。
stop 先 join 写入线程，再强制刷出剩余事件后检查完整性；cooperative/Web
每次 poll 立即写入的行为保持。loss/unexported/generation/活跃 owner 拒绝、
独占 partial 发布和仅完整成功写入后提交 captured 的规则保持，不采样或丢弃
事件来隐藏写入压力。

可选 footer writer_diagnostics version 1 仅观察 payload-write 尝试：大小桶
采用包含上界的 4096/16384/65536/262144/1048576 字节及溢出桶；墙钟耗时桶
采用 100000/1000000/4000000/16000000/100000000/1000000000ns 及溢出桶。
最慢样本保留单调起止时间和请求 bytes，并绑定最大耗时；不含 reservation/
header/footer/close，不表示失败写入的实际物理传输或 IO/GPU 原因。
同一生命周期 metadata/parser 检查类型、桶和、样本和字节边界，并兼容缺少
该块的旧记录。受控 owner 时钟/线程端口只验证策略，不证明调度/IO 吞吐；
仍需真实 Engine join/尾部集成及新的应用测量才能下性能结论。

## 有界公共 GPU 计时观察

在渲染器 API 所有者线程调用
`jce_renderer_get_gpu_timing_observation(out, sizeof(*out))`。NULL 或短缓冲
返回 false 且不写入、不查询；成功复制版本 1 观察返回 true，包括明确的不可用原因。
大缓冲中当前结构之外的字节不变。旧 `JceGpuStats` ABI 和 getter 行为保持兼容。
保留有符号原始 frequency/begin/end 与 `gpu_source_frame`；仅正频率、非负且
严格向前的区间允许 `duration_ms`。零频率、相等/逆向时间戳或缺失上下文/后端
表示不可用，不是零成本。`read_host_frame_index`、`context_generation` 标识
读取时身份，不能把它与可能重复或更旧的后端 GPU 样本关联成同一帧，也不能
证明 fence、呈现、全 GPU 完成或资源退休。
Profiler 与 Profile Analyzer 消费此接口并保留延后值/不可用显示；共享 helper
显示原始后端 CPU 区间与各线程独立等待，不减去其他线程等待。实际 owner 的
有类型 CPU 控制及普通漏接线负控验证传输/消费语义；真实后端计时仍需独立观察、
SDK/Editor 消费和无插桩对照。

## 共用应用资源观测

JCE_RESOURCE_TRACE=1 将主线程资源计数写入已有且已启用的 trace，默认关闭。
JCE_RESOURCE_TRACE_MS 默认为1000，限制到250..60000ms。连续采集使用
JCE_TRACE_STREAM；在 tools/lifecycle_trace_report.py 中显式请求
--resource-observations，并保留原有全部 lifecycle/wait 必需项。仅设置开关
而没有真实有效样本不能验收。Editor、Player 和 Bundle 共用 Engine 路径，
仍须分别实际消费；合成 QA 不能代替物理输入。

观测包括进程 RSS/commit、部分 tagged 分配、非创建的当前默认 Async 状态和
具备可用性标记的 GPU 延迟时钟。未知值在分析中为 null。宽计数采用
hi32/lo32，报告精确重建，超过 JS 精确范围时输出十进制字符串。
有界 first/last 样本与采样极值不是生命周期峰值或完整堆归因。保留 GPU
frequency/ticks、source frame、read-host frame 和 context generation；
不能将这些观测当作 fence、呈现 FPS 或同帧关联。

Benchmark 分布采用真实单调时钟的面板观测间隔，并分别显示后端 CPU/GPU。
覆盖缺口禁止基线比较，v1 文件命名空间保留旧基线；有限 workload/profile
匹配不代表 OS/GPU 独占。外部进程诊断可用 memory_residency_probe.py
--sample-threads 保留有界 TID 累计 CPU。PID 和 psutil 创建时间描述该子进程，
缺少线程 birth/name 时不能证明线程生命周期或计算身份连续的线程利用率。

进程内存的可用性遵循实际平台 provider。Web 上的 mimalloc committed/resident
估计不是 OS 进程 RSS 或 commit；`jce_mem_stats` 将 current/peak RSS 与 commit
保持为不可用零，同时保留复制结果和其他已支持字段。资源消费者把不可用的
进程内存记为 null，并从采样极值中排除。不得用 Web 线性内存容量或部分 tagged
分配替代缺失的进程内存；Native provider 保留原有语义。

## 可选 Windows 进程 GPU 速率

`memory_residency_probe.py --sample-gpu` 与
`run_process(sample_gpu=True)` 接入持久 PDH 观察器，默认运行不导入或加载
Windows provider。所启动 PID 为目标，对照进程必须显式重复传入
`--gpu-peer-pid`。每个保留对象绑定初始 psutil 创建时间，并在每次查询前后用
新的 PID reader 复查。未知身份不能成为测得的零；对照进程不归工具终止。
该前后检查不是原子的生命周期快照，也不是精确原生 birth token。

`perf_bench.run(..., sample_gpu=True, gpu_limit=256, gpu_peer_pids=(pid,))`
转发至同一观察器，原始收据保留在 `process`。共用选项校验先于输出删除或进程
启动；默认关闭。计数端口转发和断线负控制属于 CPU 验证，不查询 PDH，也不证明
实际 GPU 工作。Counter 观测窗口不能提供 FPS 或替代整帧计时。

英文 `GPU Engine(*)/Utilization Percentage` wildcard 使用
[语言无关的 add API](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw)
与官方的
[wildcard formatted-array API](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetformattedcounterarrayw)。
数组路线保留一个 wildcard handle；另有文档的本地化展开步骤用于逐个添加
匹配 handle。
[速率需要至少间隔一秒的两次采集](https://learn.microsoft.com/en-us/windows/win32/perfctrs/collecting-performance-data)。
保留 priming/waiting、无 instance、拒绝访问/不支持/无效状态、查询耗时、原始
CStatus 与明确 null。仅有效且有限的 counter 能证明测得 0%。数组增长失败后
重新查询所需大小；有限重试与 buffer 上限拒绝不完整数据，不发布为成功。

完整 PID/LUID/physical GPU/engine 结构可以带有空的 OS engine label。保留
实际 engine index 与通过原资格的 counter；类型表示为 `engine_type=null`、
`engine_type_available=false`、`engine_type_reason=EMPTY_OS_LABEL`，不得推断
为 3D、Other 或 unused。非空 label 保留原值；结构缺失仍是未解析名称，
使用有界诊断 witness，不能放宽 counter 数值或进程身份规则。

每行保留 PID/birth、adapter LUID、physical GPU、engine index/type 与窗口身份。
`--gpu-limit` 仅限制保留行数，不限制 OS 枚举或查询耗时；截断与实际查询/匹配
数量须同时记录。不得求和为 GPU 总利用率、FPS、呈现或 CPU/GPU 同帧关联。
[Microsoft 区分独立 engine 与任务管理器的最忙 engine 汇总](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)。
同窗口 Editor/DWM 观测可以显示同时发生的工作，但不能证明跨适配器拷贝或
因果瓶颈。真实驱动/locale counter 验收与有类型 DLL/CLI CPU 控制须分开；
比较无插桩运行，查询开销与不完整速率窗口也是证据的一部分。

## Profiler 界面观测

Profiler 当前 UI 间隔来自 Editor update 的单调墙钟；当前频率与 ImGui 滚动
平均分别标注，均不是呈现 FPS。后端 CPU 区间、各线程独立等待和具备资格的
GPU 延迟计时分别保留。提交等待提示只描述该 CPU 区间，不能证明 GPU 或 DWM
是瓶颈。

GPU/API/submit 历史图必须消费可用性 mask：仅连接相邻已知样本，保留真实的
已知零值，未知位置留缺口，并显示已知/未知数量。最新值未知不能清除此前
已知历史；上下文变化只清后端计时资格，保留 UI 历史。有界历史记录的是面板
观察，可能包含重复延迟 GPU 值，不能当成独立 GPU 工作或同帧序列。真实绘图
消费控制与物理界面检查仍须分别验收。

晚启动的事件消费者可能未见线程注册前缀。注册未知、已观察注册和已观察注销
分别标注；追踪注销不能证明 OS 线程退出。生产者当前注册数与消费者历史行的
范围不同；清空或 cursor 丢失后须撤销无依据的资格。后端内存未知 sentinel
显示为不可用，保留原始证据和有效零值。

Analyzer 导出须完整编码所有选定行，再原子发布至自有 profile 报告目录。保留
ring 顺序和明确的指标可用性；路径、编码、分配和写入失败应给出诊断，不能
认证旧 CSV 或部分内容。实际导出消费者及失败/重试链、codec 测试和物理按钮
操作须分别验证。

PresentMon 各输出 schema 保留自己的指标名称。明确 `--v2_metrics` 的收据仅在
`--qpc_time_ms` 条件下消费 `CPUStartQPCTime`；CPU 起点频率、FrameTime、
CPUWait 与 GPUWait 分别保留原义。该 schema 未输出的旧字段保持不可用。
增加受支持格式后保留原始 CSV 和失败收据，另建新复核报告，不重写历史失败。
这些观察不能证明光学刷新、完整同帧关联或相同 framebuffer 负载。
所有受支持报告模式均将 GPU 计时标注为未修改的事件推导估计，保留 NA 和有效
零值，并将硬件加速 GPU 调度（HWS）状态保留为 unknown。仅本 CSV 复核不能独立
认证 HWS 状态或硬件真实忙碌时长。[固定版本 PresentMon v2.6.0 的 HWS 精度说明](https://github.com/GameTechDev/PresentMon/blob/v2.6.0/README.md#tracking-gpu-work-with-hardware-accelerated-gpu-scheduling-enabled)
指出，启用 HWS 时 GPU 执行计时可能延后或偏大，程度取决于负载和 GPU。应将此精度
边界与原值一并保留，不能自行推算修正。CSV 中 HybridPresent 为零时，若缺少
swapchain 创建/resize 事件覆盖，仍不能单独证明不存在跨显卡拷贝；报告不汇总该字段。

显式 `JCE_FRAME_LOG_CONTEXT=1` 在私有帧 CSV 增加 `context_json` 列。每个完成的
`frame_wall` 行携带封存的 v1 窗口观测，其余 phase 的第四列为空；空值或 `0`
保留原三列格式。查询失败的 null 与有效零 flags 分开，缓存 point/drawable
尺寸与实时 native begin/end 观测分开，后端帧索引不当成硬件完成。共享解析器
验证整个文件，将选中窗口的 packet 与所有计时并列保留，并按实际表头核对
writer payload 字节。尺寸变化、查询缺失、失焦须保留，不能丢掉对应慢帧来
改善分位数。窗口 packet 本身不证明视口/FBO 负载、无遮挡画面、adapter LUID、
GPU fence 或实际呈现频率。

## 缓存与分配证据

缓存计时必须记录实际 hit/miss 差值、单调计数器频率及每组原始耗时。指针参与
hash 时，预热标签仍可能发生冲突并重复 shaping；源码算术不能证明此前未插桩
失败的原因。保留原性能门槛、等量工作及全部交替顺序样本。Debug CPU 微基准
与图形产品性能分开；同 owner 的确定性冲突负控应通过普通复用断言失败，不能
依赖计时噪声或地址随机化。

组件 patch 计数须区分 cJSON 分配调用、整树复制和进程字节。可检测的构造或合并
失败在 parse 前拒绝，不等于 void serializer/parser 内部 OOM 完全事务保证。
custom PBR stage trace 记录实际调用的 host/VFS API 和 engine-PAK 回退；host API
成功本身不能确认物理 host provider。程序句柄有效表示 API 准入，不表示 GPU
完成或像素验证。

既有 Windows commit probe 可显式选择 `--working-set-pages` 消费原始页 flags，
默认关闭。保留 selected/queried 覆盖、此前 walk 的 PRIVATE/MAPPED/IMAGE 类别、
原始 flags 直方图及原生身份/QPC 窗口。Valid=0 是观察到的 invalid 页标志；拒绝
或格式错误的查询保持 unknown。有效页字节可能包含 AWE/大页，不能组成 RSS
等式。文件名与地址匹配均属非原子观察；OS RSS/private commit 原值及部分名称/
查询结果独立保留。不得 trim、suspend 或触碰目标页来改善内存数字。

## 消费者诊断准入

改用既有 harness 前先检查其实际输入范围与完成合同。要求至少240帧的 Editor
迁移验证器不能认证60帧 stock Bundle 运行；保留原拒绝，另按明确范围验证已有
产物，不能重写或重跑后冒充同次证据。GUI 子系统程序可能不把 LOG_INFO 送到
stdout，应核验声明的输出文件，不能把空管道当成项目生成失败。

长capture的`sequence_exhausted`元数据字段存在时必须严格为布尔false；即使其它
计数平衡，也须拒绝true或类型错误。旧capture缺字段仍保留absent，不能合成
已知false；有限cursor耗尽不认证无限录制。完整EOF、loss、unexported、generation
及owner闭合仍是分别检查的门禁。

`[jce_log_truncated required_bytes=N]`与`[jce_log_format_error]`表示消息不完整。
不能把残留前缀当成完整结构记录，也不能根据源码或默认值补字段。使用真实sink
消息容量验证；原失败保持原样，修正producer/consumer后另建新收据。

显式`JCE_RENDER_RESOURCE_DIAG=1`的资源日志schema2必须完整取得part1/2，并按
event_ordinal、creation_serial、origin_capture_generation、view关联。缺失、重复、
交错/错配或耗尽part0均拒绝；随后再按资源identity、FBO/附件、allocated尺寸及
forwarded矩形关联成功分配与view_configured，requested尺寸本身不够。它们只是CPU
配置调用观测，GPU完成未知，不能当成分配完成fence。

render-scale `1.0 -> 0.5 -> 1.0`通过降低画质诊断像素负载敏感性。核验真实分配/视图
尺寸，保留两次全scale之间漂移及全部CSV尖峰，固定产物/Bundle、时钟与观察器。
不能把该变化报告为同画质优化或稳定400/600FPS。latched后端submit/wait各组独立，
不得相加或作为GPU/DWM因果归因。

WASM 引擎关闭与 Emscripten runtime exit 是独立观测。明确 smoke 标记及 canonical
CPU/资源 trace 闭合能证明该次引擎运行；未见 onExit 保持 null，不能推断退出码0。
保留有意的脚本异常控制及未安装的可选 VM。草 early gate 准入、prepare、colour
提交分别验证；空 shadow caster 遍历不证明阴影绘制。缺少图片即像素未验证。
比较各消费路径前冻结二进制与 SDK 身份。

canonical Runtime create/tick/destroy、Editor operation/Play scope 表示同步 CPU
调用墙钟。operation 默认成功表示正常返回；已知业务失败须显式 FAILED。视口
policy、实际 render 调用、early admission 与最终拒绝分别观测。reset 保留请求
与传入的尺寸/flags及原因；void bgfx 调用返回仍不证明后端生效或 GPU 完成，
也不能仅凭 SDL 事件数或 reset 调用数宣称重复 swapchain 重建。真实 ring 闭合
控制与绑定产物的消费验证分别进行；隐藏窗口不能认证前台呈现或所有物理 UI。

既要观察创建后端前的初始flags，也要观察后续reset调用。请求policy、启动override
是否存在及其值、effective／passed flags分别保留。核验实际后端与适配器：隐藏
窗口仍可执行真实GPU工作，枚举适配器不证明已选中。`JCE_PERF_LOG=0`仅关闭可选
遥测，不等于已编译或已准入的Tracy／bgfx profiling必然关闭。profiler结论须绑定
实际构建与准入观察；这些CPU flags本身不认证后端生效或显示呈现。

读回取消属于逻辑结果，不能据此释放仍可能被 GPU 写入的 CPU destination。
release 请求成功也可能继续保留 slot/buffer，只能经既有 ready-frame、retire
及 backend-stop 权威回收。普通 UI 取消不能隐含 render submit 或绕过 drawable
准入。poll 在本帧完成后仍须保留 busy 决策，避免后续视口复用该 capture view
band。读回格式在 target prepare 后按实际格式选择；authored HDR/LDR 输出与
实际 source 格式分别处理。

关闭追踪的采样须核验引擎实际 policy，不能只填写 harness 的布尔字段。非空
JCE_TRACE_STREAM 或 JCE_TRACE_EXPORT 优先于 JCE_TRACE 并启用追踪；须移除两者、
显式设置 JCE_TRACE=0，保留实际环境与 stream 观察。此前误标收据按原字节保留，
另追加纠正。KPI CSV、context packets 及进程采样仍有观察开销。只在明确相同
负载与冻结产物下比较，保留全部帧，区分 CPU 区间、GPU 完成与实际呈现。

canonical counter JSON 保留 int64 十进制值；读取超过2^53的原始 GPU ticks 或身份
须使用保留整数的 reader，JavaScript Number/JSON.parse 不能保证精确。先检查实际
writer/provider 链，再判断量化来源。非全零的相等 begin/end 可表示有效零区间，
全零初始状态仍不可用。

## Canonical GPU 视图 packet

`python tools/lifecycle_trace_report.py TRACE --require renderer.submit_cpu
--gpu-view-observations --out SUMMARY` 显式启用
`tools/gpu_view_observation.py` 的有界 typed 消费者；不传选项保留旧报告行为。
复用 canonical reader 与 B/E owner，要求真实 service、活动 TID/category/id/
parent、顺序 counter 和成功闭合。每个 `gpu.view.<uint16_view_id>.` packet
严格按生产顺序包含九字段：`device_epoch`、`api_frame`、`submission_ordinal`、
`source_frame`、`begin_ticks`、`end_ticks`、`timer_frequency`、
`interval_available`、`reported_ticks`。不做类型强转，验证 uint32 帧/ordinal
与 signed int64 时钟边界；正 int64 epoch 表示已知，-1 保持 unknown。保留精确
原始整数，报告中超过53位精确范围的值编码为十进制字符串。

仅当 frequency > 0、begin >= 0、end >= begin 且端点不全为零时区间可用。
可用时 reported ticks 须等于 end - begin，否则为 -1。非零且相等的端点保留
有效零耗时，不可用耗时保持 null。这些是稀疏、latched provider 样本；每个
view 的 source_frame 独立于其他 view、当前 API frame 及 CPU/CSV 行。不得求和
推算整帧时间，或据此推断呈现、fence、GPU 完成及逐帧覆盖。

仅保留有界活动记录、已知 (epoch, view) 聚合、first/last witness 和每组一个
last signature。重复资格仅比较该已知组的前一次观察，使用 epoch/view/source/
原始 ticks/frequency；unknown epoch 不入组，并打断比较连续性。耗时聚合排除
这些相邻重复，不对全部历史去重，也不是逐帧平均。显式请求却没有 packet 时
失败；全部计时不可用的 packet 保留明确 unavailable 状态。验收仍须满足
canonical 全文件 EOF、输入指纹稳定、完整 capture 元数据、丢失控制、scope
闭合及 reader/observer 上限；识别到字段前缀或读取局部文件不足以验收。

## 截图调度与运动画面

原生截图 schedule 时间是到期阈值。前一个 in-flight 请求可能等 PNG 编码和写盘
完成后才放行下一次请求。归因运动画面差异前先做同产物 A/A 控制，降低采集频次
以隔离 writer 反压。文件修改时间不是 renderer 源帧或相机位姿观察；保留不可用
的 frame/pose join，并考虑启动 reveal/composite pass 可能遮住底层场景差异。
正常退出不能单独证明所有 schedule 请求完成，尤其是 application-exit 取消路径。

请求标签和名义模拟时间不是源位姿 join。PNG 反压可能把后续捕获推迟到不同
相机状态；将每张 admitted 图像关联到实际源帧/位姿，否则保留该关联不可用。

`--screenshot-requests` 显式启用 canonical lifecycle reader 的有界请求账本。
admit/terminal 使用已接收请求的 uint64 identity，不替代 task scope，也不按全局
跨线程时间戳强排序。每个 terminal 只匹配一个未关闭 admit；允许不同请求乱序
结束。要求 admit serial 严格递增、mode/outcome 合法、完整 continuous capture
及既有 loss/EOF/身份门禁。只保留有界 active map、有限聚合与 first/last witness，
不累积全部已关闭 identity。`NO_ACTIVITY` 不表示截图验收通过。CPU 编码/文件
成功、失败或退出取消均不认证 GPU 完成、耐久存储、图像源帧或相机位姿；真实
ring/writer 协议夹具与实际产品截图是不同验收。

时域历史归对应视口 owner 所有。复用要求 scene、renderer context、连续 CPU
submission、Play ownership、尺寸与投影一致，仅在 resolve 成功后提交 metadata。
focus、pause、device 生命周期事件即使未推进 backend frame，也可能要求历史
失效。先 take/clear listener token，再注销，最后释放资源；注册失败须经完整
现有 owner 回滚。CPU 准入测试不认证动态像素或 GPU 退还。

隐藏窗口可按正常策略不绘制 Scene/Game，而继续绘制编辑器 UI。把运行认作应用渲染负载前，
必须核实实际 viewport admission、draw/pass 和资源证据。显式 capture override 会改变准入，
应列入测量对象，不能称自然可见状态。初始短段窗口/context CSV 不能认证后续人工操作或整个
计时窗口。wall-IP 直方图、OS CPU 调度栈与逐 GPU engine 的区间占用分别说明范围。

UI 行虚拟化须保留原生键盘导航、选择、弹窗、复制与完整内容范围。分数间距应遵守工具包
实际 item 取整。派生几何归现有 panel owner；用真实字体身份变化失效，并在 shutdown 释放，
不能仅以指针相同判定字体没有重载。

`renderer.view_configure` 中重复的离屏 identity counter 描述成功的 CPU 配置调用；
分配计数只认真实 create-committed owner。完整 packet 应匹配其 service、resource
serial、capture generation 和 submission ordinal。分配尺寸、转交矩形与 UI 实际
选用纹理是不同观察，均不证明 GPU 完成。扩展这些观察时，保留真实缺字段断言
负控及严格不变的原分配生命周期计数。

## 缓存失效与组件通知

缓存 generation 不是 setter 或通知计数器。省掉无关失效时，必须保留真实 typed
写入及 ECS 通知，包括等值写；用真实 observer 验证通知，不用 generation 增量
做代理。比较缓存实际依赖的字段；没有先前快照时，mutable alias 应保守处理。
保留 membership 变化，并在展开其他 setter 前恢复私有特化。

先核对 pinned ECS 实现，再断言 deferred 可见性：已有字段可能立即复制，而通知
延迟。夹具即使在断言展开后，也要退还自己拥有的 deferred scope。负控保留普通
断言汇总和退出码；崩溃或缺少测试终态不能仅凭 CTest WILL_FAIL 就算通过。
CPU 缓存控制及局部阶段节省不认证像素、分支命中次数、GPU 收益或正常时钟整帧
性能。

## 普通编辑器操作与有效GPU负载

区分manifest场景、loose场景与isolated Bundle。实际操作File/最近场景及Assets
打开入口，包括拒绝加载；不能用自动Bundle或强制Game运行替代。记录真实加载
路径和脚本集合：用户会话可能恢复另一个项目。受控运行克隆配置，并区分主动
修改副本与应用正常保存session。

初始化后绑定实际backend和vendor/device或adapter LUID。Editor持久化偏好可能
先于环境提示；Auto可能选显示器的集显。场景作者HUD标签在启动时可能陈旧。
独立GPU engine与DWM分别记录；归因合成器负载前，应观察Editor退出后的桌面基线。

记录内部 target、最终显示尺寸、sample 数和实际分辨率策略。外窗固定时自适应
缩放仍可能改变画质。`ImGui::Begin` 返回真不证明呈现已显露：同 host 的 overlay
仍可能遮住面板。按冻结的帧快照及 viewport 身份判定覆盖，不误抑制 detached
视口。完全覆盖时仅省 UI 聚合、排版和绘制提交，保留必要状态转换及 host/window
维护。隐藏 dock 的 Begin/End 本身不证明执行 3D，仍需 render/admission/submission
及实际 pass 证据。不同内容、可见性、缩放或 Play 阶段不能用来给 backend 排名。

活动截图尺寸可能覆盖 Game target。同一外窗下，关闭捕获的运行可能使用不同
的自然渲染表面。分别观察两次运行的 requested、allocated、displayed 尺寸，
再声明当前实际工作区并匹配控制；不能从截图请求推断自然尺寸。

replay 行 accepted 只证明准入。拖动区间须验证 held-button 的实际消费、完整
相机 transform/projection、focus/hover 和 target 尺寸。比较实际组件几何/状态；
seed 相同或局部相机探针不足以证明这些条件。

固定步长运动控制须通过显式 QA selector 同时覆盖停止预览和 Play，保留普通
计时策略。复用项目既有 seed 控制并比较实际 clock/state/geometry：已设 seed
的 runtime 仍可能继承未设 seed 的 preview 状态。明确保留 particle RNG/age
缺口，并用相同算法的图像重复运行测量剩余噪声。

对替换方案计时前，要求负载的真实 producer 经既有 shared wrapper 产生非零
matching submits，包括 model 的 direct/indirect 路径。candidate 加载、selector
准入和 wrapper 单元测试本身不能证明负载覆盖；CPU 覆盖仍不认证 GPU binding
或实际执行。源码/DXBC 中提前的 discard 及声明的 shader temporaries 不证明屏幕
覆盖率、物理寄存器分配/occupancy 或整帧节省；源纹理透明率不是实测 GPU 省工。

CPU Editor whole-owner 夹具可能因无关 About/asset 引用拉入 renderer shader cook。
保留真实被测 owner，仅用意外调用即失败的 typed ports 隔离这些无关依赖；不能
替换被测 owner 或删除断言实际使用的 Engine 依赖来获得便宜的通过结果。

重复资源准入拒绝属于诊断，不是新增allocation。用精确owner/epoch累计和首例
见证约束报告量，保留真实创建/退休生命周期。丢事件、计数饱和、详情截断及未
激活owner必须保留覆盖限制；合法JSON尾部或unexported=0本身不能证明捕获完整。

### 当前帧 GPU 资源依赖

CPU `prepared`/`depth_prepass_frame` 标志与已绑定 SRV 仅证明准入和绑定，不证明 GPU 已生产或 shader 实际采样。须把具体消费分支及真实常量、资源 ID、生产 clear/draw 与最终 backend view 顺序关联；生产晚于消费者时，除非有明确 history 合同，否则只能读旧内容或未定义内容。纠正 depth 顺序时保留 current-color 后处理依赖，并以普通旧顺序负控覆盖“只有该可选 pass”和硬件 view ID 最后边界。没有实际 TAA pass，不从类重影图像猜 TAA；采集门失败、目标缺失和身份未知始终标不可用。

### 进程精确身份的传参

浮点进程出生时间使用字符串或结构化 argv 传递；PowerShell 可能在观察器收到之前重格式化未引用的数值字面量。身份比较保持精确，不能增加容差掩盖传输精度损失。保留原拒绝，并在再次采集前诊断参数边界。显式 GPU 请求仍可独立观察显示拓扑；observed、requested 与实际 selected adapter 分开保留。显示 LUID 不同或 Copy engine 利用率高，均不能单独证明跨卡复制。

KPI capture 的派发合并复用既有有界字节环和单 executor task。capture 选择 16 KiB 或观察到最老记录达到 8 ms；writer 配置全零保留立即派发。年龄由下一次 submit/poll 检查，不是定时器或硬 IO 期限。finish 强制提交尾批，原 durability 边界、严格 FULL 和部分写失败规则保持。fixture 的任务/写次数减少只证明调度开销减少，不能解释未定位的 OS 写入停顿或认证整帧提升。

## 同一完成区间的宿主视口观察

显式 `JCE_FRAME_LOG_CONTEXT=2` 选择独立、有界的 context-v2 CPU 格式；
默认及 selector1 保持原字节和严格键。Engine 仅在当前 owner 主线程的
app.draw 内复制 sized 宿主报告，再与该完成区间一起封存。未报告与明确空报告
分开。请求、实际分配、选中显示尺寸、prepare 成功、CPU 函数尾部完成、Image
调用、最终排队命令、实际主 view/录制 view 提交次数分别保留 availability。
拒绝后仍可显示旧分配；未知 PostFX 尺寸不能借用 bridge 尺寸。截图可把主 view
重定向至 FBO。这些 CPU 观察不证明 GPU 执行、回收、backbuffer 显示或全部
shader 成功。计算同区间有效渲染子集前，须使用严格 reader 并保留 writer
终态、时钟和完成区间所有权证据。

显式 `JCE_FRAME_LOG_CONTEXT=3` 保留 v2 视口事实，并复用既有 Engine performance
counter 记录完成的 `frame_wall` 区间。`clock` 的 domain 为
`"jce-performance-counter"`，`frequency_hz`、`begin`、`end` 用规范 uint64
十进制字符串，读取时保留精度。严格 reader 检查全部行的耗时、顺序与频率，
包括预热和尾部；默认及 selector1/2 保持原字节。该时钟不是 Unix、GPU 或
Present 时钟。跨提供器关联须核验时钟映射与实际区间；时间重叠不证明因果，
也不能把延迟 GPU 统计配给当前 CPU 帧。

公共默认 runtime 入口在 `JCE_KPI_FRAME_LOG` 非空且
`JCE_KPI_FRAME_LOG_VIEWPORT=natural` 精确匹配时遵守宿主绘制策略，包括失焦和
暂停；仅移除 FrameLog 自身的强制绘制，显式截图仍可强制绘制。FrameLog 为空
不会强制绘制，其他视口选择值保留原有采集行为。


## 可选文件读取详情

在 tools/lifecycle_trace_report.py 的同一次标准分析中显式启用
`--filesystem-read-details`，Python API 使用 `filesystem_read_details=True`。
现有作用域计数器已包含请求字符串哈希、分发路径、首个失败阶段、字节可用掩码、
总量/请求/上限/实际字节，以及关闭和短读观察。不会输出原始路径或错误文本；
哈希不代表规范资源身份，也不能证明路径有效，零值和空请求同样保留其原始含义。

一旦存在详情，必须提供全部 15 个唯一且类型正确的字段；字段顺序不作要求。
计数器 id 关联活动读取作用域，parent 关联提交时的调用者；B/E 的自身 owner
与此不同。完全缺少详情属于未知，部分缺失、重复或跨 owner 字段拒绝分析。
活动状态复用 max-active，诊断详情最多保留 min(history,256) 个首 N-1/末条
见证并明确遗漏数量；全部后续数据仍验证，聚合键不会按路径哈希无限增长。

未知字节保持 null，实测零仍为零。返回缓冲区成功可能同时观察到关闭失败或
有上限读取的短读；不得改写原任务结果或推断完整读取/持久化成功。可选/必需
意图和 errno 不可用，OPEN 不等于文件不存在，隔离 VFS 失败也不能推断缺失。
默认报告不变，必需阶段、丢失、EOF、时钟和文件身份检查继续生效。用于下一次
自有分析，不为补充旧摘要而反复扫描已经完成的应用 trace。

新自有 Engine 会话可用 JCE_FS_OPEN_FAILURE_DETAIL=1 启用现有宿主读取 owner
的有界 OPEN 失败日志详情；默认关闭，数值 trace schema 不变。按实际 scope、
parent、generation 和请求哈希关联路径/错误两条记录，并核字节长度、hex 前缀
和截断。预算 context 归 Engine 对象所有，host-read service 仅借用到诊断
生产者 join 之后，VFS 切换不重置它；重复或 foreign begin/end 不能重置或
越权解绑当前 owner。每会话64组预算、日志过滤、丢失、轮转或缺少一条记录均可能使证据
不完整；尝试输出不等于确认交付。SDL 文本仅为 provider 解释，不是跨平台 errno，
也不能证明文件可选；这项功能不能恢复历史 trace 中未记录的文本。

共享默认 SDK 入口也上报 context2 的独立程序 source1；宿主策略拒绝和缺少
owner 的提前返回明确标 CPU_SKIPPED。scene config 的请求尺寸、prepare 成功及
已准入的 offscreen 分配分别保留；直接绘制 fallback 清除 bridge 的观察。
CPU_COMPLETED 仅指 scene-render 调用返回且到达宿主尾部，包括 renderer 内部
拒绝后返回同一 view ID 的情况，不代表渲染或 Present 成功。请求矩形不能冒充
观察到的显示；此宿主的最终显示、纹理与 ImGui 次数保持未知，包括 void PostFX
present 和截图调度。默认/v1 仅增加 enabled 检查，不清空观察 POD，也不额外
查询 extent/window。CPU owner 控制及刷新 SDK 消费不认证可见输入或性能。

### RenderDoc 绘制数量语义

RenderDoc ActionDescription.numInstances 在未使用实例化时为0。仅当
ActionFlags.Instanced 存在时才按零实例数拒绝绘制；保留原始数量，普通绘制
如需记有效次数1应另字段标注。用同一个准入函数覆盖普通绘制0、实例化0、
空索引和实例化正数控制。判断shader缺失前，保留有界的实际shader身份及明确的
绘制拒绝计数。参考 RenderDoc1.44 api/replay/data_types.h。

### 原生捕获控制与输出所有权

可见 RenderDoc 会话前，检查捕获热键与实际 UI 快捷键是否冲突。F11 捕获也
可能响应 Shift+F11；Game 工具栏的 Maximize 按钮可避免该冲突。即使编辑器
切换成功，未经协议请求的 NewCapture 仍使单次触发协议失败。

Renderer 启动时可能覆盖注入器的捕获文件模板。声明输出根前检查所选依赖
的实际配置；bgfx 可能使用配置中的 temp/bgfx。路径准入前保留通知原始
path/local/API/size，只允许预先声明的自有本地目录与文件名，并保持路径
包含关系、文件身份及唯一捕获检查。原拒绝轮次保持失败。正常 Stop/退出及
进程退休，与捕获成功、回放、GPU 计时和最终显示图像分别验证。


## 可选 shader CPU 来源追踪

在下一次自有标准分析中使用
`tools/lifecycle_trace_report.py TRACE --require renderer.shader.create --shader-provenance --out REPORT`，
或 Python 参数 `shader_provenance=True`。eager shader 加载前启用既有 trace
采集及设备 `JCE_RENDER_RESOURCE_DIAG=1`。默认分析不导入此 observer，也不增加
结果键。有序 render 数据包用精确 acquisition/capture/context ID 关联实际 CPU
缓冲区长度/SHA256、create 返回、同次调用 link 及 creator-release 返回，复用
既有 reader、B/E owner、丢失、EOF 和指纹规则。整包缺失和非零原因保留不可用
缺口；部分、重复、跨 owner、重复消费和乱序 receipt 均拒绝。pending 有界，
仅为观察，不能标为泄漏。没有原始字节就不能独立重算报告中的摘要。此子集不
认证其它 shader owner、bgfx 缓存字节、program 最终引用、GPU 执行、fence、
显示或整帧提升。合成控制通过后仍须新原生导出到 consumer 的端到端验证。

可见 F5 验证应在切换后刷新窗口状态；不能把立即得到的旧帧当作稍后状态，再按
第二次 F5 停止 Play。操作者错误轮次保持失败，以一次切换和新 PLAYING 见证
重测。deadline 超限次数必须由实际区间计算，不能写死在模板；60 Hz deadline
的轻微超限与严重卡顿分别报告。

Terminal ACK 可显式传 `trace_analysis_options={"required": PHASES,
"shader_provenance": True}` 和 `return_trace_report=True`。最终 cleanup/session
由同一次 canonical report 绑定：一次 JSON 解码及既有完整第二遍字节指纹，
不再独立重扫 session，也不接收调用者提供的可信报告。返回的 `trace_report`
包含完整分析结果；shader observed/complete receipt 仍须单独断言，ACK 成功本身
允许没有 shader 观察。不要为获取这些字段再次完整分析同一 trace。

## 进程可见性与解码范围

进程清单可能仅覆盖执行沙箱或会话。用缺少的行证明退出前，先确认观察者能够看到已知 PID、创建时间和可执行文件。受限查询错误是不可用，不是零存活进程；退出应由实际主机或所属 owner 观察，不能操作猜测的 PID。

首次完整遍历 trace 前确定所需 typed observer。诊断加载失败时，在 producer 支持的范围内显式请求 filesystem read details 和 shader provenance。必需 CPU scope 成功不认证未请求的 packet 细节；路径、errno、可选性和未保留见证仍须标为未知，没有证据不能把失败读取称作正常探测。


长时间离线 trace 可通过同一个 canonical reader 和 ACK 链路启用精确磁盘历史。
必须同时提供全新绝对数据库路径、明确记录数和字节预算；默认内存与其他 RAM
上限保持不变：

```text
python -B tools/lifecycle_trace_report.py TRACE --require engine.log_quiesce --cleanup-ownership --terminal-session-binding --out REPORT --history-db ABSOLUTE_FRESH_DB --disk-history-max-records 8000000 --disk-history-max-bytes 536870912
```

该一次性索引精确保留全部 task、parent、已完成服务成员和终端 session，不能
修复已丢失的 capture。存储、预算或关闭失败均拒绝验证，不回退到 RAM。完整
EOF 与两次完整字节指纹要求不变。API/ACK 参数见
[trace 验证预算](../../../tools/BENCHMARK.md)。可选 version-1
`reader_diagnostics` 保留首次/最后丢失检测快照及读取停止进度；经过检查的
进程时钟表示检测时刻，不是覆盖发生时刻或其原因，不能提供 CPU/GPU 时钟
关联或覆盖完整性门禁。没有该块的旧 capture 仍兼容。

## 匹配性能条件、有界视图与构建进程包含

每个测量窗口保留原始温度、时钟、功耗和限频状态。场景与尺寸相同仍不足以证明
运行点相同；不匹配比较必须拒绝，尖峰和不可用字段全部保留，不能按 MHz 比例
换算 FPS 制造收益。区分冷态和持续负载，记录观察器分段偏差。启动前为真实
界面/工具往返、预热、测量、后窗和清理预留 owner/观察器期限；guard 退出不
认证手动 Stop 或 Alt-F4，不能事后延长已启动的预算。

重排绘制前证明实际接受的范围、准入和 draw 数量；transient 仅接受前缀的策略
可能受顺序影响。当前 A/A 像素重复性先于 A/B，QA 固定模拟时间与自然时间的
用户输入性能分开。截图纹理尺寸不证明真实可见窗口或 Max；Lua 种子不证明
原生/GPU 随机性确定。opt-in 候选须有实际准入、画面及匹配整帧结果，才能认证。

Editor live trace 对保留的线程、counter、active-span 记录设界，并把本地
拒绝事件与 Core producer 丢失分开；达到上限仍能更新既有 key。Clear 仅重置
本地视图；公开 cursor/stats 没有 generation token，外部 quiescent reset
之后需要显式 Clear。CPU 缓存数量控制不认证物理内存或原生显示。

`tools/build/windows_build_job.py` 是显式、实验性的 Windows 独占进程树 owner，
默认/runtime runner 不变。挂起创建、Job 成员及 held handle 的准确路径/hash
身份证明进程包含，不证明共享 IPC 服务的逻辑独占性。共享 MSPDBSRV/VCTIP
策略拒绝；不能用它包装真实 MSVC 构建，或把旧严格清理失败改为通过。受控
CPU/CLI fixture 不认证真实编译器集成。计划终止 helper、紧急包含、不可用
元数据和关闭失败必须分别保留。

不透明几何本身不保证重排后像素相同：有限深度量化和几何交叠可能改变胜出的
采样。既有可见性 AABB 也不自动构成完整动画几何包络。保留当前 A/B 失败图像，
限定原因结论，撤回未证实的候选，不能静默修改比较规则。

手动结束且 `JCE_MAX_SECONDS=0` 的诊断不启用 timed-deadline KPI ledger，
这不是一次有效期限检查；明确记为 NOT_APPLICABLE，另行验证 CSV 时钟连续性、
frame disposition 和 terminal ACK。不要仅为等待计时器到期而额外长时间压测 GPU。

私有启动队列/flip 实验需要重新构建生产者。在启动进程前，可独立设置
`JCE_GFX_FLIP_AFTER_RENDER=0|1` 和/或 `JCE_GFX_MAX_FRAME_LATENCY=0..3`。
缺失或空值保持 bgfx 默认值；显式值必须恰好为一个 ASCII 数字。窗口化创建
在获取原生窗口/设备之前明确拒绝非法请求，指出非法字段但不回显任意环境内容；
宿主仍可选择其正常的软件回退。一次不可变快照供全部后端重试使用，修改变量
需要重新创建进程/设备。flip 控制 bgfx 的 flip 调度，不是 DXGI flip-model。
latency 只在初始化时传给固定版本 bgfx 1.146.9306-550 的 D3D11/Vulkan。
D3D12、GL/GLES 及其他未支持后端保留构造器默认值；Noop、headless 和软件
路径不适用。显式零值请求后端默认值，不代表零深度队列。Vulkan 的 fence/延迟释放
环长度变化可能降低吞吐量。
有界 `launch-gfx` 日志及 `renderer.launch_gfx` scope 仅记录 CPU 请求、转发和
准入，不代表后端确认或物理完成。`latency_support`：1 为 D3D11/Vulkan 转发，
2 为 D3D12 不支持，3 为 GL/GLES 不支持，4 为 Noop 不适用，5 为其他/未知。
`phase`：0 非法拒绝、1 初始化失败、2 CPU 准入、3 API 拒绝、4 headless、
5 软件入口；未尝试初始化时 `cpu_init=-1`。headless/软件入口忽略这些实验并
报告不适用，包括非法值。默认关闭时不新增日志/trace，也不在帧循环读取环境。
完整实际 owner 的 CPU 测试及普通缺失转发线 mutation 只证明准入、转发、reset
位保留及失败时不发布结果。性能验收必须固定资产、动态相机、尺寸、画质、节奏
和适配器，重复 A/B，核对实际后端与启动转发记录，并分别测吞吐和呈现延迟。
不能仅用 PresentMon 平均延迟/帧时之比推断精确排队帧数。

### DrawLog 自然视口准入

对于已编译支持此选择器的编辑器，启用 DrawLog 后可设
`JCE_KPI_DRAW_LOG_VIEWPORT=natural`，只取消该日志自身的旧强制捕获。
未设置、空值、`capture` 和无效值均保留旧行为。FrameLog 有独立的
`JCE_KPI_FRAME_LOG_VIEWPORT`；同时启用两种日志时，两者都应选 `natural`。
显式 Game 捕获仍优先，自然观测须保持 `JCE_KPI_SHOT`、`JCE_IMPOSTOR_BAKE`
未设置。核验实际可见视口、目标尺寸和编译产物；选择器不会更新旧可执行文件，
也不认证 GPU 工作负载。绘制次数是包含编辑器 UI 的锁存整帧样本；共享
RQ/cull 数据不代表精确同帧或单视口归因。对于已重编译并导出 `gpu_stats_available` 的产物，零表示统计观测失败，一才
接纳真实零值或正数。兼容的数值回退仍为零；缺少该列的旧 CSV 零值仍有歧义。
此标记不提供后端来源帧或执行证明。CSV 格式化与文件 IO 属于观测开销，应与
普通产品吞吐区分。

### 生命周期来源追踪开关

对于支持相应诊断的原生产物，`editor_lifecycle_probe.py --trace` 可附加
`--shader-provenance`，观察 CPU 着色器获取和链接；也可附加
`--filesystem-open-failures`，记录有预算的 host 打开失败详情。两个显式
选项均在 IO 前验证、在常规环境过滤后启用。需要资源及退出见证时，再加
`--resource-trace --require-terminal-ack`。分析复用同一权威读取器及其哈希；
这些开关不会升级旧二进制，也不认证所有模块。
FS 详情仍为原始 WARN：包含 task、parent、generation、请求哈希、操作以及
最多128字节的路径/错误 UTF8 前缀、长度和截断状态。每 Engine 会话最多
保留64组。错误文本不是 errno，可选性和投递完整性仍未知；当前类型化
FS-read 读取器不解码这些文本。保留缺失及预算限制，不把 CPU 记录当作
GPU 完成、物理呈现或性能提升。

### 深度预通道的存储证据

内部 depth-only 请求只是存储偏好，不证明物理帧缓冲已采用纯深度布局。
保留实际后端、消费者需求、normal/albedo/velocity/depth 句柄和目标代际。
provider 可能拒绝偏好布局，随后成功使用完整 MRT 回退；回退仍须保留原偏好
请求键，避免每帧重复分配。计数端口仅证明所有权和转发；少两个 RGBA8 平面
的名义容量为 width*height*8 字节，不等于实测 GPU 提交内存下降。固定原版与
候选 EXE、场景、分辨率、相机、画质和观测条件，对照动态画面及深度、法线/
速度特性切换、失败回退、复用和完整退出。在同条件整帧数据成立前，不把
省略未消费的颜色存储或清除称为帧时收益。

### 外部采样的时钟证据

进程采样以精确十进制字符串纳秒记录高精度性能计数器原点、汇总 OS 查询
起止边界，并记录运行时时钟实现及分辨率。线程、标记、期限和注入的 Windows
GPU 观察器共用该原点。样本覆盖查询区间，不是某一瞬间的帧状态。关联 CPU
阶段前须验证实际产物、依赖包、同次启动的时钟代际、导出精度及查询边界；
时钟名称本身不足以证明关联。JCE resource.sample_ns 为 SDL 启动相对时间，
不同于 Trace 的原始 QPC 时间。缺少原点的旧记录仍不可关联；first_frame_s
表示文件可见时刻，不是实际首帧或启动事件。默认必需阶段汇总不保留成功
Play/Stop 的绝对区间见证；需要时使用下述显式区间选项。单独修正采样时钟
不补齐该关联，也不证明 GPU 完成或性能改善。

Engine 所有的 FS 文本预算在 Engine 分配后才开始。应用描述工厂中的启动
配置读取可能保留数字事件而没有 path/error 文本；这不证明捕获丢失或可选性。

### 必需服务区间见证

权威读取器的可选 `--scope-witnesses` 保留必需服务首末成功 CPU 区间、时间戳
及原始事件索引，最多128个显式名称。编辑器生命周期探针配合 --trace 接受
同一选项，并要求实际 Play/Stop 服务。ACK 与非 ACK 路径复用一次完整读取及
其指纹；默认输出不变。样本按导出完成顺序保留，重复服务的首末包络不等于
完整时间线。关联前仍须验证同次运行的时钟域与产物身份；该选项本身不对齐
OS 样本、SDL 相对资源计数或 GPU 执行。合成组合测试与原生应用证据分开。

对于重新构建的程序，显式启用渲染资源 DIAG 和 Core 追踪后，
`sr.sky.admission` CPU 服务通过作用域内的 `sky.*` 计数器记录实际调用方与视图门控、
已准备的渲染上下文、CPU 帧与视图、原始程序索引以及实际上传的天空参数位模式。
策略跳过为成功；程序索引哨兵无效、瞬态分配被拒绝为失败；提交成功仅表示 CPU 提交调用返回。
索引准入只是哨兵检查，不能证明程序代际或后端存活，
`sky.gpu_completion_known=0` 不能认证 GPU 像素。不得为了观察而解析程序，
也不能从仅用于叠加相机的配置字段推断基础相机清屏模式。
预期的诊断失败不得列入必须全部成功的生命周期阶段。
规范读取器验证全部事件，但其必需阶段摘要并不完整投影任意作用域计数器；
比较场景与 Bundle 切换时，应在同一次完整验证旁保留有界的选定作用域投影，
并明确标记观测缺失。默认行为不变；使用数据前必须验证实际重新构建的可执行文件。

已准备的渲染上下文标识渲染上下文，不是唯一的场景或 SceneRenderer 对象身份；
场景身份仍须通过其既有所有者关联。

验证渲染顺序改动时，将捕获帧身份关联到实际绘制顺序、附件子资源、深度与
混合状态和 shader 绑定。在下一个消费者前比较原始颜色和深度，再比较两个
不同姿态的原生最终画面。串行单绘制时间与像素调用数仅描述该次回放。
原生整帧 ABBA/BAAB 单独验证，重复结果稳定后再更改默认路径，保留相冲突
的尾部帧时和 1% Low 观测。未改 shader 的回放计数也发生漂移时，分别报告
通过样本与像素调用。GPU Engine 样本按 PID、进程创建时间和适配器 LUID
关联，同 PCI 标识下的多个 LUID 分开保留。计数列表截断和观测间断须明确
拒绝验收。

验收采样深度的混合通道时，在同一绘制核对实际深度 SRV 和输出附件。仅颜色
别名允许 RTV 视图标识改变，但底层颜色资源和子资源必须一致。按顺序核对
两次混合，并确认通道没有引入深度拷贝或第二张输出纹理。CPU 侧的复用与
回收测试和原生图形绑定验收应分别记录。

终止时导出环形追踪须记录保留区间及覆盖事件数。核验目标 Play 区间内完整
scope 的 QPC、线程与帧身份。环形缓冲只保留 Stop 或编辑器帧时，Play scope
仍为不可用。分别保留同 ETL 的 Present 匹配、进程 GPU Engine 采样区间和精确
DWM token 到调度事件的匹配。跨线程 QPC 重叠无法单独证明应用在等待
Present 或 DWM。
