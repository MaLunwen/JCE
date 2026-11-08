# scripting/cmake — installed scripting SDK

JCEScriptingInstall.cmake installs the aggregate, roster metadata and canonical
consumer targets. JCEScripting.cmake.in is their shared find-package and runtime
staging owner; keep aliases and backend payloads paired with actual installs.

Runtime staging consumes the producer's compiled roster authority, while the
catalog retains unavailable optional backends. Missing/malformed authority
retains a warning and legacy discovery rather than becoming an empty set.
Actual STATIC_LIBRARY/INTERFACE_LIBRARY targets satisfy staging without a copy;
a static SDK with only those satisfied declarations and no external files
reports STATUS. Missing compiled rosters/targets/files or unsupported nonstatic
target types retain WARN, including when another item was successfully staged.
Neither discovery nor a no-op proves consumer linking, runtime execution,
embedding or relocation; use the explicit installed-SDK consumer gates.

JCEScriptEnable owns the validated per-target language/factory declarations and
the registration shim directory. Publish these private target properties only
after successful validation and generation. Deferred SDK admission reuses the
original shim location and adds its source once, including when a target was
first enabled in a child directory. Required native metadata never substitutes
for explicit NATIVE_FACTORIES; missing or mismatched declarations remain fatal.
