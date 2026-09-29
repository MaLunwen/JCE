# Minesweeper / 扫雷

与 `examples/snake_seven` 并列的 JCE SDK 用户项目。通过公共 SDK、C 原生脚本、Lua 和场景 UI 组件运行。没有修改引擎或编辑器业务代码。

## 启动

在仓库根目录依次执行：

```powershell
python scripts/jce.py cook examples/minesweeper
python scripts/jce.py build-project examples/minesweeper
python examples/minesweeper/tools/run.py --provider ollama
```

必须先 cook：本次实测仅 build-project 会沿用已有的陈旧 cooked 场景，出现构建成功却加载旧空场景的情况。

输出为 `dist/games/Minesweeper-release-x86_64/Minesweeper.exe`。双击可离线玩；AI 功能需要 `tools/run.py` 配置本地 Python 和 CLI。
编辑器打开本目录的 `jce_project.json` 后 Play；首次需先构建出 `native/build/mines_controller.dll`。

## 操作

- 左键翻格，右键插旗／取消，中键尝试数字快速翻开。
- F 切换插旗模式，N 重开，Esc 返回菜单；数字 1／2／3 选择经典难度。
- 三种固定难度：9×9/10、16×16/40、30×16/99；另有可调自定义棋盘。
- 首次翻格才布雷，首格和八邻域安全；胜利不要求插旗。
- AI REMIX 生成下一局自定义尺寸、雷数和配色。当前棋盘不会重新布雷。
- 最佳时间以棋盘尺寸与雷数分别保存，默认在用户本地数据目录 `JCE/Minesweeper`。

## AI 接入

四协议都通过本地网关 `127.0.0.1:11435`，后端均为 Ollama 的 `qwen3.5:0.8b`。它们不是四家云模型，也不代表实现了各厂商完整 API。

| provider | 路径前缀 | 实测 |
|---|---|---|
| openai | `/v1` | 文本、图片、参数 JSON |
| anthropic | 根路径 | 文本、图片、参数 JSON |
| gemini | `/v1beta` | 文本、图片、参数 JSON |
| ollama | 根路径 | 文本、图片、参数 JSON |

启动器的 `--provider` 可切换上述四种格式。网关需要预先启动；本机已有 `D:/DevData/ollama/api-gateway/start.ps1`，该服务不是示例仓库的一部分。回环地址必须绕过代理，启动器已经设置 NO_PROXY。

```powershell
python examples/minesweeper/tools/verify_ai.py --image <红色方块和蓝色圆形测试图.png>
python examples/minesweeper/tools/ai_director.py --provider ollama --base-url http://127.0.0.1:11435 --model qwen3.5:0.8b --brief-file examples/minesweeper/ai/director-brief.txt --out examples/minesweeper/build/ai/proposal.json --send
```

没有 `--send` 时只预览请求。`ai_director.py` 复用 JCE 的统一传输，只允许六个整数，不执行模型输出的代码或路径。
`author_scene.py` 使用现有 automation changeset API 创建场景；它保留现有实体，修改已存在场景请使用组件编辑 API。

## 组件和语言的职责

| 数据／模块 | 职责 |
|---|---|
| Canvas、UIImage、UIText、UIButton | 引擎负责绘制、布局解析和按钮命中；示例更新固定 UI 池 |
| InputIntent | Lua 发布操作序号，C 脚本消费一次 |
| CustomSettings、Theme | 玩法参数与配色的组件状态 |
| AiProposal、AiColor、AiVersion | 模型提案暂存；Lua 校验并发布 |
| director.lua | 输入驱动、组件校验；不运行模型生成的脚本 |
| MinesController / board.c | 扫雷领域规则，独立程序与编辑器共用 |
| main.c | 公共 SDK 启动、存档、异步 LLM 服务连接 |

这是生命周期的一个小切片：CLI 创作数值 → automation 场景 → 编辑器 Play → SDK 打包 → 运行时提案 → 组件生效。复杂 3D 项目的资产、预算与迭代建议见 [WORKFLOW.md](WORKFLOW.md)。

## 验证与限制

```powershell
python examples/minesweeper/tools/verify_game.py --extended --editor --ai
python examples/minesweeper/tools/test_ai.py
```

真实证据在本地 `build/verification` 和 `build/ai`，这些构建输出不入库。已完成项目见 [REPORT.md](REPORT.md)。
编辑器 Play 当前仅会话内记录成绩；SDK 独立程序负责持久化和 AI 子进程。编辑器可通过 F 模式插旗；右键／中键的独立程序输入适配未声称与编辑器完全一致。编辑器中的 AI REMIX 暂不能启动独立程序专用的服务连接。缺口见报告。
