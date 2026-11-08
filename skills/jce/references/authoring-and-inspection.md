# Authoring a consumer project

AI CLI is a development-time application consumer. It uses only public API/SDK,
documentation, repository Skill, examples, editor operations and supported build
tools. Never inspect or patch private engine/editor implementation as a shortcut
to an application feature. Report a reusable API/editor gap and its reproducer;
engine maintenance, if authorized, is a separately scoped task with its own
validation. Consumer scene, component values, balance and driver-language code
remain in the project. This generic boundary reveals no private authoring policy.

Discover supported components and fields before generating scene data.

```bash
python tools/jce_scene_kit.py schema
python tools/jce_scene_kit.py schema --type Camera
python tools/jce_scene_kit.py check <scene.json>
```

Prefer component configuration and supported script-host operations over handwritten engine behavior. Check the actual field schema, camera conventions, project settings and script-language catalog rather than inventing properties.

A derived loose scene must retain a valid ancestor project and its settings/resource
context. Stage it through the existing project owner without changing asset lookup
roots; record intended semantic deltas. Loading identical scene bytes from a report
directory can change defaults, scripts or assets, and is a different workload.

For an orthographic camera, inspect the API's ortho-size convention and verify framing at the intended viewport aspect ratio. Match editor Game View and shipped renderer backends before comparing images.

Prefer configured default/system font fallback. A display-family name is not proof that a font is loaded or licensed. Keep imported models, textures, audio and fonts with project provenance and terms; asset reuse and creation are subject to the user's authorized workflow.

Separate development-time AI assistance from AI behavior executed during the game. The reusable model transport is public; unpublished CLI/Agent/scene/physics policy and private prompts must remain outside public source and skill packages.

Repeat the application lifecycle checks after changes: validate supported scene
fields and components; simulate in the editor; build and run the corresponding
platform executable through the matching SDK; package a bundle and reopen it in
the editor; compare effective settings, input, script behavior, assets, output
and teardown. Relocate the native application and disable loose/source fallback.
Record each host, language/backend, artifact identity and actual result. A
successful model reply, compilation or existing package directory is not
acceptance. Failures feed the next public-API iteration; missing hosts remain
explicitly unavailable.

When reproducing Editor UI cost, record actual panel exposure and host/viewport.
Keep presentation separate from service work; [Editor workflow evidence](evidence-verification.md#ordinary-editor-workflow-and-effective-gpu-load)
covers separately authorized maintenance and CPU fixtures.

For portable/native consumer variants, use one serialized request/apply/ack
consumer for shared UI behavior. Label the provider actually used; an offline
preset, a validated native plan and an AI-generated result have different
scope. A void message dispatch is not application acknowledgement. Preserve
normal and explicitly muted QA content as separate acceptance inputs, and use
the public packer BOM to prove the selected script sidecars and assets survived
staging. Keep optional font bytes and pinned matching license records without
forcing that font into common defaults or the default dependency closure.
