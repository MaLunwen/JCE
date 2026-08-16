"""jce_script.vm — the Python half of JceScriptVM, the engine calling UP.

`jce_script/__init__.py` and `_generated.py` are the call-DOWN direction: a
script reaches the engine.  This module is the mirror.  When the engine drives
`jce_script_call_update()` on a handle created with language `"python"`, the
call arrives here.

WHY THERE IS A C FILE AT ALL, MEASURED RATHER THAN ASSUMED
----------------------------------------------------------
The call-down binding needs no compiler because the Python process already
exists and ctypes can call any C ABI from it.  The call-up direction cannot
borrow that argument, and the reason is structural rather than a matter of
effort:

  * `JceScriptVM` is a table of C function pointers that the ENGINE calls, in
    its own frame loop, on its own thread.  `jce_script_vm_register()` is a C
    call taking that table's address.
  * In the engine's process there is no interpreter until somebody calls
    `Py_Initialize()`, and that somebody must be C.  Nothing written in Python
    can run before the runtime that would run it exists.

So the shim in `scripting/python/src/jce_script_vm_python.c` is not an
optimisation and cannot be removed by being cleverer.  It is kept as small as
that fact allows: it owns interpreter lifetime, the GIL and marshalling for
the 18 slots, and it owns NONE of the scripting surface — the `jce` a script
sees is `jce_script.open_host()`, the ctypes binding a cross-language
differential already accepted.  The two halves compose; neither is duplicated.

*Enforced by:* tests/scripting/python/test_emit_python.py ::
`test_only_the_vm_shim_compiles_c` (the wheel stays pure Python and the C is
exactly one target) and `test_the_c_shim_owns_no_scripting_surface`.

THE INSTANCE MODEL IS LUA'S, MEMBER FOR MEMBER
----------------------------------------------
`jce_script.c` builds an instance as `{ entity = owner }` with
`metatable.__index = <the table the chunk returned>`: writes land on the
instance, reads fall through to the module, and hot-reload is one assignment
to `__index`.  `Instance` below is that object: `__getattr__` is `__index`,
normal attribute assignment is the write, and `rebind_instance` swaps the
namespace exactly as `script_lua_rebind_instance` swaps the field.

So a Python gameplay script is a module whose top-level functions take `self`:

    def on_start(self):
        self.hp = 100                       # writes the INSTANCE, like Lua
        jce.set_position(self.entity, 0, 1, 0)

    def on_update(self, dt):
        if self.hp <= 0:
            jce.destroy_entity(self.entity)

`self.entity` is the owning entity id, as in Lua.  `jce` is injected into the
module's namespace before the module body runs.

ERRORS: CATCH, LOG, THEN DISABLE THAT CALLBACK — LUA'S RULE, NOT A COPY
------------------------------------------------------------------------
`jce_script.h` states THE FAILING-CALLBACK RULE and `jce_script.c` is its
reference implementation: a handler that raises is caught, `"<method> error:
<detail>"` goes to `host.log` and to the engine log, the frame is NOT lost —
and then that handler is DISABLED ON THAT INSTANCE, with a second line saying
so, because a handler that raises every frame otherwise writes sixty log lines
a second about one defect.  This module does the same with `try/except
BaseException` plus a per-`Instance` set of disabled handler names, and the C
shim performs the log writes so the routing — which lines the host sees — is
decided in one place for both languages.

Only the four repeating FIXED-NAME handlers participate (`on_start`,
`on_update`, `on_collision`, `on_anim_event`); `on_destroy` is dispatched once
and its instance is dropped immediately after, `call_message`'s method name
comes from the caller, and the named-global dispatchers have no instance to
disable anything on.  `rebind_instance` clears the set, which is what makes
"fix the script and save" work.  All of that is argued in `jce_script.h`; this
module implements it and does not restate the argument.

One routing detail copied deliberately: a COMPILE error reaches the engine log
only, never `host.log`.  `script_lua_instantiate_source` logs it with
`LOG_ERROR` and makes no `host.log` call, while a RUN error goes to both.
That is why `drain()` returns a visibility flag per message rather than a bare
list.  *Enforced by:* the lifecycle differential's `call_update_3_raises` /
`call_update_4_after_error` / `rebind_instance` steps, which assert both sides
emit a host line, that the SAME handler is silent afterwards, and that a rebind
brings it back.
"""

from __future__ import annotations

import ctypes
import sys
import traceback
import types

from . import close, open_host
from ._generated import NOT_EXPOSED

__all__ = ["MAX_COROUTINES", "PROTOCOL", "VM_OWNED", "Instance", "ScriptVM"]

# Bumped whenever the C shim and this module must change together.  The shim
# refuses to create a VM against a different number and names both, because
# `pip install jce-script` and the engine binary ship and upgrade separately —
# a silent skew would surface as an argument-count TypeError inside a
# lifecycle handler, attributed to the user's script.
# *Enforced by:* test_emit_python.py :: test_the_vm_protocol_matches_the_shim,
# which reads JCE_PY_VM_PROTOCOL out of the C file.
PROTOCOL = 1

# The hand-written manifest entries this VM implements itself, because they
# are VM machinery rather than host glue: `log` is the host's log callback and
# the three coroutine entries are the scheduler.  Everything else in
# NOT_EXPOSED stays unimplemented and says why in the manifest's own words —
# see `_Jce.__getattr__`.  Deriving the refusal from NOT_EXPOSED rather than
# from a second list is what makes an eighth hand-written entry name itself
# here without anyone editing this file.
VM_OWNED = frozenset({"log", "start_coroutine", "wait_seconds",
                      "stop_coroutine"})

# Mirrors JCE_SCRIPT_MAX_COROUTINES, engine/src/middleware/script/
# jce_script_internal.h:47.  Copied and not imported: that header is
# engine-private and this module imports nothing from C.  The cap matters
# because Lua's `coro_alloc_slot` returns -1 when full and `start_coroutine`
# then hands the script back 0 — a script that spawns per frame gets the same
# answer from both runtimes instead of one of them growing without bound.
# *Enforced by:* test_emit_python.py :: test_the_coroutine_cap_matches_the_engine.
MAX_COROUTINES = 256


# THE FAILING-CALLBACK RULE's two constants, both of which must agree with the
# engine header rather than merely resemble it.
#
# The notice is byte-for-byte what `JCE_SCRIPT_DISABLED_NOTICE_FMT` in
# engine/include/jce/middleware/script/jce_script.h renders, because the three
# lifecycle differentials compare this line across languages: it is the one
# line about an error that carries no language-specific detail, which is what
# makes it comparable at all.  A Python module cannot include a C header, so
# the agreement is a gate instead.  *Enforced by:* test_emit_python.py ::
# test_the_disabled_notice_matches_the_engine.
DISABLED_NOTICE = ("%s disabled for this script instance after the error "
                   "above; hot-reload the script or respawn the entity to "
                   "re-enable")

# The four REPEATING handlers whose NAME is fixed by jce_script.h and therefore
# means the same thing in every backend.  A dispatcher states its participation
# by passing the name; `call_message` passes None because `msg_name` is the
# CALLER's string (deriving participation from the name would let
# jce.send_message(e, "on_update") disable the real on_update), and `release`
# passes None because on_destroy is dispatched once and the instance is gone
# immediately after.  Both reasons are argued in jce_script.h.
DISABLABLE = frozenset(("on_start", "on_update", "on_collision",
                        "on_anim_event"))


class Instance:
    """One attached script.  Lua's `{entity=owner}` with `__index = module`.

    `__getattr__` is consulted only when normal lookup fails, which is exactly
    when Lua consults `__index`: an attribute written by the script shadows
    the module's, and nothing the script writes leaks into the module and
    thence into the next instance of the same file.
    """

    def __init__(self, ns: dict, entity: int) -> None:
        self.__dict__["_jce_ns"] = ns
        # The disabled set lives ON THE INSTANCE, as Lua's mask lives in the
        # instance's own metatable: it is per instance, it dies with the
        # instance, and `rebind_instance` reaches it where it already reaches
        # `_jce_ns`.  A VM-level dict keyed by instance id would also work
        # here (ids are never recycled) but would not mirror the reference,
        # and the reference's choice is what the differentials pin.
        self.__dict__["_jce_disabled"] = set()
        self.entity = entity

    def __getattr__(self, name: str):
        # Reached through __dict__ rather than `self._jce_ns`: reaching for the
        # namespace through normal attribute lookup inside __getattr__ is how
        # this class recurses to a RecursionError if _jce_ns is ever absent.
        ns = object.__getattribute__(self, "__dict__")["_jce_ns"]
        try:
            return ns[name]
        except KeyError:
            raise AttributeError(name) from None


class _Jce:
    """The `jce` object a script sees: the generated Api plus VM machinery.

    Composition, not a second surface.  The 71 generated entries are reached
    by delegation to the `Api` that `jce_script.open_host()` returned, so this
    class cannot disagree with the manifest about any of them — there is
    nothing here for them to disagree with.
    """

    __slots__ = ("_vm", "_api", "_log")

    def __init__(self, vm: "ScriptVM", api, log) -> None:
        self._vm = vm
        self._api = api
        self._log = log

    # ── the four this VM owns ────────────────────────────────────────────
    def log(self, msg) -> None:
        """jce.log — the host's log callback, as Lua's l_jce_log."""
        self._log(str(msg))

    def start_coroutine(self, fn, *args) -> int:
        """Start a generator function as a coroutine.  Returns its handle.

        Lua spells the body `jce.wait_seconds(n)`; Python spells it `yield n`,
        because a plain function call cannot suspend a Python frame.
        `jce.wait_seconds(n)` is kept as the readable spelling of the value to
        yield — `yield jce.wait_seconds(0.5)` — so the two languages read the
        same even though only one of them could have used a call.
        """
        return self._vm._start_coroutine(fn, args)

    def wait_seconds(self, seconds) -> float:
        return float(seconds)

    def stop_coroutine(self, handle) -> None:
        self._vm._stop_coroutine(handle)

    # ── everything else ──────────────────────────────────────────────────
    def __getattr__(self, name: str):
        if name in NOT_EXPOSED and name not in VM_OWNED:
            # The manifest's own reason, not a paraphrase.  A gap that names
            # itself is a decision; a gap that returns None is a defect that
            # looks like a host with nothing wired.
            raise NotImplementedError(
                f"jce.{name} is not available to Python scripts: "
                f"{NOT_EXPOSED[name]}. It is hand-written in the Lua VM and "
                f"absent from the C ABI, so no binding over jce_script_api "
                f"can reach it.")
        api = object.__getattribute__(self, "_api")
        if api is None:
            raise RuntimeError(
                f"jce.{name}: this VM was created without a JceScriptHost, so "
                f"the scripting surface is not bridged")
        try:
            return getattr(api, name)
        except AttributeError:
            raise AttributeError(
                f"jce has no entry '{name}' — the scripting surface is "
                f"contracts/script-api.json and nothing else") from None


def _brief(exc: BaseException) -> str:
    """`<file>:<line>: TypeError: unsupported operand ...`.

    Deliberately not a full traceback: `host.log` takes a single line in Lua
    and the lifecycle differential compares those lines.  The full traceback
    goes to stderr, once, so a developer still gets it.
    """
    # A SyntaxError carries its OWN location and has no traceback into the
    # script — the deepest frame is this file, which would report vm.py's line
    # number for a typo in somebody's gameplay code.  Lua's compile error names
    # the chunk and the line; so does this.
    if isinstance(exc, SyntaxError) and exc.filename:
        return (f"{exc.filename}:{exc.lineno or 0}: "
                f"{type(exc).__name__}: {exc.msg}")
    tb = exc.__traceback__
    last = None
    while tb is not None:
        last = tb
        tb = tb.tb_next
    where = ""
    if last is not None:
        where = f"{last.tb_frame.f_code.co_filename}:{last.tb_lineno}: "
    return f"{where}{type(exc).__name__}: {exc}"


class ScriptVM:
    """One `JceScript` handle's worth of Python state.

    Created and owned by the C shim; every method below is called from one
    slot of `JceScriptVM`, on the engine's thread, with the GIL held.
    """

    # host_visible flags for drain()
    QUIET = 0     # engine log only, as Lua's compile-error path
    HOST = 1      # host.log AND engine log, as Lua's run and handler paths

    def __init__(self, api, log) -> None:
        self._api = api
        self._log = log
        self._jce = _Jce(self, api, log)
        self._instances: dict[int, Instance] = {}
        self._order: list[int] = []          # instantiation order, for call_named
        self._modules: dict[int, dict] = {}
        self._coros: dict[int, list] = {}    # handle -> [generator, remaining]
        self._next_id = 1
        self._pending: list[tuple[int, str]] = []

    # ── construction from C ──────────────────────────────────────────────
    @classmethod
    def open(cls, host_ptr: int, host_size: int,
             log_fn: int, log_user: int) -> "ScriptVM":
        """Bridge a caller-owned JceScriptHost and return the VM.

        RAISES rather than degrading when a host was supplied and cannot be
        bridged.  A VM whose `jce` is missing would run every handler happily
        and reach the engine with nothing — the silent-failure shape this
        layer exists to refuse.  The C shim turns the exception into a NULL
        from `create_sized`, which `jce_script_vm_create` then reports.
        """
        log = _log_bridge(log_fn, log_user)
        api = open_host(host_ptr, host_size) if host_ptr else None
        return cls(api, log)

    def close(self) -> None:
        """`destroy`'s Python half.

        Closes the `JceScriptApi*` as well as dropping the instances.  The
        handle is a heap copy of the host table made by `jce_script_api_open`;
        letting it go would leak one per VM, and a VM is created and destroyed
        on every scene load.  NOT the interpreter — see the C shim's
        INTERPRETER LIFETIME note for why that is never finalised.
        """
        self._instances.clear()
        self._order.clear()
        self._modules.clear()
        self._coros.clear()
        self._pending.clear()
        if self._api is not None:
            close(self._api)
            self._api = None

    # ── error plumbing ───────────────────────────────────────────────────
    def _note(self, visibility: int, text: str) -> None:
        self._pending.append((visibility, text))

    def _caught(self, what: str, exc: BaseException, visibility: int) -> None:
        traceback.print_exception(type(exc), exc, exc.__traceback__,
                                  file=sys.stderr)
        self._note(visibility, f"{what} error: {_brief(exc)}")

    def drain(self) -> tuple:
        """Everything to log since the last drain, oldest first.

        The C shim calls this after EVERY slot and performs the log writes, so
        that "which lines the host sees" is one decision covering Lua and
        Python rather than two that can drift.
        """
        if not self._pending:
            return ()
        out = tuple(self._pending)
        self._pending = []
        return out

    # ── instantiation ────────────────────────────────────────────────────
    def _exec_module(self, name: str, source: str,
                     compile_label: str, compile_vis: int,
                     run_label: str, run_vis: int) -> dict | None:
        """Compile and run `source` in a fresh namespace, or None.

        THE TWO VISIBILITIES ARE PARAMETERS BECAUSE LUA INVERTS THEM BETWEEN
        ITS TWO CALLERS, and the inversion is observable rather than cosmetic:
        `host.log` is a host callback and therefore part of what the lifecycle
        differential compares, while `LOG_ERROR` is not.

          script_lua_instantiate_source : compile -> LOG_ERROR only  (:662)
                                          run     -> host.log + log  (:634-646)
          script_lua_compile_module     : compile -> host.log + log  (:985-993)
                                          run     -> LOG_ERROR only  (:996-999)

        One shared helper with one hardcoded routing would emit a host line
        where Lua emits none, or the reverse, in two of the four cases — a
        difference the differential would report as a defect in the dispatch
        it was actually testing.
        """
        ns: dict = {
            "__name__": _module_name(name),
            "__file__": name,
            "jce": self._jce,
        }
        try:
            code = compile(source, name, "exec")
        except (SyntaxError, ValueError) as exc:
            self._note(compile_vis,
                       f"{compile_label} ({name}): {_brief(exc)}")
            return None
        try:
            exec(code, ns, ns)          # noqa: S102 — running the script IS the job
        except BaseException as exc:    # noqa: BLE001 — nothing may reach C
            traceback.print_exception(type(exc), exc, exc.__traceback__,
                                      file=sys.stderr)
            self._note(run_vis, f"{run_label} ({name}): {_brief(exc)}")
            return None
        return ns

    def instantiate(self, path: str, source: str, owner: int) -> int:
        """`instantiate`'s Python half.

        The C shim has already read `path` through `host.read_file` — exactly
        where `script_lua_instantiate` reads it, so the "no read_file host
        callback" and "cannot read script" refusals are one implementation for
        both languages.  Only the chunk name differs from `instantiate_source`.
        """
        return self.instantiate_source(path, source, owner)

    def instantiate_source(self, name: str, source: str, owner: int) -> int:
        ns = self._exec_module(name or "=chunk", source,
                               "compile error", self.QUIET,
                               "script run error", self.HOST)
        if ns is None:
            return 0
        inst_id = self._next_id
        self._next_id += 1
        self._instances[inst_id] = Instance(ns, owner)
        self._order.append(inst_id)
        return inst_id

    def release(self, inst_id: int) -> None:
        inst = self._instances.get(inst_id)
        if inst is None:
            return
        self._invoke(inst, "on_destroy", (), None)   # see DISABLABLE
        del self._instances[inst_id]
        try:
            self._order.remove(inst_id)
        except ValueError:
            pass

    def instance_count(self) -> int:
        return len(self._instances)

    # ── dispatch ─────────────────────────────────────────────────────────
    def _invoke(self, inst: Instance, method: str, args: tuple,
                slot) -> None:
        """Call `inst.method(*args)`.

        `slot` is the handler name for THE FAILING-CALLBACK RULE, or None for
        a dispatcher that does not participate.  It is PASSED IN rather than
        taken from `method`, so a message named "on_update" cannot disable the
        real on_update.
        """
        if slot is not None and slot in inst.__dict__["_jce_disabled"]:
            return
        try:
            fn = getattr(inst, method)
        except AttributeError:
            return                       # opting in by declaring it, as Lua
        if not callable(fn):
            return
        try:
            fn(inst, *args)
        except BaseException as exc:     # noqa: BLE001 — nothing may reach C
            self._caught(method, exc, self.HOST)
            if slot is not None:
                inst.__dict__["_jce_disabled"].add(slot)
                self._note(self.HOST, DISABLED_NOTICE % method)

    def _dispatch(self, inst_id: int, method: str, args: tuple,
                  slot) -> None:
        inst = self._instances.get(inst_id)
        if inst is None:
            return
        self._invoke(inst, method, args, slot)

    def call_start(self, inst_id: int) -> None:
        self._dispatch(inst_id, "on_start", (), "on_start")

    def call_update(self, inst_id: int, dt: float) -> None:
        self._dispatch(inst_id, "on_update", (dt,), "on_update")

    def call_collision(self, inst_id: int, other: int) -> None:
        self._dispatch(inst_id, "on_collision", (other,), "on_collision")

    def call_message(self, inst_id: int, msg_name: str, number_arg: float,
                     str_arg) -> None:
        if not msg_name:
            return
        # slot=None: PINNED, see jce_script.h at jce_script_call_message.
        self._dispatch(inst_id, msg_name, (number_arg, str_arg), None)

    def call_anim_event(self, inst_id: int, ev_id: int, name, f0: float,
                        f1: float, i0: int) -> None:
        self._dispatch(inst_id, "on_anim_event",
                       (ev_id, name or None, f0, f1, i0), "on_anim_event")

    # ── named globals (the two slots whose absence is invisible) ─────────
    def _named(self, fn_name: str):
        """The module-level callable `fn_name`, newest instance first.

        Lua looks this up in `_G`, which every chunk shares.  Python has no
        shared global soup and this VM deliberately does not build one: each
        instance's module namespace is its own.  The engine-visible contract —
        "a script declares `def on_volume(entity, value)` and the UI finds it"
        — is identical; the DIVERGENCE is that a handler outlives its instance
        in Lua and does not here.
        *Enforced by:* test_jce_script_vm_python.c ::
        test_a_named_handler_does_not_outlive_its_instance, which states the
        divergence as the expected result rather than leaving it undiscovered.
        """
        if not fn_name:
            return None
        for inst_id in reversed(self._order):
            inst = self._instances.get(inst_id)
            if inst is None:
                continue
            fn = inst.__dict__["_jce_ns"].get(fn_name)
            if callable(fn):
                return fn
        return None

    def _call_named(self, fn_name: str, args: tuple) -> bool:
        fn = self._named(fn_name)
        if fn is None:
            return False
        try:
            fn(*args)
        except BaseException as exc:     # noqa: BLE001
            self._caught(fn_name, exc, self.HOST)
        return True                      # it existed and was invoked, as Lua

    def call_named(self, fn_name: str, entity: int) -> bool:
        return self._call_named(fn_name, (entity,))

    def call_named_num(self, fn_name: str, entity: int, value: float) -> bool:
        return self._call_named(fn_name, (entity, value))

    def call_named_str(self, fn_name: str, entity: int, s) -> bool:
        return self._call_named(fn_name, (entity, s))

    # ── coroutines ───────────────────────────────────────────────────────
    def _start_coroutine(self, fn, args: tuple) -> int:
        """Mirrors `l_jce_start_coroutine`, including WHEN the body first runs.

        Lua `lua_resume`s the new thread up to its first yield INSIDE
        `jce.start_coroutine` — so a coroutine's opening statements have
        already executed by the time the call returns, and a body that never
        yields runs to completion and is never scheduled (handle 0).
        Registering the generator and letting `update_coroutines` take the
        first step would delay every coroutine's first side effect by one tick,
        which is a difference in the recorded host-call ORDER, not a detail.
        """
        try:
            gen = fn(*args)
        except BaseException as exc:     # noqa: BLE001
            self._caught("coroutine", exc, self.HOST)
            return 0
        if not isinstance(gen, types.GeneratorType):
            self._note(self.HOST,
                       "start_coroutine error: the function did not return a "
                       "generator - a JCE coroutine body must contain `yield`")
            return 0
        try:
            waited = next(gen)           # run to the first yield, as Lua does
        except StopIteration:
            return 0                     # finished without ever waiting
        except BaseException as exc:     # noqa: BLE001
            self._caught("coroutine", exc, self.HOST)
            return 0
        if len(self._coros) >= MAX_COROUTINES:
            # Lua's coro_alloc_slot returns -1 and start_coroutine pushes 0.
            return 0
        handle = self._next_id
        self._next_id += 1
        self._coros[handle] = [gen, _wait_of(waited)]
        return handle

    def _stop_coroutine(self, handle) -> None:
        self._coros.pop(int(handle or 0), None)

    def update_coroutines(self, dt: float) -> None:
        # Snapshot, so a coroutine started DURING this tick is not advanced in
        # the same tick — script_lua_update_coroutines snapshots coro_count
        # for exactly this reason.
        for handle in list(self._coros.keys()):
            slot = self._coros.get(handle)
            if slot is None:
                continue
            slot[1] -= dt
            if slot[1] > 0.0:
                continue
            try:
                waited = next(slot[0])
            except StopIteration:
                self._coros.pop(handle, None)
                continue
            except BaseException as exc:  # noqa: BLE001
                self._caught("coroutine", exc, self.HOST)
                self._coros.pop(handle, None)
                continue
            slot[1] = _wait_of(waited)

    # ── hot reload ───────────────────────────────────────────────────────
    def compile_module(self, name: str, source: str) -> int:
        # Visibilities INVERTED relative to instantiate_source, because Lua
        # inverts them here; see _exec_module.
        ns = self._exec_module(name or "=reload", source,
                               "reload compile error", self.HOST,
                               "reload run error", self.QUIET)
        if ns is None:
            return 0
        mod_id = self._next_id
        self._next_id += 1
        self._modules[mod_id] = ns
        return mod_id

    def rebind_instance(self, inst_id: int, mod_id: int) -> None:
        inst = self._instances.get(inst_id)
        ns = self._modules.get(mod_id)
        if inst is None or ns is None:
            return
        inst.__dict__["_jce_ns"] = ns
        # A rebind is the engine saying the code may have changed, so every
        # callback THE FAILING-CALLBACK RULE disabled comes back.  Without
        # this the script you just fixed stays dead until the process
        # restarts, which is worse than the spam the rule exists to stop.
        inst.__dict__["_jce_disabled"].clear()

    def release_module(self, mod_id: int) -> None:
        self._modules.pop(mod_id, None)


def _wait_of(yielded) -> float:
    """A yielded delay, clamped as `script_lua_update_coroutines` clamps it.

    `(float)lua_tonumber(co, -1)` yields 0.0 for a non-number, and negatives
    are clamped to 0.0 at both scheduling sites; `yield` with no value gives
    None here and 0.0 there.
    """
    try:
        wait = float(yielded)
    except (TypeError, ValueError):
        return 0.0
    return 0.0 if wait < 0.0 or wait != wait else wait


def _module_name(name: str) -> str:
    """A `__name__` for a chunk that is a path, a `=chunk`, or anything else.

    Never registered in `sys.modules`: two entities running the same script
    file must not share module state, which is the property `Instance` exists
    to provide and an import cache would quietly undo.
    """
    base = name.replace("\\", "/").rsplit("/", 1)[-1]
    if base.endswith(".py"):
        base = base[:-3]
    return base.lstrip("=@") or "jce_script_chunk"


def _log_bridge(log_fn: int, log_user: int):
    """A callable over the host's `void (*log)(void *user, const char *msg)`.

    Built with ctypes from the raw pointers the shim passes rather than with a
    C wrapper, so the shim never has to know what `jce.log` means.  A NULL
    callback yields a stderr sink — the same shape as `jce_script.c:62`'s
    no-host `LOG_INFO` else-branch, which is the reason `log` is hand-written
    in the manifest in the first place.
    """
    if not log_fn:
        def _fallback(msg: str) -> None:
            sys.stderr.write(f"[script] {msg}\n")
        return _fallback

    proto = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p)
    fn = proto(log_fn)
    user = ctypes.c_void_p(log_user)

    def _emit(msg: str) -> None:
        fn(user, msg.encode("utf-8", "replace"))
    return _emit
