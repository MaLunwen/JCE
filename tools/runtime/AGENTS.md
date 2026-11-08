# tools/runtime - compilerless application stock consumer

This Windows x64 Release/dist/embedded SDK consumer owns the Lua/JavaScript
stock executable and its version-2 appended secure project PAK reader. It uses only public
SDK interfaces and the shared default runtime callbacks; platform self-path
resolution remains the engine host-path owner's responsibility. No archive is
extracted, external compiler invoked, or alternate game loop introduced.

The 64-byte little-endian envelope binds exact host/outer-PAK sizes and FNV-1a64
checksums. Those checks detect corruption, not hostile replacement or signing.
Publication separately records SHA-256 identities. The existing public mmap owner supplies a borrowed image (with its own platform
fallback); no consumer-owned platform IO or second whole-image heap copy is used.
The borrowed image survives
all default runtime, filesystem and Bundle users, including failed init cleanup.

Admission uses actual public Bundle inspection, existing package hash validation
and a typed scene table. One contract-1.1 self-contained Bundle is supported;
external Bundle dependencies, native class refs and optional VM scripts fail.
The packaged-only default-main macro requires an actual typed-scene admission
hook; both boot scenes and QA overrides must remain in that validated table.
It does not change ordinary default-main developer behavior.

Python controls live in tools/tests/test_stock_app.py. They cannot certify the
native reader, refreshed SDK link, graphical execution or relocation. Root owns
serial native/SDK/consumer validation. Preserve failed evidence and explicit
unsupported architectures; no C/C++ toolchain payload is part of this slice.


The default-main stock policy uses the existing ISOLATED active VFS and closes
only project/content discovery. Explicit absolute host operations, normal user
preferences, saves and diagnostics remain available; this is not a sandbox.
The finite default-main settings/pipeline/particle/locale/mixer/resolver branches
share their existing loaders. An archive budget is a parser admission ceiling,
not a 512 MiB device memory qualification.

`stock-app build-runtime` is a separate first-party component producer. It
holds the shared SDK/fixed-consumer-cache leases, reuses public PAK/COFF tools
and supplies the authenticated, boot-free prebuilt OBJ through the SDK helper.
It never relaxes ordinary app DIST scene admission. Verified SDK/source identity
enters every actual compile command. Matching asset/key outputs retain bytes and
mtime only after their bounded fingerprint and a fresh deep inspection agree.

`jce_stock_runtime_verify` builds the host and complete-owner CPU fixtures/ordinary
mutants, then runs only their eight exact CTest names. The controls are EXCLUDE_FROM_ALL for a normal stock
producer build. They do not launch the raw stock host, which has no appended
Bundle and must refuse startup. Do not configure another recipe around it.
The producer's own LICENSE defaults to ../../LICENSE; it is a build input, not
a runtime content fallback. Required Lua/JS links and registration use the SDK's
jce_script_enable/shim before the first shared default-runtime scene load.


Publication requires the explicit successful producer terminal receipt and its
SHA-256, exact EXE and SDK product, stable current source/compile identity and
retained native CPU log. A generic GUI executable cannot declare itself stock.
Sealing requires explicit --work-dir and --receipt outside the final output
directory; that directory must be empty before sealing and contains only the EXE
after success. Evidence is retained separately and is not a runtime dependency.

CPU fixtures use the SDK's existing public JCE::Core target for conventional
main(), preserving the same archive and required dependencies. The real stock
application retains JCE::JCE and its complete application whole-archive policy.
No SDK target property, ABI, private implementation or renderer stub is replaced.

Version 2 requires one authenticated/encrypted outer PAK containing exactly
stock/project.jbundle. The original typed Bundle bytes are not repacked. The
public archive owner performs keyed lookup, authentication and decryption;
its one-index/no-dictionary budget is checked before opening. The decrypted
Bundle has a separate 512 MiB parser ceiling, not a low-memory qualification.
Default runtime exit and Bundle borrowers close before decrypted bytes and the
mapped executable are released. Raw version-1 payloads are not Dist inputs.

The producer receipt and schema-2 runtime manifest bind the actual sealing-key
SHA-256 and its owned producer-cache path. Seal requires an explicit external
--pak-key-file matching both. Publication does not copy or export a raw key;
only the final EXE is a runtime delivery. A fully zero-configuration builder is
still pending. The embedded client key raises extraction cost, not secrecy.
Secure packing and fresh authenticated inspection use the existing public SDK
packer. Pack-mode source verification decrypts and memcmps the one staged input,
which must retain the exact original Bundle SHA and typed dependency graph.
