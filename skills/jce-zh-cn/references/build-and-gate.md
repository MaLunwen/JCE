# 构建与验证

使用 CLI 支持的目标和参数；scripts/jce.py 转发到 tools/build/jce.py。

```bash
python scripts/jce.py --help
python scripts/jce.py targets
python scripts/jce.py lint
python scripts/jce.py test --arch x64 --jobs 8
python scripts/jce.py test --configure --variant debug --arch x64 --jobs 2
```

lint 命令执行源码检查与架构审计。缺失的私有检查属于不可用，不算通过。迭代时执行针对性检查，交付前执行规定的完整检查。

公开头改动需要刷新 SDK 并验证消费项目。语言绑定改动需要以检查模式运行两个生成器。已提交 ABI 检查看的是 HEAD；工作区中待提交的基线不能使旧 HEAD 通过。

第三方原件只从 contracts/vendor-sources.json 的固定版本获取。核验已有原件，不修补第三方源码树或 Conan 包。仅修改配置的 Conan hook 遵循 CONAN_HOME。缓存二进制构建不能证明源码纯净或跨平台兼容。

全新测试目录使用 `test --configure --variant debug|release`。CLI 默认仅检查
系统依赖；CI 明确设置 JCE_SYSTEM_PACKAGE_MODE=install 与
JCE_SYSTEM_PACKAGE_SUDO=1。原生工具名称和 Debug CRT 策略遵循目标平台。
Linux 原生依赖元数据需要可执行的 pkg-config。应检查实际执行，特别是 WSL
继承 Windows 工具目录时，不能仅凭发现同名文件判定可用。
CMAKE_BUILD_PARALLEL_LEVEL 同时控制编译构建和 Conan 宿主、构建上下文的
依赖并行度，共用默认值为八。CTest 单独使用 --jobs/CTEST_PARALLEL_LEVEL。
资源受限的主机应保留这些限制；正式性能测量应排除同时运行的构建任务。

受支持的构建入口位于 scripts/。编译器与依赖命令由通用 tools/build 实现生成，
不手写第二套构建配方。复用兼容的目标/变体/工具链缓存，仅隔离真实 ABI 或
配置差异，避免每次试验复制整套源码和构建树。检查磁盘余量，区分出货应用
大小与 PDB、静态 SDK 库、依赖缓存和验收证据。生成的测试产物放被忽略的
build/，测试源码仍在 tests/。第三方原件和引用的原始证据保持不变。GPU 验证
不能抢焦点；仅在需要渲染时使用隐藏或非聚焦窗口。
