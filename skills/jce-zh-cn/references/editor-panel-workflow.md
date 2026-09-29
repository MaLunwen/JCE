# 编辑器接入

阅读 editor/AGENTS.md 和最近的面板或核心规范。新增 UI 路径前检索已有面板、共享控件及公开 SDK 机制。

面板 ID 会持久化：保留已有枚举值，新增值追加到末尾。追踪注册、分发、配置序列化、菜单、标签和可见性；只增加 switch 分支不等于接线完成。

UI 文字使用编辑器 i18n 机制。通过既有职责模块注册组件默认值、序列化、检视与撤销行为。不建立第二套组件 schema，不绕过 SDK 边界。

输入和 Play 改动需验证激活、焦点、鼠标捕获与释放、键盘输入、停止恢复及多次循环后的实体选中。临时输入手势不能误拖动停靠面板，也不能让检视器保留失效的运行时句柄。

文件查看器验证包括打开时激活、历史恢复、标签切换及后台音视频暂停。运行时资源和模拟状态需要对应的释放路径。

```bash
python tools/lint/check_editor_consumer_purity.py
python tools/lint/check_inspector_undo_scope.py
python tools/lint/check_editor_consumption.py
```

执行相关交互及运行时回归和必要的编辑器构建。按职责拆分大文件，不复制面板状态。
