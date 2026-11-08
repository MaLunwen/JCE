# 架构与职责

修改模块前阅读 AGENTS.md 和最近的模块规范。
长期维护的模块地图位于 contracts/module-memory-index.md。

- engine/include/jce/ 维护公开 C99 API；engine/src/ 维护实现。
- editor/ 和 examples/ 消费公开 SDK。消费项目不包含引擎内部头或第三方头。
- scripting/ 在同一套 C ABI 上实现语言适配。
- tools/ 维护显式接收输入的通用自动化；scripts/ 维护手动入口和转发入口。
- 游戏内容、数值、打包默认配置及专用工具归属 examples/ 下的项目。
- contracts/ 维护稳定契约。本地计划与交付记录放入被忽略的 docs/。
- 未公开的专用 AI 工作流放入 private/；公开核心构建不能依赖它。

依赖方向由底层 OS，经渲染、中间件、运行时和应用层指向消费端。复用 contracts/dependency-ownership.yml 指定的唯一职责实现。新增包装器不能成为重复实现分配、调度、物理或渲染的理由。

静态实例速度预处理资源由各 SceneRenderer 持有，使用
`engine/src/middleware/scene/jce_sr_cull.c` 私有 prepare/release 函数。任何失败路径
之前初始化无效句柄；队列准入失败须退休刚加载的 program。场景销毁经既有
program/queue 所有者释放资源，多视图保持独立状态。源级控制须结合重建产物
的实际追踪；CPU 夹具通过不等于后台分配闭包或 GPU 完成验收。

深度预通道队列同样由各 SceneRenderer 持有，在同一 cull 所有者中使用私有
`sr_depth_prepass_queue_prepare/release` 函数。保持延迟分配、准入前清空队列，
分配失败时继续逐实体绘制。销毁释放队列，新生命周期重新创建。构造回滚须在
下一块分配失败时释放此前成功分配的所有 cull 缓存。使用实际所有者路径、
失败注入和编译执行的旧行为负控验证；这些 CPU 检查不证明整个进程堆闭合
或 GPU 已完成。

`engine/src/renderer/jce_text.c` 在既有字体子系统中共享存活的不可变原始字节。
先读取实际内容，再用公共 archive 内容哈希、长度与完整字节比较匹配；名称
和挂载标识不能证明内容相同。FT/HB face、字号、SDF 设置及图集各自独立，
调用者缓冲仍由调用者持有。先关闭 HB/FT，再释放最后一个原始字节引用；
引用归零立即移出并释放。稳定 blob ID 与既有 trace scope 记录引用和字节
生命周期，溢出明确拒绝准入。保留字体 API 的同线程契约。共享机制的峰值、
RSS 和 IO 收益须在实际构建中测量，其他字体引擎继续使用各自所有者。

`engine/src/renderer/jce_shaders.c` 统一持有 shader 阶段到 program 的链接事务。
关闭 bgfx 自动阶段销毁，在成功与失败后均显式释放两个创建者引用；成功的
program 保留自己的阶段引用。原生 PAK、文件系统、overlay 和延迟实现路径
复用同一私有所有者，保留资产键、backend 失败日志与通用链接结果、释放数量
追踪。计数端口的运行时负控和实际 Noop stock 字节生命周期测试只证明各自
所有权范围，不证明物理 shader 执行或画面正确。

`engine/src/os/core/jce_trace_capture.c` 复用既有 Chrome 编码器的有界消费者
缓冲，将 host 写入在约 1 MiB 合批。保留生产者 ring、全部生命周期事件及
完整性门禁；耗尽 drain 预算立即继续，编码、短写及 CloseIO 失败均拒绝验收。
先 join 再释放 staging。footer 的 writer 统计只描述该消费者的 CPU/IO
尝试与停顿，不构成应用或 GPU 性能认证。
正式目标须是调用者独占的新路径：私有 host-FS reserve owner 用 SDL `wbx`
独占创建正式文件并写入非 JSON 的 PENDING 标记，再独占创建 nonce sibling
partial。路径查询失败不能证明不存在。确认 footer 写入、CloseIO、完整历史
及原保留标记逐字节未变后，才经公共 host rename 替换本次标记。失败标记和
partial 均保留作诊断；即使 partial 含正面 JSON，也不能替代指定正式捕获，
reader 必须拒绝 PENDING。既有文件/目录拒绝覆盖；调用者负责阻止并发或恶意
外部替换，read/rename 间隙不声称提供这类保护。

公开 `jce_fs_host_copy_file_new` 将明确的 host 源流式复制到独占创建的 host
目标，临时缓冲不超过64KiB。必填 out_created 初始为false，仅本次实际创建
目标后变为true，后续IO/关闭失败仍保持true。既有文件/目录不被覆盖；成功须
完整传输、终端EOF及两个流关闭成功。只有created目标才属于调用者可清理文件。
既有FS trace记录传输/关闭结果；此API不保证fsync持久性或并发变化源的稳定快照。

私有 scene/jce_scene_line_colors_internal.h 在完整 JSON 解析和严格准入的
LineRenderer 局部颜色更新间共享字段规则。保留通用回退、未使用存储的规范化
以及既有 typed setter/OnSet；调用结束后不保留 ECS 活指针或序列化镜像。
CPU 分配和所有权控制不证明整帧提速、画面或 GPU 完成。
