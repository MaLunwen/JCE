# Authoring a consumer project

Distinguish using JCE from changing JCE. A consumer project lives under examples/ or an external SDK project; its scene, components, balance and scripts remain there. Do not edit engine/editor internals to deliver a consumer feature without user authorization.

Discover supported components and fields before generating scene data.

```bash
python tools/jce_scene_kit.py schema
python tools/jce_scene_kit.py schema --type Camera
python tools/jce_scene_kit.py check <scene.json>
```

Prefer component configuration and supported script-host operations over handwritten engine behavior. Check the actual field schema, camera conventions, project settings and script-language catalog rather than inventing properties.

For an orthographic camera, inspect the API's ortho-size convention and verify framing at the intended viewport aspect ratio. Match editor Game View and shipped renderer backends before comparing images.

Prefer configured default/system font fallback. A display-family name is not proof that a font is loaded or licensed. Keep imported models, textures, audio and fonts with project provenance and terms; asset reuse and creation are subject to the user's authorized workflow.

Separate development-time AI assistance from AI behavior executed during the game. The reusable model transport is public; unpublished CLI/Agent/scene/physics policy and private prompts must remain outside public source and skill packages.

Validate scene data, build the consumer against the matching SDK, run it, capture output and inspect the result. A package directory merely existing is not application acceptance.
