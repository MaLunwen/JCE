# Caged Kingdom

CK is the production game consumer at `examples/caged_kingdom`. Its implementation
source is currently absent by owner choice; the checked-in CMake/project metadata
does not constitute a runnable game. The target identity remains `CagedKingdom`
and `caged_kingdom.exe` on Windows.

The owner keeps the bilingual v1.1 design freeze and production/reference archives
under local `way/demo_complete/`. These originals remain untracked for this
release. Their README and decision log define design authority; frozen design
validation must not be presented as successful gameplay or engine acceptance.
The former Lightning Lab scene and scripts are not available in this checkout.

When implementation sources are supplied, the supported consumer entry point is:

```sh
python scripts/jce.py app examples/caged_kingdom --arch x64 --variant release
```

The application must consume the installed JCE SDK and ship as a single portable
executable with embedded resources. Transferable scene/application content uses
one `.jbundle`; loose development assets are not delivery dependencies. Reuse
build caches through the shared tooling and validate editor, executable and
reopened Bundle behavior before publication.
