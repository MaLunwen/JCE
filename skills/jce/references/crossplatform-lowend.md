# Platform and low-end requirements

Consult AGENTS.md for compatibility targets and CMakePresets.json plus conan/profiles/ for actual build configurations. A target declaration is not evidence that a toolchain or device was tested.

The one-core, 512 MiB, integrated-graphics baseline concerns core/headless capability and lightweight workloads. The editor and advanced rendered games have workload-dependent requirements.

Put platform-specific code behind OS adapters and public capability queries. Keep deterministic fallback behavior when hardware acceleration, a codec, a scripting runtime or an advanced rendering feature is unavailable.

Validate only the platforms the request covers. Record compiler/runtime floors, hardware/backend, memory and elapsed-time evidence. Report unavailable cross builds and devices separately; a native cached build is not a portable-source proof.

Never alter upstream code to work around a platform build. Use an approved upstream release, first-party adapter outside the upstream tree, or generated configuration supported by the dependency.
