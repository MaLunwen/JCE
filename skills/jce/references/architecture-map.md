# Architecture and ownership

Read AGENTS.md and the closest module charter before changing a module.
The maintained map is contracts/module-memory-index.md.

- engine/include/jce/ owns the public C99 API; engine/src/ owns implementation.
- editor/ and examples/ consume public SDK interfaces. Consumer code does not include engine internals or vendor headers.
- scripting/ implements language adapters over the same C ABI.
- tools/ owns reusable automation with explicit inputs; scripts/ owns manual and forwarding entrypoints.
- A game's content, balance, packaging defaults and dedicated tools belong under examples/.
- contracts/ contains stable authorities. Local plans and delivery records belong under ignored docs/.
- Optional unpublished AI workflows belong under private/; public core builds must not require them.

Follow lower-to-higher layering from OS primitives through renderer, middleware, runtime and application to consumers. Reuse each service's authoritative owner in contracts/dependency-ownership.yml. A new wrapper is not a reason to duplicate allocation, scheduling, physics or rendering implementations.
