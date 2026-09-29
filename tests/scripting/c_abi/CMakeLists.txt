cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  The script-facing shared library's suite.                              #
#                                                                         #
#  These tests link NOTHING — not the engine, not even the library's       #
#  import lib.  They LoadLibrary / dlopen the built object by absolute     #
#  path and resolve every entry point by name, which is exactly what a     #
#  ctypes or JNA binding does.  Linking the import library would prove the #
#  symbols exist to the LINKER, which is not the question the DLL's        #
#  consumers ask.                                                          #
# ----------------------------------------------------------------------- #

if(NOT TARGET jce_script_api)
    # Not silent: the symbol-table test is where "the surface is all the
    # shared object exports" is checked, and its absence belongs in the
    # configure log rather than being inferred from a test count.
    message(STATUS
        "JCE tests: script API C-ABI suite SKIPPED — jce_script_api is not "
        "built (-DJCE_BUILD_SCRIPT_API_SHARED=OFF)")
    return()
endif()

jce_add_unit_test(test_jce_script_api_abi
    SOURCES  test_jce_script_api_abi.c
    INCLUDES "${CMAKE_SOURCE_DIR}/scripting/c_abi/include"
             "${CMAKE_SOURCE_DIR}/scripting/c_abi/src")

# The object under test, and a build-order edge to it.  The test reads this
# file's export directory off disk, so it must exist and be current first.
target_compile_definitions(test_jce_script_api_abi PRIVATE
    JCE_SCRIPT_API_DLL_PATH="$<TARGET_FILE:jce_script_api>")
add_dependencies(test_jce_script_api_abi jce_script_api)

if(UNIX)
    target_link_libraries(test_jce_script_api_abi PRIVATE ${CMAKE_DL_LIBS})
endif()
