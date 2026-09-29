"""reload_py.py -- the module compile_module/rebind_instance swaps in.

Deliberately a DIFFERENT on_update from lifecycle_py.py, and no on_destroy:
rebinding must replace the whole namespace, so "v2" proves the swap happened
and the absence of on_destroy proves it was a REPLACEMENT rather than an
overlay.
"""


def on_update(self, dt):
    jce.log("v2")                                         # noqa: F821
