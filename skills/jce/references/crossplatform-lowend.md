# Platform and low-end requirements

Consult AGENTS.md for compatibility targets and CMakePresets.json plus conan/profiles/ for actual build configurations. A target declaration is not evidence that a toolchain or device was tested.

The LOW-END design baseline is a graphical desktop, one CPU core, 512 MiB RAM
and 2008/2010-class integrated graphics without a discrete GPU. It covers usable
lightweight editor and rendered runtime paths as well as core/headless services.
PS2 is a resource-budget analogy, not a device support or equivalence claim.
Complex scenes and advanced features have explicit workload/tier budgets; they
must not silently redefine the baseline as headless-only. Record the actual
high-end development machine, and report physical low-end validation separately
from forced low settings on a modern host.

Desktop/mobile/console and x86/x64/ARM/ARM64 are compatibility goals. Default
driver languages are Java, Python, Lua, C, C++, C# and JavaScript through one
native C ABI. Compiler, licensed console toolchains, device and package evidence
remain separate from catalog entries. A missing backend or device is a gap.

Put platform-specific code behind OS adapters and public capability queries. Keep deterministic fallback behavior when hardware acceleration, a codec, a scripting runtime or an advanced rendering feature is unavailable.

Validate only the platforms the request covers. Record compiler/runtime floors, hardware/backend, memory and elapsed-time evidence. Report unavailable cross builds and devices separately; a native cached build is not a portable-source proof.

Never alter upstream code to work around a platform build. Use an approved upstream release, first-party adapter outside the upstream tree, or generated configuration supported by the dependency.
