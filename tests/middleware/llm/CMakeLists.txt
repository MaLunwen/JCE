cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  L4 llm-layer unit tests.                                               #
#                                                                         #
#  The transport is a real child process, and the test executable IS the  #
#  provider (run with --fake-provider).  A python one-liner would have    #
#  been shorter and would have made this suite fail on a machine with no  #
#  python on PATH -- where "the transport is broken" and "python is not   #
#  installed" look identical from inside a red test.                      #
# ----------------------------------------------------------------------- #

jce_add_unit_test(test_jce_llm
    SOURCES test_jce_llm.c
    LINK    jce_core jce_llm)

# Runtime negative control: same real transport/fixture, only the accepted
# request trace wire removed. Successful compilation is necessary evidence;
# expected failure must come from the unchanged runtime trace assertions.
set(_llm_trace_owner "${CMAKE_SOURCE_DIR}/engine/src/middleware/llm/jce_llm.c")
file(READ "${_llm_trace_owner}" _llm_trace_original)
# No semicolon in the regex match: a CMake list separator is not a second wire.
string(REGEX MATCHALL "llm_trace_request_begin[(]submit_scope[.]id[)]"
    _llm_trace_wires "${_llm_trace_original}")
list(LENGTH _llm_trace_wires _llm_trace_wire_count)
if(NOT _llm_trace_wire_count EQUAL 1)
    message(FATAL_ERROR "LLM old-wire control requires exactly one real accepted-request trace wire")
endif()
string(REPLACE "llm_trace_request_begin(submit_scope.id);"
    "(void)submit_scope.id; (void)llm_trace_request_begin;"
    _llm_trace_mutated "${_llm_trace_original}")
if(_llm_trace_mutated STREQUAL _llm_trace_original)
    message(FATAL_ERROR "LLM old-wire control did not remove its exact production call")
endif()
set(_llm_trace_mutant "${CMAKE_CURRENT_BINARY_DIR}/jce_llm_trace_old_wire.c")
jce_test_write_if_changed("${_llm_trace_mutant}" "${_llm_trace_mutated}")
jce_add_unit_test(test_jce_llm_trace_old_wire
    KIND contract
    SOURCES test_jce_llm.c "${_llm_trace_mutant}"
    LINK jce_core
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
set_tests_properties(test_jce_llm_trace_old_wire PROPERTIES WILL_FAIL TRUE)
unset(_llm_trace_owner)
unset(_llm_trace_original)
unset(_llm_trace_wires)
unset(_llm_trace_wire_count)
unset(_llm_trace_mutated)
unset(_llm_trace_mutant)
