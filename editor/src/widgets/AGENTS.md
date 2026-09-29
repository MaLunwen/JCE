# editor/src/widgets/ — Reusable Custom ImGui Widgets

## Identity
- **Language**: C++17 + ImGui
- **Role**: bespoke widgets shared across panels (timeline strip, curve editor primitive…).

## File map
- `jce_widget_timeline.{h,cpp}` — generic time-ruler + clip-strip widget (used by sequencer, animation, audio mixer).

## Rules
1. **Pure widgets** — no editor state coupling; take params in, return interaction result.
2. **DPI-scale** all dimensions.
3. **i18n** any visible text.
4. **No global state** — instance-id pattern (caller passes `ImGuiID`).

## Don't
- Don't make a widget panel-specific; that belongs in `panels/`.
- Don't allocate per call; reuse caller-provided buffers.

## Common tasks
- **Add a widget** → `jce_widget_<name>.{h,cpp}` with self-contained API; document in header.
