# 构建与验证

使用 CLI 支持的目标和参数；scripts/jce.py 转发到 tools/build/jce.py。

```bash
python scripts/jce.py --help
python scripts/jce.py targets
python scripts/jce.py lint
python scripts/jce.py test --arch x64 --jobs 8
```

lint 命令执行源码检查与架构审计。缺失的私有检查属于不可用，不算通过。迭代时执行针对性检查，交付前执行规定的完整检查。

公开头改动需要刷新 SDK 并验证消费项目。语言绑定改动需要以检查模式运行两个生成器。已提交 ABI 检查看的是 HEAD；工作区中待提交的基线不能使旧 HEAD 通过。

第三方原件只从 contracts/vendor-sources.json 的固定版本获取。核验已有原件，不修补第三方源码树或 Conan 包。仅修改配置的 Conan hook 遵循 CONAN_HOME。缓存二进制构建不能证明源码纯净或跨平台兼容。
