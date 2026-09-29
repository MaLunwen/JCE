"""lifecycle_py.py -- the Python half of the lifecycle differential.

The same script semantics as lifecycle_lua.lua, in Python.  Neither file is
generated: they are the two expressions being compared, and generating one of
them from the other would put the thing under test on both sides.
lifecycle_runner.c drives BOTH through the public jce_script_* entry points
over the same recording mock host.

Read lifecycle_lua.lua's header for the three rules both fixtures follow (no
number is formatted by the script, every type is reported through `tname`,
`jce.log` carries only literal markers) and why each one is there.

A Python gameplay script is a module whose top-level functions take `self` --
the instance -- exactly as a Lua script's methods do.  `self.entity` is the
owning entity; attributes written on `self` live on the instance and shadow
the module, which is `metatable.__index` in the other language.
"""


def tname(v):
    """The canonical type token.  bool BEFORE int: in Python `True` is an int
    and would otherwise report as a number, which is the one place this
    mapping could silently agree with Lua for the wrong reason."""
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "boolean"
    if isinstance(v, (int, float)):
        return "number"
    if isinstance(v, str):
        return "string"
    return type(v).__name__


# ── instance lifecycle ───────────────────────────────────────────────────

def _spin():
    jce.log("co-a")                                       # noqa: F821
    yield jce.wait_seconds(0.5)                           # noqa: F821
    jce.log("co-b")                                       # noqa: F821


def on_start(self):
    jce.log("on_start")                                   # noqa: F821
    self.ticks = 0
    jce.set_position(self.entity, 1.5, 2.25, -0.5)        # noqa: F821
    # `yield` is Python's spelling of jce.wait_seconds; the VM resumes the
    # body to its first yield inside start_coroutine, as Lua does, so "co-a"
    # belongs to on_start's stream.
    jce.start_coroutine(_spin)                            # noqa: F821


def on_update(self, dt):
    self.ticks += 1
    jce.log("on_update")                                  # noqa: F821
    jce.set_velocity(self.entity, dt, 0.0, 0.0)           # noqa: F821
    if self.ticks == 3:
        raise RuntimeError("boom")


def on_fixed_update(self, dt):
    # Logs the dt so the differential compares a VALUE, not just a call.
    jce.log("on_fixed_update " + str(dt))                 # noqa: F821


def on_destroy(self):
    jce.log("on_destroy")                                 # noqa: F821


def on_collision(self, other):
    jce.log("on_collision")                               # noqa: F821
    jce.destroy_entity(other)                             # noqa: F821


def on_ping(self, num, str_arg):
    jce.log("on_ping")                                    # noqa: F821
    jce.anim_set_float(self.entity, tname(str_arg), num)  # noqa: F821


def on_anim_event(self, id, name, f0, f1, i0):            # noqa: A002
    jce.log("on_anim_event")                              # noqa: F821
    jce.ui_set_text(self.entity, tname(name))             # noqa: F821
    jce.anim_set_float(self.entity, "f0", f0)             # noqa: F821
    jce.anim_set_float(self.entity, "f1", f1)             # noqa: F821
    jce.anim_set_int(self.entity, "i0", i0)               # noqa: F821
    jce.anim_set_int(self.entity, "id", id)               # noqa: F821


# ── named globals: the two slots whose absence is invisible ──────────────

def on_named_hit(entity):
    jce.log("named")                                      # noqa: F821
    jce.destroy_entity(entity)                            # noqa: F821


def on_slider(entity, value):
    jce.log("named_num")                                  # noqa: F821
    jce.anim_set_float(entity, "slider", value)           # noqa: F821


def on_field(entity, text):
    jce.log("named_str")                                  # noqa: F821
    jce.ui_set_text(entity, tname(text))                  # noqa: F821
