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
