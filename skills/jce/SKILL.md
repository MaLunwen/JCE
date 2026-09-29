---
name: jce
description: Use when working on JCE engine/editor code, reusable build tooling, installed-SDK consumer projects and their validation. Read the repository contract and route to the relevant procedure.
---

# JCE

The maintained source is skills/jce in this repository. Local skill-directory
links point here; a clone needs no user-installed skill or scratch documents.
Read AGENTS.md, README.md, relevant public API headers and the closest module
charter. User scope and Git preferences take precedence.

Use public SDK interfaces from consumer projects. Search existing APIs and
implementation before adding code. General tooling takes explicit inputs;
project content, balance and dedicated generators stay under examples/<project>.
Keep original third-party source unchanged and outside the tracked tree.

## Task routing

| Task | Reference |
| --- | --- |
| Architecture or file ownership | [architecture-map](references/architecture-map.md) |
| Code style | [code-style](references/code-style.md) |
| Build, source checks and tests | [build-and-gate](references/build-and-gate.md) |
| SDK or consumer deployment | [user-project-sdk](references/user-project-sdk.md) |
| Editor panel, menu or interaction | [editor-panel-workflow](references/editor-panel-workflow.md) |
| Runtime/editor parity | [runtime-parity](references/runtime-parity.md) |
| Script binding changes | [scripting-bindings](references/scripting-bindings.md) |
| Platform and low-end requirements | [crossplatform-lowend](references/crossplatform-lowend.md) |
| Image or performance evidence | [evidence-verification](references/evidence-verification.md) |
| Existing capability investigation | [capability-liveness](references/capability-liveness.md) |
| Project authoring and inspection | [authoring-and-inspection](references/authoring-and-inspection.md) |
| CK consumer boundaries | [consumer-ck-production](references/consumer-ck-production.md) |
| Git and pending workspace delivery | [git-and-worktrees](references/git-and-worktrees.md) |
| Toolchains | [toolchain-reference](references/toolchain-reference.md) |
| Maintained layout and truth sources | [repository-state](references/repository-state.md) |

Run build/test/gate commands through scripts/jce.py. Its implementation is
single-sourced under tools/build; automated checks live under tools/lint and
tools/audit. Public-header changes require SDK refresh and consumer smoke tests.
A visual or timing claim requires an appropriate capture or real clock check.
Report unavailable private checks separately; they are never PASS evidence.

Unpublished AI CLI/Agent/scene/physics workflows and their detailed instructions
are local private extensions. Their source, contracts, prompts and acceptance
records must not enter public commits or skill archives. Ordinary scene/physics
APIs, editor interfaces and SDK mechanisms remain reusable public capabilities.
