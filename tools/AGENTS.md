# tools/ — reusable automation

Automated build, asset generation, analysis and validation implementations live
here. They take explicit inputs, derive repository paths from their own location,
produce outputs under ignored build/dist/reports directories and carry no secrets.

- build/: build driver, installed-SDK acceptance and pristine source acquisition.
- lint/ and audit/: source, ABI, dependency, format and policy gates.
- scriptgen/ and shadergen/: generators from maintained contracts.
- provenance/: generic content hashes and externally supplied signatures.
- render/media/asset CLIs: general measurements, cooking and inspection.

A dedicated game's generator, palette, fixture or packaging default belongs to
examples/<project>/. General engine regression fixtures remain with their tests.
Unpublished AI workflows live under private/tools and are optional; reusable
SDK/editor capabilities never depend on their presence. No automated recipe
may depend on scratch docs, a user's home configuration or a copied vendor tree.
