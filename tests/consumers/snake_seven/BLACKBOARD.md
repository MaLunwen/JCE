# The blackboard — the one contract seven languages share

Seven languages drive this game, one module each. They do not call each
other. They agree through **entity positions**, because a transform is the
only surface present in every one of the seven binding sets.

**Position only. Never scale, never rotation.** Scale goes through matrix
compose/decompose, a zero component is a degenerate basis, and anything that
normalises a transform may hand back a different number than was written.
Position round-trips unexamined. Five entities × three floats = fifteen
slots, and that has been enough.

| Entity | x | y | z |
| --- | --- | --- | --- |
| `GameState` | state | score | tick |
| `GameMeta` | best | length | run |
| `Direction` | dx | lastTurnTick | dz |
| `Pending` | pendDx | intent | pendDz |
| `Verdict` | wall | self | ate |

`state`: 0 MENU · 1 PLAYING · 2 PAUSED · 3 GAMEOVER
`intent` (bit field, set by input, cleared by the manager):
1 = start/restart, 2 = pause toggle
`length`: body segments, excluding the head. Spec 14 says the snake starts
at length 4, so `length` starts at **3**.
`run`: increments once per new game. Every module watches it and resets its
own private state when it changes — that is how a restart reaches modules
that share no code.

## Cells and world coordinates

The board is 30 × 20 (spec 11). Cell `(cx, cz)` sits at

```
x = cx - (COLS-1)/2      z = cz - (ROWS-1)/2
```

so cell `(0,0)` is the **top-left** on screen. It is top-left and not
bottom-left because the camera looks straight down with **screen-up = −Z**
(see `tools/gen_scene.py` for the derivation). Therefore:

> **UP decreases `cz`.**

This is the single most dangerous line in the project. An inverted vertical
axis is invisible to every symmetric check — the editor and the runtime
invert together, and two identically wrong pictures compare equal. It was
wrong once already, and it was a person who noticed, not a test. The test
that can see it is in `verify.py`: it presses UP and asserts the head's `z`
**decreased**, against an absolute expectation rather than against another
run of the same code.

## Who owns what

| Module | Language | Owns |
| --- | --- | --- |
| GameManager | C | state machine, score reset, best score, restart |
| Snake | Lua | tick clock, move interval, head movement |
| SnakeBody | C++ | segment history, body follow, self-collision |
| Food | Python | free-cell spawn, eating, score and growth |
| GameBoard | Java | 30×20 bounds, wall collision |
| Input | JS | key reading, no-reverse, one turn per tick |
| Hud | C# | SCORE / BEST / state text |

Every module reads the blackboard, writes only the slots listed as its own
below, and never assumes another module ran first in the same frame. Order
within a frame is not guaranteed and must not be relied on; the `tick`
counter is what orders things that must be ordered.

### Write ownership (a slot has exactly one writer)

- `GameState.x/y/z` — GameManager, except `tick` which Snake advances
- `GameMeta.x` — GameManager · `GameMeta.y` — Food · `GameMeta.z` — GameManager
- `Direction.*` — Snake (it adopts `Pending` at a tick boundary)
- `Pending.x/z` — Input · `Pending.y` — Input sets, GameManager clears
- `Verdict.x` — GameBoard · `.y` — SnakeBody · `.z` — Food

A slot with two writers is the bug this table exists to prevent.
