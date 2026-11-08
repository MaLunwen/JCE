# Tower Defense contextual content Bundle

[tower-defense.jbundle](tower-defense.jbundle) is one content archive requiring
JCE 0.13.4 or newer. It contains no compiled native application module, SDK or DLL kit.

Editor Play requires the C native class `TdController` and authoring factory
`td_module` supplied by the [SDK consumer](../../tests/consumers/td_lab/README.md).
Opening the content in a stock editor without that context is not portable
playable-game delivery. A separately built single EXE or self-contained HTML
application is a different product and validation scope.

[Bundle provenance](BUNDLE_PROVENANCE.md) records the exact content identity and
two contextual reopen/Play/Stop controls. [Source and terms](SOURCE_AND_TERMS.md)
retain the upstream pin, MIT notice, converted-asset and raster-font boundaries.
Raw verification outputs and developer build trees remain outside this catalog.
