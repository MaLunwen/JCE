---
name: jce
description: Use when working on JCE engine/editor code, reusable build tooling, installed-SDK consumer projects and their validation.
metadata:
  language: en-US
---

# JCE

This is the default en-US edition. [Simplified Chinese edition](../jce-zh-cn/SKILL.md).
The maintained source is skills/jce in this repository; local runtime links resolve here.

Read AGENTS.md, README.md, the relevant public API and the closest module charter.
User scope, language preference and authorized Git actions take precedence.
Use public SDK interfaces in consumer projects and reuse existing component/runtime mechanisms.
General tools take explicit inputs; project content and dedicated tooling belong under examples/.
Original third-party source and packages must remain unchanged.

## Task references

Read only the reference relevant to the task.

| Task | Reference |
| --- | --- |
| Architecture and ownership | [Architecture and ownership](references/architecture-map.md) |
| Code and text conventions | [Code and text conventions](references/code-style.md) |
| Build and validation | [Build and validation](references/build-and-gate.md) |
| SDK consumers and distribution | [SDK consumers and distribution](references/user-project-sdk.md) |
| Editor integration | [Editor integration](references/editor-panel-workflow.md) |
| Editor/runtime parity | [Editor/runtime parity](references/runtime-parity.md) |
| Scripting and language bindings | [Scripting and language bindings](references/scripting-bindings.md) |
| Platform and low-end requirements | [Platform and low-end requirements](references/crossplatform-lowend.md) |
| Visual and timing evidence | [Visual and timing evidence](references/evidence-verification.md) |
| Capability liveness | [Capability liveness](references/capability-liveness.md) |
| Authoring a consumer project | [Authoring a consumer project](references/authoring-and-inspection.md) |
| Caged Kingdom consumer boundary | [Caged Kingdom consumer boundary](references/consumer-ck-production.md) |
| Git workflow | [Git workflow](references/git-and-worktrees.md) |
| Toolchain discovery | [Toolchain discovery](references/toolchain-reference.md) |
| Maintained repository state | [Maintained repository state](references/repository-state.md) |

## Verification and publication

Use scripts/jce.py for builds, tests and gates. Source checks and the architecture audit are separate requirements.
Public-header changes require SDK refresh and consumer validation; binding changes require both generators.
Pixel and timing claims need captures and actual clock evidence. Missing/private checks are unavailable, never PASS.

Unpublished AI CLI/Agent/scene/physics policy, private prompts and delivery notes must not enter public source or skill archives.
Public model transport, SDK/editor interfaces and ordinary scene/physics capabilities remain reusable public mechanisms.
Keep both language editions' reference inventory and command examples synchronized.
