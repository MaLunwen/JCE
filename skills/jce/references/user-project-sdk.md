# SDK consumers and distribution

Consumers include public JCE headers and link the matching installed SDK. They do not include engine/src/ or direct vendor headers. Search public APIs and component configuration before introducing an engine feature.

```bash
python scripts/jce.py sdk --arch x64 --variant dist --no-debug
python scripts/jce.py smoke --arch x64 --variant dist
python scripts/jce.py package editor --arch x64 --variant dist
python scripts/jce.py package editor --arch x64 --variant dist --standalone
```

For a project, use supported cook/app/package/accept subcommands with an explicit project directory. Keep its CMake, manifest, icons and dedicated tooling with the project. Read each subcommand's --help before choosing options.

Public-header changes require refreshing the requested SDKs and rebuilding their consumers. Installed headers are build output: never repair them manually. Match the library, headers, scripting adapters, graphics tier and variant from one configuration.

Dist selects a Release build without Tracy or patented codec paths. Release and dist are distinct products; an old executable cannot be relabeled as dist. Inspect the packaged binary and runtime dependencies, not just the selected preset name.

The shipping default is a compact, self-contained native application: embedded Lua and JavaScript plus native C/C++ application code. Python, Java and C# remain optional authoring/SDK integrations with explicitly documented external runtimes. Do not add these VMs or SDK payloads to the shipping executable merely to claim seven embedded languages. Required project scripts must resolve on the target host; reject a missing driver or module instead of silently discarding behavior.

Acceptance includes only the delivered application in a relocated empty directory, clean user state, no source-tree asset fallback and inspected representative captures. Record executable hash, source identity, variant, platform and test limits. Verify the actual embedded assets/notices and ordinary/delayed imports. This Windows evidence does not establish other platforms.

For the current Windows native single-EXE recipe, `--standalone` selects the static-CRT graph and embedded assets with Lua/JavaScript/native ABI support. Native C/C++ ABI support alone does not prove that a project's script classes are statically registered. A `.jcec` or `.jcecpp` path names a class; putting a descriptor or marker file into a content bundle does not create that class registry. Preserve optional native-module developer tests separately, and explicitly validate the actual static registration or use a behavior-preserving supported portable script variant. Shipping must not depend on a sibling project DLL, external VM, SDK installation or extraction wrapper.

Runtime models are canonical glTF/GLB. Convert and validate supported editor source-format imports and all dependent assets before cooking or packaging. Project policy and tools stay with the consumer; do not insert game-specific rules into the generic engine or editor.

Single-executable delivery is the engine invariant for editor and consumers.
A Web shipment is one HTML containing its wasm/bootstrap/assets. Cross-target
probes cannot consume host JNI headers. Static Windows SDK production reuses
the editor dependency closure; the SDK CRT is part of its installed ABI.
Check ordinary/delayed imports and relocate only the application file into an
empty directory. Runtime-created settings are not shipment dependencies.
Do not certify a native-script admission profile using only source declarations;
retain actual compiled controller behavior and artifact identity.

## Build and delivery directory ownership

Keep intermediate objects, generated code, link outputs and validation evidence under build/ (or reports/ for shared reports). SDK installation lives under dist/sdk; it is not an application shipment. Stage final editor applications under dist/editor and consumer applications under an explicit dist/apps/<project>/<platform-architecture-variant>/ directory. The application directory contains only its required platform executable/container; native single-file and Web single-HTML shipments contain only that file. Keep hashes, inventories, logs and captures outside it. Packaging is a separate step from compilation; never point a final output at a build cache or recursively copy a link directory. Honor supported --out/--artifact-dir options after reading their help. Document any explicit staging step and validate the exact staged bytes by empty-directory relocation. Runtime-created caches/settings are recorded separately. Concurrent maintainers update the affected en/zh skill references alongside implementation changes and coordinate ownership before editing shared sections.


Bundle delivery uses one `.jbundle` file containing its selected scene, scripts and asset closure. It is a content package, not an engine/SDK distribution or independently executable application. The app builder forwards the manifest `bundles` list to the installed SDK embedding helpers; relative paths resolve against the project and list order is preserved. A stock project template can therefore embed a Bundle into the final single executable. Verify both editor reopen and an EXE-only relocated directory; pack/build success alone does not certify gameplay or every target platform.

Windows DIST applications default to the GUI subsystem and must not create a console window. Apply target policy before prebuilt PAK/bundle early returns; embedding must not bypass it. Validate the produced PE subsystem (2=GUI), not just CMake declarations. CLI development tools retain their explicit console policy.
