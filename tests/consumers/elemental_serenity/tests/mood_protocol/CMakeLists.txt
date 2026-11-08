cmake_minimum_required(VERSION 3.20)
project(EsMoodProtocol C CXX)
find_package(JCE REQUIRED)
add_executable(EsMoodProtocol ../mood_protocol.c)
set_target_properties(EsMoodProtocol PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
target_link_libraries(EsMoodProtocol PRIVATE JCE::Core)
# No Root engine target, Conan discovery, GPU or copied Lua implementation.

# Actual full Lua producers with controlled public host destinations; no GPU.
enable_testing()
set(_es_mood_assets "${CMAKE_CURRENT_LIST_DIR}/../../resources/assets")
add_test(NAME es_mood_protocol COMMAND EsMoodProtocol "${_es_mood_assets}")
foreach(_es_mood_control IN ITEMS old-missing-ack-wire old-camera-read missing-camera-guard old-stale-next missing-next-selector)
    string(REPLACE "-" "_" _es_mood_suffix "${_es_mood_control}")
    add_test(NAME es_mood_protocol_${_es_mood_suffix}
        COMMAND EsMoodProtocol "${_es_mood_assets}" --${_es_mood_control})
    set_tests_properties(es_mood_protocol_${_es_mood_suffix} PROPERTIES
        WILL_FAIL TRUE LABELS "domain:consumer;consumer;kind:contract")
endforeach()
set_tests_properties(es_mood_protocol PROPERTIES LABELS "domain:consumer;consumer;kind:integration")
unset(_es_mood_assets)
unset(_es_mood_control)
unset(_es_mood_suffix)
