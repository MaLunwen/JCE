# 脚本与语言绑定

公开脚本宿主 ABI 和 contracts/script-api.json 是长期维护的依据。各语言适配相同操作，不另建玩法 schema。

```bash
python tools/scriptgen/gen_script_bindings.py
python tools/scriptgen/gen_script_c_abi.py
python tools/audit/check_script_language_catalog.py
```

生成器默认检查自己生成的输出。契约有明确改动后才重新生成并审阅差异，不手改生成文件。遵循 ABI 规则保留已有字段顺序，新增支持成员追加到末尾。

语言清单只能证明注册，不能证明运行。构建托管产物和原生适配，在编辑器 Play 与消费端运行时执行相关语言的行为轨迹。比较返回值、宿主调用、错误处理、生命周期和 tick 顺序。

C#、Java、Python 还需要实际运行时及部署文件。只暂存 DLL 可能遗漏 runtimeconfig、类文件或模块。记录外部运行时要求，并在搬移后验证发现路径。

不支持的语言应明确失败，不能静默使用另一后端。通用 LLM 桥保留凭据与环境边界；私有 AI 策略不属于语言绑定实现。
