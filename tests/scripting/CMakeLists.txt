cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  tests/scripting — the language-binding layer's suites.                 #
#                                                                         #
#  Discovery, for the same reason scripting/CMakeLists.txt discovers:      #
#  scripting/python, scripting/java and scripting/cpp are being added in   #
#  parallel, and each of their suites should arrive as one new directory   #
#  under here, touching no file that another agent owns.                   #
# ----------------------------------------------------------------------- #

file(GLOB _jce_scripting_test_dirs CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt")
foreach(_cml IN LISTS _jce_scripting_test_dirs)
    get_filename_component(_dir "${_cml}" DIRECTORY)
    get_filename_component(_name "${_dir}" NAME)
    add_subdirectory("${_name}")
endforeach()
unset(_jce_scripting_test_dirs)
unset(_cml)
unset(_dir)
unset(_name)
