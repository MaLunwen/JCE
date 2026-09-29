# smoke_py.py -- the script tests/sdk_smoke_scripting runs THROUGH THE PAK.
#
# It is deliberately dull.  What is under test is not this file: it is that an
# out-of-tree project, building only against an installed SDK, can get a
# CPython into the engine's process, find the shipped jce_script package,
# resolve ".py" to the "python" language, read THESE BYTES back out of the
# cooked+packed archive its own build produced, and have both lifecycle slots
# reach the host.
#
# A Python gameplay script is a module whose top-level functions take `self`,
# the instance -- the same shape a Lua script's methods have.  `jce` is
# injected into the module namespace by jce_script/vm.py.
#
# THE MARKERS ARE LITERAL AND THE COUNTING HAPPENS IN C.  A script that
# formatted its own counter would move the oracle into the thing being
# tested, and "the log line said 3" would then be a claim about str() rather
# than about how many times the engine called on_update.

JCE_SDK_SMOKE_SCRIPT_MARKER = 1


def on_start(self):
    jce.log("SMOKE_PY_START")


def on_update(self, dt):
    jce.log("SMOKE_PY_UPDATE")
