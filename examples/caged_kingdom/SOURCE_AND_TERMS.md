# Source, terms and project contract

Caged Kingdom is the final game authored in JCE. No separate upstream game repository was supplied. SETTING_BIBLE.md defines the setting; CK_Asset_Manifest_v1.0.md inventories assets. Planning documents are not licenses for imported art. Existing private assets remain ignored and need their own provenance before redistribution.

Source: [MaLunwen/JCE](https://github.com/MaLunwen/JCE), examples/caged_kingdom.
Repository source terms: [MPL-2.0](../../LICENSE). Imported assets/dependencies
retain their original terms; do not infer asset rights from the code license.
Keep dedicated tools and policy in this directory. Consume only public SDK
interfaces. Original third-party code is not vendored here.

## Fonts

Default UI families are `JCE` (the locally supplied engine font) and `Caveat`.
Font assets are ignored; a source checkout requires locally supplied fonts.
Caveat identifies The Caveat Project Authors and SIL Open Font License in its
metadata; retain the upstream license with any distribution.

`FOT-MatisseElegantoPro-EB.otf` is a local Fontworks asset. Its metadata states
“All Rights Reserved”; no redistribution grant was supplied here. It is no
longer loaded or selected by the default UI. The original and stale cooked copy are retained under the ignored
`local_assets/fonts/` directory, outside all cooked asset roots. Source-code licensing does not grant
font rights. The `JCE.ttf` name table alone is not proof of asset provenance.
