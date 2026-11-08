# 脚本与语言绑定

公开脚本宿主 ABI 和 contracts/script-api.json 是长期维护的依据。各语言适配相同操作，不另建玩法 schema。

```bash
python tools/scriptgen/gen_script_bindings.py
python tools/scriptgen/gen_script_c_abi.py
python tools/audit/check_script_language_catalog.py
```

生成器默认检查自己生成的输出。契约有明确改动后才重新生成并审阅差异，不手改生成文件。遵循 ABI 规则保留已有字段顺序，新增支持成员追加到末尾。

语言清单只能证明注册，不能证明运行。构建托管产物和原生适配，在编辑器 Play 与消费端运行时执行相关语言的行为轨迹。比较返回值、宿主调用、错误处理、生命周期和 tick 顺序。

默认交付使用内嵌 Lua/JavaScript 与原生 C/C++ 应用代码。C#、Java、Python 是可选外部运行时创作/SDK 路径，不是默认交付依赖。明确选择这些开发路径时，独立核验实际 runtimeconfig、类/模块发现与搬移行为。原生 ABI 烟测不能证明项目类已注册；`.jcec`/`.jcecpp` 引用需实际静态类注册，或保留行为且受支持的可移植变体。

不支持的语言应明确失败，不能静默使用另一后端。通用 LLM 桥保留凭据与环境边界；私有 AI 策略不属于语言绑定实现。

视口呈现尺寸独立于鼠标有效性。响应布局使用追加的 ScriptHost viewport_size／
生成的 get_viewport_size 服务；pointer_position 的零尺寸仍表示输入不可用。
宿主通过带显式长度的 Runtime 创建选项，在 on_start 前提供 Canvas 单位的初始
尺寸，呈现尺寸变化时更新。可选 on_viewport_resize 复用现有消息通道，不推进
模拟时间。编辑器先发布实际 Game View 布局，再消费一次合并的待启动 Play 请求，
并将 ImGui 显示坐标映射到同一解析后的 Canvas/FBO 尺寸。此追加公共能力需要
重建 SDK 和消费者，检查两个生成器及当前尾部短 host 控制。CPU 控制不证明
真实界面操作或多 DPI 验收。

使用现有 Runtime 组件 JSON patch 服务时，仅提交脚本拥有的字段，并在安全时复用
小型传输表。不需要创建组件时，先用公开存在性检查保留缺失状态。不要为了修改
姿态而读取、解码并写回整个组件，以免同时覆盖其他所有者的字段。自定义 ScriptHost
回调遵循其自身明确的 patch 语义。以原输入和数学运算为对照，通过实际 VM 验证
组件缺失及外部字段变化；计数宿主端口不能证明原生合并器或 FPS。
