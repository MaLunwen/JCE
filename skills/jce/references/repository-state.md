# Repository truth sources

Do not copy transient counts or old branch assumptions into a result.
- Layout: contracts/source-layout.json and actual tracked paths.
- Version/dependencies: CMakeLists.txt, conanfile.py and vendor-sources.json.
- Public API/ABI: engine/include/jce and contracts/abi-snapshot.txt.
- Binding contracts: contracts/script-api.json and the script generators.
- Source checks: python scripts/jce.py lint.
- Tests: python scripts/jce.py test; state the variant and feature configuration.
- SDK consumption: python scripts/jce.py sdk and smoke.

skills/jce is the authoritative skill source. Local runtime links do not make a
second copy. docs/.docs/private and a user's home are not public clone inputs.
