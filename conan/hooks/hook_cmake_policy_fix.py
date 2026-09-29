"""CMake 4 compatibility through generated configuration, never source edits."""

def pre_generate(conanfile):
    key = "tools.cmake.cmaketoolchain:extra_variables"
    variables = dict(conanfile.conf.get(key, default={}, check_type=dict))
    variables.setdefault("CMAKE_POLICY_VERSION_MINIMUM", "3.5")
    conanfile.conf.define(key, variables)
