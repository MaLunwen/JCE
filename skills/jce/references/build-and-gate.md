# Build and validation

Use the supported CLI and target options; scripts/jce.py forwards to tools/build/jce.py.

```bash
python scripts/jce.py --help
python scripts/jce.py targets
python scripts/jce.py lint
python scripts/jce.py test --arch x64 --jobs 8
python scripts/jce.py test --configure --variant debug --arch x64 --jobs 2
```

The lint command runs source gates and the architecture audit. Missing private suites are unavailable, not PASS. Run focused checks during iteration and the required complete suites before delivery.

Public-header changes require an SDK refresh and consumer smoke validation. Binding changes require both generators in check mode. A committed ABI check inspects HEAD; a pending working-tree baseline does not make an older HEAD pass.

Fetch original dependencies only from immutable pins in contracts/vendor-sources.json. Verify existing originals; never repair an upstream checkout or Conan package. Configuration-only Conan hooks respect CONAN_HOME. A cached binary build does not prove pristine source or cross-platform compatibility.

Fresh test trees use `test --configure --variant debug|release`. The CLI defaults
to checking host packages; CI explicitly sets JCE_SYSTEM_PACKAGE_MODE=install
and JCE_SYSTEM_PACKAGE_SUDO=1. Native tool names and Debug CRT policy follow
the target platform.
Native Linux dependency metadata requires a working pkg-config executable.
Check actual execution, especially when WSL inherits Windows tool directories.
CMAKE_BUILD_PARALLEL_LEVEL controls both compiler builds and Conan's host/build
dependency contexts; the shared default is eight. CTest has a separate
--jobs/CTEST_PARALLEL_LEVEL budget. Preserve these limits on resource-constrained
hosts and exclude concurrent builds from formal performance measurements.

Supported build entry points live in scripts/. Compiler and dependency commands
are constructed by the shared tools/build implementation; do not hand-write a
second build recipe. Reuse a compatible target/variant/toolchain cache. Isolate
only real ABI/configuration differences and avoid full source/build copies per
trial. Check disk headroom and distinguish shipped application size from PDBs,
static SDK libraries, dependency caches and retained evidence. Generated test
artifacts go under ignored build/; source tests stay under tests/. Keep original
dependencies and referenced raw evidence unchanged. GPU validation must not
steal focus; use hidden/nonfocused windows only when rendering is necessary.
