# Scripting and language bindings

The public script-host ABI and contracts/script-api.json are the maintained authorities. Languages adapt the same operations; they do not define separate gameplay schemas.

```bash
python tools/scriptgen/gen_script_bindings.py
python tools/scriptgen/gen_script_c_abi.py
python tools/audit/check_script_language_catalog.py
```

The generators check their own emitted output by default. Regenerate only after an intentional contract change and review the diff; do not hand-edit generated artifacts. Preserve existing ABI field order and append supported members under the ABI policy.

A catalog entry proves registration, not execution. Build the managed artifacts and native adapters, then run behavioral traces for the relevant languages in both editor Play and the consumer runtime. Compare values, host calls, error handling, lifecycle and tick order.

C#/Java/Python also need their actual runtime/deployment files. A staged DLL alone may omit runtimeconfig, class files or modules. Record external runtime requirements and validate discovery after relocation.

An unsupported language must report failure clearly instead of silently running another backend. Preserve credential/environment boundaries for the generic LLM bridge; private AI policy is not binding implementation.
