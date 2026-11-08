# Standalone native adapter: no engine, SDL or allocator link requirement.
jce_add_unit_test(test_jce_native_atomic_word SOURCES test_jce_native_atomic_word.c
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
jce_add_unit_test(test_jce_memory_atomic_accounting
    SOURCES test_jce_memory_atomic_accounting.c
    LINK jce_core mimalloc-static
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/core")
jce_add_unit_test(test_jce_memory_atomic_accounting_release
    SOURCES test_jce_memory_atomic_accounting.c
    LINK jce_core mimalloc-static
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/core")
target_compile_definitions(test_jce_memory_atomic_accounting_release PRIVATE NDEBUG=1)
if(MSVC)
    target_compile_options(test_jce_memory_atomic_accounting PRIVATE /UNDEBUG)
else()
    target_compile_options(test_jce_memory_atomic_accounting PRIVATE -UNDEBUG)
endif()

# Deterministic old plain-RMW control only runs the single-threaded protocol
# case. It must fail ordinary assertions, never execute a C data race.
set(_memory_atomic_owner "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_memory.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_memory_atomic_owner}"
    "${CMAKE_SOURCE_DIR}/engine/src/os/platform/jce_native_atomic_word_internal.h")
file(READ "${_memory_atomic_owner}" _memory_atomic_actual)
string(REGEX MATCHALL "jce_native_atomic_word_add\\(" _memory_atomic_wires "${_memory_atomic_actual}")
list(LENGTH _memory_atomic_wires _memory_atomic_count)
if(NOT _memory_atomic_count EQUAL 12)
    message(FATAL_ERROR "Memory plain-RMW control requires all twelve actual shared-counter RMW wires")
endif()
string(REPLACE "jce_native_atomic_word_add(" "plain_word_add("
    _memory_atomic_old "${_memory_atomic_actual}")
set(_memory_atomic_dir "${CMAKE_CURRENT_BINARY_DIR}/memory-atomic")
file(MAKE_DIRECTORY "${_memory_atomic_dir}")
set(_memory_atomic_copy "${_memory_atomic_dir}/old-plain.inc.h")
unset(_memory_atomic_previous)
if(EXISTS "${_memory_atomic_copy}")
    file(READ "${_memory_atomic_copy}" _memory_atomic_previous)
endif()
if(NOT _memory_atomic_previous STREQUAL _memory_atomic_old)
    file(WRITE "${_memory_atomic_copy}" "${_memory_atomic_old}")
endif()
jce_add_unit_test(test_jce_memory_atomic_accounting_old_plain KIND contract
    SOURCES test_jce_memory_atomic_accounting.c
    LINK jce_core mimalloc-static
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src" "${CMAKE_SOURCE_DIR}/engine/src/os/core")
target_compile_definitions(test_jce_memory_atomic_accounting_old_plain PRIVATE
    JCE_MEMORY_PROTOCOL_CONTROL=1 JCE_MEMORY_ACCOUNTING_OWNER="${_memory_atomic_copy}")
if(MSVC)
    target_compile_options(test_jce_memory_atomic_accounting_old_plain PRIVATE /UNDEBUG)
else()
    target_compile_options(test_jce_memory_atomic_accounting_old_plain PRIVATE -UNDEBUG)
endif()
set_tests_properties(test_jce_memory_atomic_accounting_old_plain PROPERTIES WILL_FAIL TRUE)
foreach(_memory_atomic_variable IN ITEMS owner actual wires count old dir copy previous)
    unset(_memory_atomic_${_memory_atomic_variable})
endforeach()
unset(_memory_atomic_variable)
