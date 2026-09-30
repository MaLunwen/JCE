# Git 工作流

保留用户分支、暂存改动、忽略的资产和备份引用。改写历史前查看状态和指定基线。用于验证的源码导出属于测试产物，不能成为替换用户工作区的理由。

用户保留提交与推送操作时，让改动在工作区中可审阅。明确授权改写已发布提交时，先创建 Git 备份分支，记录实际观察到的远端对象 ID。

获授权改写历史时，使用带该远端 ID 的 force-with-lease。lease 失败后先检查新的远端历史，不直接覆盖。只发布已验证的公开内容，排除本地和私有交付文件。

检查暂存空白、实际索引路径、生效的 attributes 和 ignore 结果。不可用验证与通过分开报告，交付资产记录精确源码及构建身份。

提交 `v-X.Y.Z` 版本时，同时更新 CMakeLists.txt 中的 `project(JCE VERSION ...)`、Java 的 `EXPECTED_API_VERSION` 镜像和 README 的 Version 行。提交前运行 `python scripts/jce.py lint`。CI 会核对已提交的版本主题与 CMake；设置 `git config core.hooksPath tools/hooks` 后，仓库内的 `tools/hooks/commit-msg` 也会在本机核对暂存内容。版本号变化必须使用匹配的版本主题，不匹配的版本主题会被拒绝。

用户要求独立工作区和清理时，使用受管理的 worktree 生命周期工具。未核对进行中的工作和保留资产前，不移除工作区或忽略的项目内容。
