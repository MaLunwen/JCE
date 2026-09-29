# tools/jce_agent_bridge — the engine side of the agent layer, as an SDK consumer.
#
# Built OUT-OF-TREE against an installed SDK, exactly like tests/sdk_smoke:
#
#     cmake -S tools/jce_agent_bridge -B <bdir> -DJCE_DIR=<sdk>/lib/cmake/JCE
#
# and driven by `python scripts/jce.py agent-bridge`.
#
# WHY OUT-OF-TREE, when an in-tree target would have been one line.  Two
# reasons, and the second is the one that matters:
#
#   * it compiles against the PACKAGED headers and links the PACKAGED library,
#     so the capabilities it exposes are shown to survive SDK installation.
#     This tree has shipped a public header that compiled in-tree, passed 359
#     unit tests, and broke every SDK consumer on the first line of api.h.
#   * it builds into its OWN build directory, so it never contends for the
#     shared one.  Several sessions work in this checkout at once and two
#     concurrent builds into one directory corrupt each other rather than
#     queueing.
#
# jce_add_pak IS REQUIRED, and this comment first said the opposite.  The
# reasoning was that the bridge reads scenes and recipes from paths it is given
# and ships no assets, so a pak would only add a host-tool dependency.  The
# link disagreed:
#
#   jce_engine_core.lib(jce_engine.c.obj): unresolved external symbol
#       assets_pak_data / assets_pak_data_size
#
# The engine references the embedded pak unconditionally, and those symbols are
# what jce_add_pak generates.  So the bridge carries one placeholder asset --
# nothing reads it; it exists so the link has a pak to embed.
cmake_minimum_required(VERSION 3.20)
# CXX is required even though the source is pure C99: the JCE fat lib contains
# C++ objects (bgfx, Bullet, ...) and the final link must use the C++ driver.
project(JceAgentBridge C CXX)

find_package(JCE REQUIRED)

add_executable(JceAgentBridge src/main.c)
set_target_properties(JceAgentBridge PROPERTIES
	OUTPUT_NAME          jce_agent_bridge
	C_STANDARD           99
	C_STANDARD_REQUIRED  YES)
target_link_libraries(JceAgentBridge PRIVATE JCE::JCE)

jce_add_pak(JceAgentBridge RESOURCE_DIRS assets)
