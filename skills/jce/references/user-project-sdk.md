# SDK consumers and distribution

Consumers include public JCE headers and link the matching installed SDK. They do not include engine/src/ or direct vendor headers. Search public APIs and component configuration before introducing an engine feature.

```bash
python scripts/jce.py sdk --arch x64 --variant dist --no-debug
python scripts/jce.py smoke --arch x64 --variant dist
python scripts/jce.py package editor --arch x64 --variant dist
```

For a project, use supported cook/app/package/accept subcommands with an explicit project directory. Keep its CMake, manifest, icons and dedicated tooling with the project. Read each subcommand's --help before choosing options.

Public-header changes require refreshing the requested SDKs and rebuilding their consumers. Installed headers are build output: never repair them manually. Match the library, headers, scripting adapters, graphics tier and variant from one configuration.

Dist selects a Release build without Tracy or patented codec paths. Release and dist are distinct products; an old executable cannot be relabeled as dist. Inspect the packaged binary and runtime dependencies, not just the selected preset name.

Stage required managed runtime configuration, Java classes and Python modules alongside their adapters. State any required external .NET/JVM/Python installation. Include project and dependency license information and avoid redistributing unverified local fonts or assets.

Acceptance includes a relocated package, clean user state, no source-tree asset fallback, representative language/runtime checks and inspected captures. Record the executable hash, source commit, variant, platform and test limits. Packaging completes only after the delivered contents have been verified.
