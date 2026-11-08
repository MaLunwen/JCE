# Generated output layout

Generated files are local output, never source authority. Unit-test source
remains under tests/ or tools/tests/; test work and new validation output belong
under ignored build/. Reuse a compatible build/cache owner before creating a
new tree. This contract changes prospective defaults, not existing paths.

| Role | Location and lifetime |
| --- | --- |
| Persistent build and dependency caches | Keep existing absolute CMake paths, including `build/desktop/<target-variant>/` and compatible historical cache trees during the default single-executable migration. Keep compatible Conan stores, source pins and shader caches with their current owners. Reuse only when the owner's complete configuration/content identity admits it. |
| Generated test work | build/tests/ and existing owner-selected build/ children contain fixtures, temporary projects, test logs and test executables. Keep each run scoped to its own work directory; do not duplicate a complete source/build/SDK tree for a trial. |
| Bounded validation and raw evidence | New Core-5 output defaults to build/validation/benchmark/. Existing unique run/control directories, suite IDs and immutable content-key fixtures remain authoritative. Keep requested sample/time/size budgets, failed controls and availability distinctions; the directory is not a validity claim or an automatic retention service. |
| Gate diagnostics | The source runner defaults to build/reports/lint-failure.txt and the audit runner to build/reports/architecture-audit-failure.txt. The lint runner retains its existing failure-write/success-clear behavior for its current log only. Audit --log retains its explicit path and write-on-success behavior. |
| Delivery receipts | Keep manifests, hashes, provenance, relocation/audit results and failure diagnostics in the existing owner's build/ or explicitly selected evidence root. Link them to the exact final artifact and configuration instead of copying the payload for every receipt. |
| Final deliveries | Preserve `dist/editor/<named-target-executable>`, `dist/sdk/<target-tuple>`, and `<project>/dist/apps/<app>/<target-tuple>`. Their names and platform-specific payload/container rules remain owned by the existing delivery implementation. |

## Reuse and evidence boundaries

A CMake output executable is mutable: a later link may replace or modify it.
It is not an immutable artifact store. Do not hardlink immutable evidence or a
historical baseline to that mutable executable. Reuse an already verified
immutable artifact by identity when its owner supports that operation; otherwise
retain only the exact independently required artifact and its receipt. This
contract adds no artifact store, alternate build recipe or cache owner.

Default validation does not copy PDBs, the installed SDK, dependency caches or a
whole build/source tree into each run directory. Keep those files in their
original owners; create a separately requested delivery payload only when its
actual acceptance requires it. Count final application size separately from
debug symbols, static SDK libraries, caches and retained evidence.

## Migration and retention

This is prospective. Existing reports/, build/ output trees, explicit --out or
--log roots and referenced history stay where they are. Do not rename a CMake
cache directory, rewrite its absolute paths, recreate deleted history, or move
an existing Conan/source cache to make the tree look uniform. An explicit
caller-selected output root continues to override a default.

Retention and cleanup are explicit user-owned decisions. There is no automatic
history migration, retention sweep, cache deletion or new cleanup command in
this contract. The existing current lint failure-log lifecycle is unchanged.
Before an authorized cleanup, distinguish disposable test work from compatible
caches, exact historical artifacts and referenced raw evidence.

## Default single-executable policy

Editor and application shipments default to one native executable. The
--standalone switch is a compatibility alias for the same embedded policy;
--delivery-profile authoring explicitly selects the optional development bundle.
Editor executables use build/desktop/<target>/<variant>. New native Release
objects and Conan generators use obj/ and conan/ under the target. XP uses
windows-x86-xp or windows-x64-xp, with obj/ separate from the GUI candidate in
dist/. Direct SDK desktop project builds use their project's matching layout.
Configure pins the requested release/dist variant. Qualified historical caches
remain available in place under their existing leases.
Skip-build requires the exact configured variant, embedded shipping flags and
configured output location. Foreign source/profile/CRT/compiler closures refuse.
Windows editor packaging verifies ordinary/delayed PE imports, embedded assets
and notices for both variants. Dist requires GUI PE subsystem 2. Release allows
the console subsystem. Console SDK inspectors retain their own subsystem.
Other-platform dependency/relocation validation remains explicitly unavailable.

## Source-aware cache cleanup

Existing Editor and SDK `--clean` routes share the bounded cache-ownership
inspection in tools/build/standalone.py. Inspect every requested root before
removing any root. A nested `jce_project.json` protects declared project source,
including malformed or unreadable manifests. Noncanonical/aliased roots,
unreadable traversal, directory/unknown Windows reparse points, changed directory
identity and exhausted entry/depth budgets refuse cleanup. Inspection never
follows child symlinks. This is a refusal guard, not an automatic source mover,
retention sweep, general-purpose source detector or concurrent filesystem lock.
Keep source projects outside disposable caches in new layouts; preserve existing
nested projects in place until their owner explicitly approves a migration.
