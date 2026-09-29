# Caged Kingdom

CK is the final game consumer, located at `examples/caged_kingdom`.
Open this directory as a project in JCE Editor. From the repository root:

```sh
python scripts/jce.py app examples/caged_kingdom --arch x64 --variant release
```

The project uses the installed JCE SDK through `find_package(JCE)`. The CMake
target remains `CagedKingdom`, and the executable remains `caged_kingdom.exe`.
Local private assets and generated output retain their existing ignore policy.
After relocating a checkout, configure a fresh consumer build directory; old
CMake caches contain absolute source/build paths.

主线内容暂时停滞，规划于 09-2026 至 01-2027 内继续实现。

## Lightning Lab

独立技术案例位于
`resources/assets/scenes/lightning_lab.scene.json`。在 JCE 编辑器中打开该场景
并进入 Play，约 1 秒后会自动生成一次可复现的三维分支闪电；下降先导会与
避雷针向上流光连接，随后出现首次回击和多次后续回击。爆裂与滚雷按观察
距离和 343 m/s 声速分层到达。运行时算法位于
`resources/assets/scripts/lightning_lab.lua`，完整模型、预算与实体清单见
`SCENES_DESIGN.md` 的“技术实验场景”章节。
