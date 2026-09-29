# Build and validation

Use the supported CLI and target options; scripts/jce.py forwards to tools/build/jce.py.

```bash
python scripts/jce.py --help
python scripts/jce.py targets
python scripts/jce.py lint
python scripts/jce.py test --arch x64 --jobs 8
```

The lint command runs source gates and the architecture audit. Missing private suites are unavailable, not PASS. Run focused checks during iteration and the required complete suites before delivery.

Public-header changes require an SDK refresh and consumer smoke validation. Binding changes require both generators in check mode. A committed ABI check inspects HEAD; a pending working-tree baseline does not make an older HEAD pass.

Fetch original dependencies only from immutable pins in contracts/vendor-sources.json. Verify existing originals; never repair an upstream checkout or Conan package. Configuration-only Conan hooks respect CONAN_HOME. A cached binary build does not prove pristine source or cross-platform compatibility.
