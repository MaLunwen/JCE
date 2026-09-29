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
