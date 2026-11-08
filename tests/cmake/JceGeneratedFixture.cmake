# Configure-generated text fixtures only; unchanged content keeps its timestamp.
# Callers retain output paths, dependencies, mutation checks and directory policy.
include_guard(GLOBAL)

function(jce_test_write_if_changed OUTPUT CONTENT)
    if(EXISTS "${OUTPUT}")
        file(READ "${OUTPUT}" _jce_generated_previous)
        if("${_jce_generated_previous}" STREQUAL "${CONTENT}")
            return()
        endif()
    endif()
    file(WRITE "${OUTPUT}" "${CONTENT}")
endfunction()
