# caged_kingdom — Sandbox Game (L7 Consumer)

> Caged Kingdom is the final game consumer built on JCE. It lives alongside the other SDK consumer examples; its game identity and output name are unchanged.
>
> **Design authority**:
> - `examples/caged_kingdom/SETTING_BIBLE.md` —— 世界观 / 角色 / 区域代号 / 货币 / 女魔王编号 / 主线幕剧结构
> - `examples/caged_kingdom/SCENES_DESIGN.md` —— 单关场景设计册（entity 清单 + 烘焙清单 + v1 实现状态）
>
> 改剧情设定前先改 bible；改场景内容前先改 design 册，再编辑 JSON。

## Identity

- **Layer**: L7 (Consumer). Uses ONLY `#include <jce/api.h>` (or per-layer umbrellas). Never touches `engine/src/**` or third-party headers (SDL/bgfx/flecs/...).
- **Language**: C99 (matches engine to keep ABI trivial).
- **Output**: `caged_kingdom.exe` (or platform equivalent) + `assets.pak`.

## Layout

```
examples/caged_kingdom/
├── CMakeLists.txt
├── src/
│   ├── main.c              ← entry: jce_engine_set_app_desc(&ck_app); jce_main
│   └── game/
│       ├── ck_app.c/.h         ← JceAppDesc impl (on_init/update/render/event/shutdown)
│       └── ck_engine_smoke.c/.h ← smoke-test scene helpers
├── resources/              ← raw assets, cooked into assets.pak at build
│   └── assets/
│       ├── scenes/lightning_lab.scene.json  ← deterministic storm lab
│       ├── scripts/lightning_lab.lua        ← bounded DBM + staged discharge
│       └── sounds/lightning_lab_thunder.wav ← project-generated thunder
└── ui/                     ← RmlUI documents
```

## Rules

1. **Use ONLY public engine APIs.** `<jce/api.h>` is sufficient; per-layer headers are fine. Anything else is a layering violation.
2. **No SDL / bgfx / flecs / RmlUI / ImGui includes.** Period. Even though the game is technically standalone, the rule is enforced to prove the public API is complete.
3. **Implement `JceAppDesc`** and register before engine create:
   ```c
   jce_engine_set_app_desc(&ck_app);
   /* SDL3 main shim takes over from here */
   ```
4. **All assets** loaded via PhysFS-mounted `assets.pak`. No `fopen` to OS paths.
5. **All allocations** via `jce_malloc / jce_free`.
6. **Game logic** lives here, NOT in middleware. If you find yourself wanting to add to `engine/src/middleware/world/`, ask: "is this primitive, or game-specific?" Game-specific → stays in `examples/caged_kingdom/`.
7. **Cross-platform**: the game must build on every supported platform. Test on the 512 MB / single-core baseline.

## Common tasks

| Task | Where |
|------|-------|
| Add a game scene | `examples/caged_kingdom/src/game/` + JSON in `resources/` |
| Add a weapon variant | Reuse `<jce/middleware/world/jce_weapon.h>`, configure via JSON; balance numbers live here |
| Add a HUD screen | RmlUI document in `ui/`, mount via `<jce/api_ui.h>` |
| Wire a new editor module for play-mode | Register a `JceGameModule` (see `engine/src/runtime/AGENTS.md`) |

## Don't

- Don't `#include <SDL3/...>`, `<bgfx/...>`, `<flecs.h>`, `<imgui.h>`, `<RmlUi/...>`.
- Don't include anything from `engine/src/**`.
- Don't bypass `JceAppDesc` and run your own loop.
- Don't ship assets outside `assets.pak` (except platform-required loose files).
