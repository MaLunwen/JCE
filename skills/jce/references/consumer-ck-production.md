# Caged Kingdom consumer boundary

Caged Kingdom is the final game and an SDK consumer under examples/caged_kingdom/. Read that project's AGENTS.md and SOURCE_AND_TERMS.md before changing its content.

Its game rules, setting, balance, scenes, platform icons, package defaults and dedicated generators/tests belong to the project. Reusable tools take explicit project inputs; a named game's defaults do not belong in engine/, editor/ or tools/.

Use public JCE interfaces. Search supported components and SDK APIs before proposing a new engine service. A project's plan is not proof that a runtime format, mission system or feature exists.

Distinguish headless engine requirements, editor requirements and the game's workload-specific performance budget. Record the tested workload instead of copying historical frame-rate numbers.

Imported assets need their own provenance and terms. local_assets/ and unpublished content are ignored; the source license does not grant rights to fonts or art. Keep old project plans and local acceptance notes out of contracts/.
