# 编辑器与运行时一致性

隐藏 Editor QA 使用显式选择的虚拟 Game 指针源，沿正常 runtime commit 顺序
提交，不伪造 OS focus/hover 或改变普通鼠标准入。记录 source 与 accepted；
合成输入通过不证明物理桌面输入。所有权、几何、可见性和 Play 生命周期变化
时清理虚拟状态。保留旧产物失败捕获，用重建后的可执行文件验证完整
Play/Stop 与退出路径后，才能宣称实际消费了输入修复。

Runtime指针commit与Canvas UI指针提交是两个消费者。Canvas使用同一bridge的
非消费、当前几何核验后的virtual peek；消费两次会清掉Runtime状态。Runtime
单元通过不证明UIButton派发，应在正常渲染宿主核验实际按钮效果、前后场景观察
及完整Stop恢复。

Canvas按钮一致性还须覆盖界外按住后移入、按下后拖出再返回、A到B拖动，以及
隐藏、禁用或指针无效后的恢复。只有新的down边沿取得按钮归属；未知指针区间后须观察up再按下。
验证Play/Stop/重启与空帧或被拒绝的render不会重放上一帧click/value轮询输出；
保留InputField text/submit各自的clear-on-read所有权。无窗口owner控制不能
代替重建后宿主的真实物理输入验收。

编辑器 Play 与出货游戏使用共享的运行时、场景和输入服务，不复制游戏逻辑。沿公开 API 追踪各宿主的创建、每帧输入和释放。

比较实际生效的运行时描述、相机设置、输入动作、脚本 tick 顺序、实体变化、资源挂载、导航、存档路径和流式加载职责。结构定义相同，不能证明两端填入相同内容。

松散项目文件、烘焙目录和内嵌 PAK 的查找路径可能不同。宣称内嵌可搬移前，将包移到独立目录，排除源码树和松散资产回退。

比较像素前统一渲染后端和有效设置。核验时间性渲染步骤确实执行，并使用同一时钟；调用轨迹相似不保证图像一致。

Windows Native D3D Auto 在现有 bgfx init 前，通过私有当前窗口 monitor
观察请求唯一 vendor/device 键；每个渲染器创建查询一次，后端回退尝试复用结果。
无法匹配或有歧义时保留原策略。Editor 和出货窗口可能落在不同物理显示器，
因此除了请求的后端，还须比较实际 selected device 与回退原因。不能把选卡
放入游戏代码，也不能推断 Auto 始终选择集显。

有界 DIST 启动记录 `GPU display adapter` 分别记录 observed vendor/device
及 LUID、实际 requested 键、selected caps 键、policy、拒绝原因及 exact-match。
`renderer.adapter.startup` 在现有 core trace 中关联 reason / observed /
requested / selected / exact-match 计数；后端初始化耗尽时结束为 FAILED。
关闭 trace 时无 ring 工作。此跨度记录 CPU 启动决策与后端报告身份，不证明
GPU 完成、原生迁移或性能；实际 owner 的 CPU fixture 与普通断言失败负控
不能替代原生选卡和画面证据。

当前 Windows PE 高性能 hint 仍为 `NvOptimusEnablement=1` /
`AmdPowerXpressRequestHighPerformance=1`；公开 `integrated` / `discrete`
仍是 Intel / NVIDIA 品牌别名。名称和 Auto 匹配均不能保证驱动路由或改善
Copy 流量。私有选卡边界见[平台与低端要求](crossplatform-lowend.md)；比较
Editor 与可执行程序性能时，保留明确的显示器、设备和实际呈现设置观察。

Editor Play 使用独立的 preview 呈现上限：`editor.frame_rate.play` 默认为 60，
0 表示不增加编辑器上限。项目目标 FPS 与 preview cap 都为正时取较小值；
项目为 0、preview 为正时取 preview；preview 为 0 时保留项目值，包括 0。
保留创作的 VSync，创建失败或 Stop 时恢复进入 Play 前的显示设置。
渲染配置未解析时仍启动 FPS transaction，并保留进入 Play 前的 VSync。
此偏好不改写项目/运行时 FPS 意图、固定 dt 或捕获协议；临时暂停或失焦
限速结束后恢复合成的运行目标。平台可执行程序仍消费项目原有意图，
跨宿主比较须明确匹配实际呈现上限。

```bash
python tools/lint/check_runtime_desc_parity.py
python tools/lint/check_project_settings_consumed.py
python tools/lint/check_play_mode_isolation.py
```

两端都验证多次 Play/Stop 和语言后端。运行时释放不能破坏编辑场景选中，也不能将资源泄漏到下一次模拟。报告实际测试的语言与后端组合，不能只说场景打开了。

创作场景恢复与宿主预览恢复分别验证。Game 停止态预览相机须在 Play、创建失败、
隐藏视口 Stop 和重试后保留完整姿态与投影。运行时相机变化由现有所有者隔离；
不完整的公开 getter 快照不能保存正交比例或 roll。相机状态与实际视图都要验证。

应用验收包含重复编辑器模拟、对应平台可执行程序运行，以及在编辑器重新
打开打包的 bundle。三种宿主使用同一服务并比较实际设置，覆盖脚本/运行时
生命周期与迁移验证。仅编译通过不足以完成验收。

通过 `jce_runtime_set_pointer_position` 提交视口内逻辑像素坐标，并同时提交
位移、按键和触控。视口尺寸为零明确表示当前不拥有输入。
原生脚本模块通过 `JceScriptHost.cursor_set` 选择光标资产，由窗口宿主创建
平台句柄并决定显示优先级；不要调用模块自己静态链接的 SDL 创建光标。
场景纹理的工作线程解码和渲染线程上传使用公开的 `JceTextureCpu` 服务，
保留完整 cooked mip 链。只解码基础 mip 的图片预览不能保证渲染一致性。
用 `tools/compare_captures.py` 比较相同尺寸的实际截图，明确区域并报告误差；
仅排除已说明的诊断覆盖层，不能通过对齐或改色隐藏差异。

UIButton 颜色反馈必须保持图形层级和裁剪；用真实抓图检查弹窗遮挡和按钮文字。启动异步恢复应将场景 Bundle 交给隔离的 Bundle 加载器，不能把二进制 Bundle 当作场景 JSON 解析。

Canvas child traversal and equal-sort-order root canvases must use stable entity order. ECS component enable/disable changes migrate archetypes and must not change painter or hit-test ordering; keep the overlapping-button enable-toggle regression.

### 指针相机与稀疏场景

仅使用创作相机的默认宿主应在脚本更新后、绘制前重新应用场景主 Camera。编辑器模拟的指针归属不依赖 Virtual Camera；须将显示的 Game 图像中的位置和位移都映射到实际渲染像素。SDL 按钮事件在按钮状态变化前发布自身的绝对位置。明确指定但仍在加载的模型不能回退为无关基础图形。相机诊断默认关闭，启用时使用有界的相机与宽高比缓存。

动态对象通过公开预制体接口按需生成并复用，同时声明脚本依赖 sidecar。创作不会自动更新已有 cooked 目录；构建前须执行正式 cook。可复用 cook 驱动通过 `--rgba8 --mipmaps` 支持明确不压缩的图像及完整、考虑色彩空间的 mip 链。UTC 纪元毫秒与单调性能时长应分别使用。 核对原作时钟语义，同时以用户要求的玩法设计为准。现代复刻应让运动和计时共用基于时间的模拟时钟，限制补算工作并声明挂起策略；需要时仅在显式诊断中保留旧式按渲染帧推进行为。验证不同呈现帧率下的状态等价。

明确创作的模型材质应在就绪后整体可见：一同请求所有非空贴图路径，直到几何与全部贴图驻留后才提交颜色和阴影。等待加载的创作模型仍占有绘制路径，不能回退为基础图形；贴图未完整时不能闪现嵌入默认材质。

SDL 游戏输入应分别消费物理鼠标与触摸流一次。私有转换层拒绝由触摸合成的鼠标镜像及由鼠标合成的触摸镜像；原始应用/窗口事件分发仍保留 UI 兼容路径。不能全局禁用触摸 UI，也不能在各游戏重复实现镜像过滤。刷新 SDK 后验证原生转换层负例、浏览器单指/双指/三指手势及 UI 控件的触摸激活。

仅需单指针的 UI 消费端通过 jce_input_primary_pointer 传入逻辑表面尺寸。该接口在真实输入及回放中保留主触点释放位置，同时保持游戏鼠标/触摸查询独立。默认 Canvas UI 复用此接口；过滤 SDL 镜像后须实测触摸按下/释放能激活创作按钮。

Web 的 SDL 实时适配层在中立事件摄入前，将包含末端像素的触点坐标转换为逻辑表面归一化坐标。浏览器触摸手势应匹配客户端像素轨迹；修正归属平台适配层，不能在各游戏或 UI 消费端分别补偿。

## 生命周期归属与完整捕获

启动、应用服务和销毁阶段复用现有 jce_trace；嵌套同步工作结束后恢复父任务归属。
JCE_TRACE_EXPORT 请求开启捕获，在生产者结束后仅导出仍驻留的记录。先核对
jceTrace 的丢失与代际元数据，再判断捕获是否完整。使用
tools/lifecycle_trace_report.py，显式传入 --require 阶段名称；失败、取消、
未配对、重置或被覆盖的记录不能成为完成验收的证据。CPU 阶段不证明 GPU 已完成。

事件回调中注册或注销监听器必须按入口时的身份各调用一次：已移除者不再调用，
新注册者等待下一事件，关闭注册表取消当前生命周期。验证嵌套分发、旧句柄、
大量监听器和分配失败。编辑器 Play 必须先成功保存快照才能创建可变运行时；
创建失败先释放服务再恢复场景，恢复失败保留快照供显式 Stop 重试。重复 Play/Stop
须保留创作状态。捕获、创建、销毁和恢复复用现有性能阶段，不能另建玩法循环或
项目专用分析器。

终态 Play 清理只为活动 Play、待启动请求或保留恢复点调用 Stop。已经退休的宿主
保留最终关闭阶段，但不能制造额外的 Stop 事务。分别验证未启动、已停止、待启动
和活动终态及重复清理；保留旧行为的普通断言证据和真实宿主失败 trace。

并发 PostFX 管线需要按 view_base 筛选 TAA 记录。on/off 标记表示 CPU 侧
resolve 提交，不表示请求开关或 GPU 完成。不能用 Scene 的最后一条记录推断
Game 状态；带视口注记的日志缺少目标视口时，证据不完整。

时域历史必须对应紧邻的上一渲染帧。禁用状态实际执行 apply、尺寸变化、跳过帧及历史拷贝未提交
都会使复用失效。首次及恢复帧使用当前颜色，避免读取未定义历史；功能提交与
时域画面质量仍需分别用真实 GPU 证据验证。

PostFX 即使跳过 apply 也会消耗本帧矩阵和外部运动输入，第二次 apply 需要
重新提供。验证前重置输出和 active 状态；运动模糊及景深的激活记录要求
composite 确实提交。

每帧末尾临时关闭、随后重新开启，且中间没有 apply 时，仍保留紧邻的历史；
实际跳过或禁用的 apply 会消耗它且不生成替换历史。不能只因开关变化就清除。

PlayerLoop 阶段与实际执行回调的跨度共用 core 的有界服务追踪。回调变更前保留
名称，重启后保留句柄单调性，关闭时取消活跃分发。大阶段不得依赖堆快照或在 OOM
时截断分发。渲染内存/计时快照须区分资源数量、瞬态使用量、估算与整适配器统计，
不能等同于自有堆内存或实际呈现。

DIST 文件系统、同步资产加载和最终化使用独立于 Tracy 的 core trace。
成功状态来自实际返回值或 READY；空清理记录取消。字节读取的零值不能
区分 EOF 与错误，不作为成功证据。资产最终化仅证明 CPU 准备，不证明 GPU 完成。

stock 着色器只把实际 loader 调用取得的引用记入有界栈 batch。共享的生产表
加载器复用同名 (vs,fs)；重复表格项和九个 PBR scalar alias 均为借用，不能
新增取得计数。不同真实加载可返回相同缓存句柄，只抵消这些多取得的引用。
Web deferred ID 各自释放解析时引用。使用实际重复计数及真实后端启动、绘制、
退出验证；CPU 引用控制不能认证后端生命周期或已测内存节省。

相对脚本、数据和鼠标图标地址优先读取已挂载 VFS；ISOLATED 缺项不能回退到宿主
文件或 runtime PAK。绝对工具路径保留宿主读取。core 私有有界入口在 PAK 解压前
检查元数据限额，在 PhysFS 分配前检查大小，并拒绝短读取；PAK 缓冲区直接移交，
避免第二份复制。真实 runtime/Lua/PAK 控制验证过大资产的解压调用为零；
OVERLAY 的显式松散源回退仍保留。

公开离屏目标使用有界八槽队列保留被替换的句柄，并按渲染器实际提交帧计龄。
同一帧反复 prepare 不会推进计龄；目标隐藏时仍会收集到期资源。队列已满时
新的尺寸调整请求返回失败并保留当前目标；尺寸未变的 prepare 仍可成功。渲染器关闭后端前解除关联的 GPU 句柄，调用者
可随后销毁已脱离后端的包装对象。应用只使用公开离屏 API 和正常生命周期，
不能手动调用私有 retirement/submission 接缝。真实 Noop 资源计数测试验证所有权；
GPU 画质和 Bundle 重开需要独立证据。提交帧时钟不是 GPU 完成 fence，也不构成
所有后端或设备的通用安全认证。

CPU 实例上传共用渲染器生命周期所有者，覆盖 32/64/80 字节布局。场景、粒子适配器和应用
不得建立第二套 arena，也不能从消费者调用这些私有 API。引擎维护须保留所有者线程的
预留与提交、同一提交周期内不可变的 encoder 绑定、真实的部分提交数量，以及 CPU staging
与后端退役各自的生命周期。共享 128 MiB 逻辑计费和明确的瞬态预留量是设计预算，
不代表物理驻留或 GPU 完成。调整容量策略后须验证真实多视口画面与实际提交群体；
辅助函数测试或进程正常退出不足以证明通过。禁用自动合批时仍保留显式创作的全部实例数据，
并区分队列提交统计与硬件 invocation 数量。

持久呈现尺寸通过公共 `JceRuntimeCreateOptions` 的 float `viewport_width/viewport_height`
及 `jce_runtime_create_with_options(desc, services, options, options_size)` 在 `on_start`
前按已知字段复制；原 `JceRuntimeDesc` 和 legacy create 保持不变。零 options size
保留 legacy 行为；非零 size 的 NULL、短前缀或非法尺寸拒绝创建。两维都为 0 表示
不可用，否则两维均须有限且位于 (0, 65535]。`jce_runtime_set_viewport_size` 发布
持久 Canvas 像素尺寸，独立于 pointer、focus 和 hover；尺寸变化经既有脚本 message
调用可选 `on_viewport_resize`，不产生输入、额外 tick 或 dt 变化。追加的
`JceScriptHost.viewport_size` 按调用者 `host_size` 复制，旧 host 缺少该回调时返回
0,0；Lua 的 `jce.get_viewport_size()` 返回 width,height，resize 回调可据此重新布局。

原生 DPI 输入使用 `jce_window_get_point_size` 读取 `jce_window.c` 缓存的真实 SDL
window points；`jce_window_get_size` 为 drawable/Canvas pixels，`jce_window_get_logical`
为配置 logical 尺寸。创建和已有 resize 路径刷新缓存，读取及
`jce_window_point_to_pixels` 转换均不查询 OS；转换不钳位，有限的界外坐标仍在界外，
无效输入返回 false 且保留输出。ScriptHost 与 Canvas 共用该转换，触摸归一化
坐标只缩放一次，无效 pointer 仍返回零。诊断分别记录 logical、points 和 pixels；
实际 owner 正负控制不能代替真实 DPI 点击/选格验收。

```bash
python scripts/jce.py test --suite engine --filter "^test_jce_(runtime_viewport(_old_initial|_old_notify)?|window_coordinates(_old_scale)?)$"
```

场景视频宿主每次准入的模拟/预览 tick 调用公共 `jce_scene_video_advance`。
它推进解码 transport，不创建纹理、上传像素或提交绘制；既有路径变更及 ECS 析构
仍可发出一次资源退役命令。普通场景渲染经私有 owner 准备最新已发布帧，独立于
`playing`，因此一次播放结束仍保留最后帧，多视口也不重复上传。原有
`jce_scene_video_update` 保留推进并准备纹理的兼容契约。纹理准备失败保留旧的
自有图像及可重试的帧计数。共同 CPU advance/prepare/retire span 不是 GPU fence，
也不能证明呈现。完整实际 owner 的计数控制，与真实后台生命周期、视频画面及
GPU 指标须独立验收。

当前完整 owner CPU 基线实测 15 项全部通过。advance 上传、playing gate 丢末帧、
失败准备仍消耗计数、遗漏实际 render-prefix 接线四个普通负控分别出现
10/1/3/8 项 Unity 断言失败，由 CTest WILL_FAIL 接纳；不能把崩溃或构建失败
计作有效负控。这是计数的 CPU 所有权证据，不是视频画面或 GPU 完成证明。

```bash
python scripts/jce.py test --suite engine --filter "^test_jce_scene_video_phases(_old_cpu_upload|_old_playing_gate|_old_failed_counter|_missing_render_wire)?$"
```

## 不可绘制状态的宿主 CPU 生命周期

桌面最小化表示失去焦点，移动端/显式 PAUSE 仍是独立的硬冻结。恢复时查询
真实焦点并丢弃一个已观察到的暂停模拟区间，不改写 wall/KPI 时钟。零 drawable
不准入渲染帧、resize、GPU 资产 finalize 或截图 hook；CPU 更新仍受共享有效
后台许可约束。`jce_engine_set_run_in_background` 与 getter 是主线程宿主属性，
默认 false，不代表作者原始 Project Settings。stock/Editor policy 保存旧值，
结合作者意图与手动/OS 暂停发布有效许可，再条件恢复。前台辅助 hooks 与
runtime pause 的既有独立频率仍保留，本轮不声称解决这一一致性边界。
捕获可以只准入绘制而不赋予模拟时间。后台模拟须通过接受时钟推进及真实场景/
相机变化证明，单张图像不够；Editor 自然视口另有准入门。验收打包 runtime
一致性时，同时记录 AUTO 实际选中的适配器/后端、已提交的 3D 目标尺寸和输出
图像尺寸。

正常与获准后台更新消费同一个真实 CPU hook owner：保留 INITIALIZATION
既有频率，依序处理 input/actions、固定时钟、subsystems、UPDATE/app/LATE。
fixed-dt 选择后的接受 dt 仍须有限且不超过 0.1 秒；后台沿用作者帧率的同一
pacing owner，不增加完成渲染帧、不满足 MAX_FRAMES。异步/子进程/flush
维护仍在原上游，冻结路径也能消费；恢复的零 dt 不被 Editor fixed-dt 覆盖。
stock 实体缓存清理延后到获准 draw；视频 CPU 推进与渲染 prepare 上传分离，
既有一次 texture 退役不证明设备完成。实际 owner 的 CPU 正负控与新的 SDK、
真实最小化/恢复、Web resize、性能及资源闭合证据须分别验收。

### VFS 整文件容量准入

公共 `jce_fs_read_all` 复用已有 bounded VFS owner，容量为 `SIZE_MAX-1`，
在 PAK 提前解压分配及普通文件分配前拒绝长度或终止符余量不可表示的输入。
公共失败输出保持零，成功输出为实际完整长度；内部 bounded reader 拒绝时保留
已观察总长度的合同不变。PAK 解压长度必须等于声明长度，成功转移已有解压缓冲区，
这两项行为变化须明确记录；不能宣称 PAK 与普通文件缓冲区都保证 NUL 终止。
SIZE32 CPU 端口仅模拟长度算术，不代表实际目标架构、GPU 或性能通过。

### Runtime 网络插值门槛

应用层 transform 与 animator render step 共同使用已有
`net_bridged && session_mode != NONE` 条件，按此顺序各消费一次 runtime 的
clock alpha；prediction、sequencer、video 保留独立条件。底层 animator 允许
直接无会话 loopback，修复应位于 runtime 消费点，不能给该 API 新加全局会话
前置条件。四种实际源码区块组合及旧遗漏括号普通负控，与原七个 L4 animator
测试分别验证接线和算法，不代表真实传输或完整 runtime 生命周期执行通过。

### 场景资源准备阶段

Scene/Game 在编辑器资产 finalize 后显式消费公共渲染线程资源准备 API，旧
render 入口保留同 owner 自动调用。scene、context、backend CPU 帧序号和
cache 事实去重未变化的准备；同轮新 terminal decode 和未上传的视频 counter
仍可触发消费。原模型上传限额与每次完整 render 的引用/淘汰时钟仍独立。
既有 scoped resource trace 记录身份与复用，不表示 GPU 完成。该阶段允许
上传/退役，但不提交场景颜色、阴影或 compute，也不推进模拟/动画时间。
完整渲染仍连续：此切片本身不能使 live_preview 有效、不能据此降为 5 Hz，
也不能证明 GPU 负载下降。SDK/ABI、实际编辑器可达性、Native/Web 性能仍须
使用独立的新鲜证据。

### 多视口骨骼演员集合

原 velocity-frame guard 继续保证共享 renderer 的骨骼采样每代一次；演员
集合改为全场景短快照，不能由首相机的裁剪列表决定。原六个姿态 pass 使用
同一 authored-order 快照，请求与完整代次实体 ID 复用同一个有容量检查的
renderer 缓冲。快照失败不提前标记代次，允许同轮无双推进重试；切换场景
时在 prune 发布新场景前失效旧接受代次，不能让扩容失败借用前场景成功。串行事件
新增的组件下一代加入，移除仍由原 has/get 检查处理。此切片没有新增宿主
CPU epoch 或低频渲染开关；SimLOD 相机策略、legacy 每 render 时间行为与
GPU 准备仍是独立边界。完整实际 owner 的 ECS/clip/player 控制及原接线
普通负控，不等价于 SDK、实际绘制姿态或性能验收。

### 保守内容存在性与可选 Mixer 默认配置

`jce_fs_probe_read` 返回 `JceFsPresence`，沿用 `jce_fs_host_read_all` 的实际
active reader 与 policy。相对地址优先查询 builtin VFS；OVERLAY 仅在明确缺失后
回退宿主，ISOLATED 不回退。绝对路径和 NULL active reader 仍归宿主读取。
没有存在性回调的 custom reader 返回 UNKNOWN；替换 reader 会撤销 builtin probe。
UNKNOWN 不能跳过高优先级源的真实读取。精确 not-found，或 stat 本次返回 OK
且已确认 PhysFS 搜索路径为空时，才允许分类为缺失。在既有 VFS 锁内观察搜索路径，
stat 前清除旧 TLS 错误。本次查询错误、非普通文件、分配失败和无法分类的适配器
均保持 UNKNOWN；只有本次 OK 不能绕过非空的高优先级源。PRESENT 与 ABSENT
只是元数据观察，不表示成功读取内容，也不是持久快照。`fs.probe_read` scope
成功不能证明已经读到字节。

Mixer loader 仅探测 NULL/空路径的隐式默认配置。证明 ABSENT 后跳过该次读取，
仍保留既有直接 runtime PAK 后备或真实默认配置；该后备不重新定义文件系统策略。
显式路径和 UNKNOWN 必须消费真实 read service，保留 active VFS、custom reader
及宿主覆盖的优先级，再尝试 runtime PAK。存在但格式错误、长度非法、分配/解码
失败及显式配置错误仍是失败，不能归为可选缺失。`runtime.mixer.configure` 一次
记录地址哈希、选中/尝试的 source、outcome、读取尝试/失败及 applied/default-seeded
状态。即使 PAK 后备提供可用内容，原 FAILED IO 也必须可追踪；填入默认配置不能
把此类失败改成配置成功。

通过真实 owner 控制验证策略优先级、custom reader、仅打包内容、解析/分配错误
及普通遗漏接线负控。实际目标 OS 的缺失/访问/非普通文件行为、安装 SDK 刷新和
应用消费迁移后的 PAK 分别验证。CPU 控制与 SDK 构建不证明 GPU 完成、画面质量
或已测性能。

### Native 清理所有权

优先使用 `jce_engine_create_owned` 和 `jce_engine_try_destroy`。创建失败仍可能
返回非空清理 owner；必须保留它，直到成功清理将槽位置空。待清理期间禁止后续
业务回调和第二个 Engine 会话。不能仅因收到终止结果或调用了 void 销毁函数，
就清空 SDL/JNI/Java 句柄。旧 create 失败后的待清理槽由 `jce_engine_destroy(NULL)`
或下一次旧 create 重试。该次 create 只清理，即使成功也返回 NULL；之后重新配置
并显式创建下一会话。它不是自动运行的后台回收器。

Engine 在进入 DRAINING 前关闭私有的默认异步执行器懒创建准入。退出/子系统
回调及会话间隙不能重新创建默认线程池；只有下一次被接纳的 Engine 构造，在
分配并发布 owner 后才重新开放。早期拒绝及旧接口的仅清理调用不能重新开放。
该开关不撤销已有 borrow，也不改变独立执行器的所有权：现有默认执行器仍可
查询，直到原 owner 线程关闭它。Standalone 的公开 default shutdown 后重建
语义不变。复用 Engine/Core 的共享所有者，不在用户项目另造线程池守卫。
通过 `contracts/module-memory-index.md` 定位私有 session 实现与实际
`test_jce_engine_async_retirement` 控制；2026-10-18 retirement-r3 已实际通过，
旧关闭调用负控独立触发普通断言。产物变化仍需重验，旧二进制不继承新资格。

验证 app 初始化失败与 sampler 超时、join 失败、resume 失败的组合、重复拒绝、
回调重入以及恢复后恰好一次释放。每次拒绝及其原因均保留在追踪中。CPU 清理
闭合不能证明物理 GPU 已完成或整帧性能已提升。

重试追踪使用 `lifecycle_trace_report.py --cleanup-ownership`，并要求实际成功的
阶段（如 `engine.async_shutdown`）。通用 `--require engine.cleanup_attempt`
会拒绝此前合法的清理拒绝；保留首个 owner 与重启各自的输出路径。终止诊断顺序是：
join 采集线程，排空/join 日志线程并保留同步文件/sink，封存追踪，最后释放日志
存储。注册线程计数不包含原生 SDL 日志线程，还须检查 `engine.log_quiesce` 完成。

项目渲染配置属于启动依赖。共享 Bundle packer 将
`Settings/RenderPipeline.rp.json` 映射为 `settings/render_pipeline.rp.json`；
存在但无效/冲突的文件必须拒绝打包。逐宿主核对实际 Bundle 字节与运行时策略。
Editor Bundle 通过显式 archive owner 的 `jce_fs_read_pak_bounded` /
`jce_render_pipeline_load_fs` 读取，不能继承全局 loose-file 优先级。保存项目意图
使用 `jce_render_pipeline_get_requested`，不能用已被当前硬件限制的描述符代替。
`render_pipeline.publish` 的 `rp.*` 计数记录解析后的有效 CPU 策略，不证明 GPU
pass 执行。恢复漏装的配置属于正确性修复，性能对照必须说明这一依据。
非限帧 GPU 满载本身不能证明浪费；同时测普通限帧负载及实际适配器的单位帧成本。

### Play 期间的编辑器内容时钟

Scene 相机导航保留原有受限墙钟 delta。会话期间，Scene 视口的反射、主渲染、
全屏内容效果和 Canvas 消费已接纳的 Play 渲染 delta；Pause 由既有 Play
所有者发布零值。停止后的创作预览保留原有墙钟，Game 消费相同的运行中
delta。调整这条路由时保留所有渲染与资源准入以及墙钟时间戳更新。CPU
路由控制不能证明 GPU 首视图所有权、时间性像素或功耗改善；单步 delta
发布和渲染侧绝对时间仍须单独验证，本规则不认证它们。

## PostFX 输出与 Game 叠加层

apply 后使用当前结构大小和版本调用 `jce_postfx_get_output_target`。owned
纹理/FBO 对记录真实分配尺寸及格式；空链返回 borrowed 输入，没有可写 FBO。
句柄仅借用至下一次 apply、resize 或销毁。CPU 准入不证明 GPU 完成或上下文
存活。叠加提交必须保留颜色，且不依赖场景深度或模板。

Game View 仅在 D3D11、当前视口/bridge/输出尺寸及格式一致、附件独立，且
没有物理调试尾部时，将屏幕空间 Canvas 直接绘制到该输出。其他情况保留
bridge composite。显示和捕获须选择同一最终纹理；Scene View 与独立程序
呈现保留原路径。实际像素及动态输入验收前不能宣称一致；移除一次拷贝
本身不能证明整帧提速。

## AudioSource 待播放请求

`jce_runtime_audio_stop` 停止已登记的声音和待播放意图，也覆盖已完成但尚未被
下一次 runtime poll 消费的解码。未提交给执行器的排队项立即释放；已提交任务
先撤销播放意图并请求取消，其借用的解码存储保留至终态 poll 或既有场景清理的
discard 屏障。Stop 不阻塞等待解码，后续 Play 创建新的播放意图，不叠加旧请求。
完成处理还会在上传和播放前核对完整实体绑定及捕获的 clip 路径，避免替换后的
片段消费旧解码结果。异步场景创建时声音数为零本身属于预期，不能据此认定资产
丢失。CPU 所有者控制验证的是这些生命周期规则，不能证明解码保真度、实际可听
输出、GPU 行为或整帧性能提升。
