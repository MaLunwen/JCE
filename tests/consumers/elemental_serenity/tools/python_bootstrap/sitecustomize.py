"""ES editor CPython startup: honor the consumer's optional runtime path.

Put this directory on PYTHONPATH only when launching this project. CPython
imports sitecustomize before jce_script.vm imports ctypes. Keep the directory
handle alive so dependent extension DLLs remain discoverable on Windows.
No runtime files are copied, patched or searched by a hard-coded machine path.
"""
import os

_es_runtime_dependency = os.environ.get("ES_RUNTIME_PRELOAD", "")
_es_runtime_directory = None
if os.name == "nt" and _es_runtime_dependency:
    _es_runtime_directory = os.add_dll_directory(
        os.path.dirname(os.path.abspath(_es_runtime_dependency)))
