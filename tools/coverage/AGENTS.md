# tools/coverage/AGENTS.md — Coverage Measurement Scripts

## 目的
为 IMPLEMENTATION_PLAN 阶段 1 的 **≥85% 行覆盖** 硬性门槛提供可执行工具。

## 入口
`run_coverage.py` — 跨平台覆盖率运行 + 门槛校验
- 输入：`--build-dir`（已配置 `JCE_BUILD_TESTS=ON`）+ `--sources <dir>`（重复）+ `--threshold`
- 输出：Cobertura XML（默认 `build/coverage/cobertura.xml`）+ 控制台百分比
- 退出码：`0` 达标 / `1` 不达标 / `2` 工具缺失

## 后端
| 主机 | 工具链 | 状态 |
|------|--------|------|
| Windows MSVC | OpenCppCoverage | ✅ 已接入 |
| Linux gcc/clang | gcovr / llvm-cov | ⏳ TODO |
| macOS clang | llvm-cov | ⏳ TODO |

## 本地用法
```powershell
# 1) 先确保测试启用并构建
cmake -S . -B build/desktop/windows-x64-debug -DJCE_BUILD_TESTS=ON
cmake --build build/desktop/windows-x64-debug --target test_jce_str

# 2) 跑覆盖率
python tools/coverage/run_coverage.py `
    --build-dir build/desktop/windows-x64-debug `
    --sources engine/src/os/core `
    --threshold 85
```

## 硬约束
- **零三方 Python 依赖**（只用 stdlib），与 lint 脚本一致
- 不写到仓库根；所有产物在 `build/coverage/`
- 安装命令文档在 `--help`，agent 看到 `exit 2` 直接知道怎么补
- 阶段 1 推进期门槛 = 85%；扩到 L3+ 后视情上调（不下调）

## 不做的事
- 不强制 CI 跑（先本地门槛；CI 接入排在 plan 阶段 1 后段）
- 不做 branch coverage（line coverage 已是 plan 要求；branch 留给阶段 2+）
