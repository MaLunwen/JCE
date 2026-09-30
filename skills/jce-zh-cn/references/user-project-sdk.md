# SDK 消费与分发

消费端包含 JCE 公开头并链接匹配的安装 SDK，不包含 engine/src/ 或直接使用第三方头。引入引擎功能前检索公开 API 与组件配置。

```bash
python scripts/jce.py sdk --arch x64 --variant dist --no-debug
python scripts/jce.py smoke --arch x64 --variant dist
python scripts/jce.py package editor --arch x64 --variant dist
python scripts/jce.py package editor --arch x64 --variant dist --standalone
```

用户项目使用受支持的 cook、app、package、accept 子命令，并显式传入项目目录。CMake、清单、图标和专用工具随项目放置。选择参数前阅读各子命令的 --help。

公开头改动需要刷新请求覆盖的 SDK 并重建消费端。已安装头属于构建产物，不能手工修补。库、头、语言适配、图形层级和变体应来自同一配置。

dist 使用 Release 编译，关闭 Tracy 和专利编解码路径。release 与 dist 是不同产品，不能将旧可执行文件改名为 dist。核验包内二进制和运行依赖，不能只看 preset 名称。

随适配器暂存所需托管运行配置、Java 类和 Python 模块。说明所需外部 .NET、JVM 或 Python 安装。附带项目及依赖许可，不分发未核验的本地字体或资产。

验收需包含搬移后的分发包、全新用户状态、无源码树资产回退、代表性的语言与运行时检查及实际查看的截图。记录可执行文件哈希、源码提交、变体、平台和测试范围。核验交付内容后，打包才算完成。

Windows 原生单 EXE 交付使用 --standalone。它使用独立的静态 CRT 依赖图，
内嵌编辑器资产并链接 Lua、C、C++、JavaScript。SDK 与外部 Python、Java、
C# 运行时单独交付。不能用 SDK 或运行时的自解压包替代原生单文件执行。
核验常规及延迟 PE 导入，并将唯一 EXE 搬到空目录启动验证。
完整语言分发包仍作为不同交付保留。

Windows 原生项目的 jce_script_api.dll 可放在模块旁边。
迁移编辑器时，不需要将项目运行库放到编辑器 EXE 旁。
