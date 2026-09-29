# 编辑器与运行时一致性

编辑器 Play 与出货游戏使用共享的运行时、场景和输入服务，不复制游戏逻辑。沿公开 API 追踪各宿主的创建、每帧输入和释放。

比较实际生效的运行时描述、相机设置、输入动作、脚本 tick 顺序、实体变化、资源挂载、导航、存档路径和流式加载职责。结构定义相同，不能证明两端填入相同内容。

松散项目文件、烘焙目录和内嵌 PAK 的查找路径可能不同。宣称内嵌可搬移前，将包移到独立目录，排除源码树和松散资产回退。

比较像素前统一渲染后端和有效设置。核验时间性渲染步骤确实执行，并使用同一时钟；调用轨迹相似不保证图像一致。

```bash
python tools/lint/check_runtime_desc_parity.py
python tools/lint/check_project_settings_consumed.py
python tools/lint/check_play_mode_isolation.py
```

两端都验证多次 Play/Stop 和语言后端。运行时释放不能破坏编辑场景选中，也不能将资源泄漏到下一次模拟。报告实际测试的语言与后端组合，不能只说场景打开了。
