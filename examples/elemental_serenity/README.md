# Elemental Serenity single-file Bundle

Open `elemental_serenity.jbundle` in the JCE editor. This is the normal audio/content
portable Lua variant for Windows. The single Bundle carries its scene, scripts,
assets and project RenderPipeline settings without loose sidecars. Font bytes
are excluded; the host supplies its default font.

Authoring source and dedicated regression tools: [`tests/consumers/elemental_serenity`](../../tests/consumers/elemental_serenity).
Source and terms: [SOURCE_AND_TERMS.md](SOURCE_AND_TERMS.md).
Repository source license: [LICENSE](LICENSE). Asset-specific notices retain their original terms.

SHA-256: `e46cc9a1b9b5e5a4741b7eb8682fa25befe7ac7d2e462dd027e0e5d77e743395` (6874857 bytes).

The package matches the maintained scene and restores its blue sky. Its requested
antialiasing policy is FXAA on/TAA off; this is a content correction, not a
same-quality performance gain. All63 Archive entries passed integrity checks.
The exact Bundle was opened in Windows Editor and completed two Play/Stop cycles.
A separately built public SDK consumer was exercised on Windows D3D12/Intel for
seasons, weather and camera movement; normal exit and CPU cleanup were verified
in a separate session from the longer interactive run.

These observations do not certify every feature, pixel parity, GPU completion,
audio output correctness, physical low-end performance or all-platform support.
See [BUNDLE_PROVENANCE.json](BUNDLE_PROVENANCE.json) and
[BUNDLE_VARIANT.md](BUNDLE_VARIANT.md) for exact inputs and validation scope.
