---
name: jce-zh-cn
description: 用于需要简体中文 JCE 指南的引擎与编辑器开发、通用构建工具、SDK 消费项目及验证任务。
metadata:
  language: zh-CN
---

# JCE 简体中文译版

默认版本为 [英文 en-US](../jce/SKILL.md)，本版为其对应译版。
长期维护源码位于本仓库 skills/jce-zh-cn，本地运行入口通过链接读取仓库版本。

阅读 AGENTS.md、README.md、相关公开 API 和最近的模块规范。
用户任务范围、语言偏好及已授权的 Git 操作优先。
消费项目使用公开 SDK，复用已有组件和运行时机制。
通用工具显式接收输入；项目内容及专用工具归属 examples/。
第三方原始源码和包必须保持不变。

## 任务参考

只读取当前任务相关的参考文档。

| 任务 | 参考 |
| --- | --- |
| 架构与职责 | [架构与职责](references/architecture-map.md) |
| 代码与文本规范 | [代码与文本规范](references/code-style.md) |
| 构建与验证 | [构建与验证](references/build-and-gate.md) |
| SDK 消费与分发 | [SDK 消费与分发](references/user-project-sdk.md) |
| 编辑器接入 | [编辑器接入](references/editor-panel-workflow.md) |
| 编辑器与运行时一致性 | [编辑器与运行时一致性](references/runtime-parity.md) |
| 脚本与语言绑定 | [脚本与语言绑定](references/scripting-bindings.md) |
| 平台与低端要求 | [平台与低端要求](references/crossplatform-lowend.md) |
| 图像与计时证据 | [图像与计时证据](references/evidence-verification.md) |
| 能力是否真正运行 | [能力是否真正运行](references/capability-liveness.md) |
| 编写消费项目 | [编写消费项目](references/authoring-and-inspection.md) |
| CK 消费端边界 | [CK 消费端边界](references/consumer-ck-production.md) |
| Git 工作流 | [Git 工作流](references/git-and-worktrees.md) |
| 工具链发现 | [工具链发现](references/toolchain-reference.md) |
| 长期维护的仓库状态 | [长期维护的仓库状态](references/repository-state.md) |

## 验证与公开边界

通过 scripts/jce.py 执行构建、测试和检查。源码检查与架构审计是分别需要完成的验证。
公开头改动需要刷新 SDK 并验证消费端；语言绑定改动需要运行两个生成器。
画面和计时结论需要真实抓图与时钟证据。缺失或不可用的私有检查不能算通过。

未公开的 AI CLI、Agent、场景与物理策略、私有提示词及交付记录不能进入公开源码或 skill 包。
通用模型传输、SDK 和编辑器接口、普通场景与物理能力仍属于可复用的公开机制。
保持两种语言版本的参考文档清单和命令示例同步。
