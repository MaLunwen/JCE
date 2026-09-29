# Caged Kingdom 消费端边界

Caged Kingdom 是最终游戏，也是 examples/caged_kingdom/ 下的 SDK 消费项目。修改内容前阅读该项目的 AGENTS.md 和 SOURCE_AND_TERMS.md。

玩法、设定、数值、场景、平台图标、打包默认配置及专用生成器和测试归属项目。通用工具显式接收项目参数；特定游戏的默认配置不放入 engine/、editor/ 或 tools/。

使用 JCE 公开接口。提出新引擎服务前检索已有组件和 SDK API。项目计划不能证明某种运行时格式、任务系统或功能已经实现。

区分无头引擎、编辑器及游戏工作负载的性能要求。记录被测工作负载，不照搬历史帧率。

导入资产需要独立来源与条款。local_assets/ 及未公开内容被忽略；源码许可不能授予字体或美术权利。旧项目计划和本机验收记录不放入 contracts/。
