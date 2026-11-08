# Complete real registry + host JSON owner. No native window or renderer.
function(_jce_editor_register_hotkey_persistence_controls)
    set(_owner "${CMAKE_SOURCE_DIR}/editor/src/core/jce_hotkeys.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_owner}")
    file(READ "${_owner}" _source)
    set(_load_begin "extern \"C\" bool jce_hotkeys_load(void)")
    string(FIND "${_source}" "${_load_begin}" _load_first)
    string(FIND "${_source}" "${_load_begin}" _load_last REVERSE)
    if(_load_first LESS 0 OR NOT _load_first EQUAL _load_last)
        message(FATAL_ERROR "Hotkey persistence requires one actual loader boundary")
    endif()
    string(SUBSTRING "${_source}" 0 ${_load_first} _prefix)
    set(_old_load [=[extern "C" bool jce_hotkeys_load(void)
{
    char path[640];
    hotkeys_path(path, sizeof(path));
    size_t len = 0;
    char *buf = (char *)ed_read_file(path, &len);
    if (!buf) return false;
    if (len > (1 << 20)) { ED_FREE(buf); return false; }

    JceJson *root = jce_json_parse(buf, len);
    ED_FREE(buf);
    if (!root) return false;

    JceJson *arr = jce_json_get(root, "hotkeys");
    int n = jce_json_array_size(arr);
    for (int e = 0; e < n; ++e) {
        JceJson *item = jce_json_array_at(arr, e);
        const char *id = jce_json_string_value(jce_json_get(item, "id"), nullptr);
        if (!id || !jce_json_has(item, "key")) continue;
        for (int i = 0; i < JCE_HK_COUNT; ++i) {
            if (std::strcmp(s_table[i].id_string, id) == 0) {
                s_table[i].cur.key  = jce_json_get_int(item, "key", 0);
                s_table[i].cur.mods = (uint8_t)(jce_json_get_int(item, "mods", 0) & 0xFF);
                break;
            }
        }
    }
    jce_json_free(root);
    return true;
}
]=])
    set(_set_guard "    if (!chord_valid(chord)) return;\n")
    string(FIND "${_source}" "${_set_guard}" _set_first)
    string(FIND "${_source}" "${_set_guard}" _set_last REVERSE)
    if(_set_first LESS 0 OR NOT _set_first EQUAL _set_last)
        message(FATAL_ERROR "Hotkey setter control requires one actual admission guard")
    endif()
    foreach(_variant IN ITEMS actual old_load old_set)
        set(_body "${_source}")
        set(_target test_jce_editor_hotkey_persistence)
        set(_kind integration)
        if(_variant STREQUAL "old_load")
            set(_body "${_prefix}${_old_load}")
        elseif(_variant STREQUAL "old_set")
            string(REPLACE "${_set_guard}" "" _body "${_source}")
        endif()
        if(NOT _variant STREQUAL "actual")
            string(APPEND _target "_${_variant}")
            set(_kind contract)
            if(_body STREQUAL _source)
                message(FATAL_ERROR "Hotkey ${_variant} control did not change the owner")
            endif()
        endif()
        set(_output "${CMAKE_CURRENT_BINARY_DIR}/hotkey-persistence/${_variant}.cpp")
        jce_test_write_if_changed("${_output}" "${_body}")
        file(READ "${_output}" _written)
        if(NOT _written STREQUAL _body)
            message(FATAL_ERROR "Hotkey compiler input differs from selected owner")
        endif()
        jce_add_cxx_test(${_target} KIND ${_kind}
            SOURCES test_jce_editor_hotkey_persistence.cpp
                "${CMAKE_SOURCE_DIR}/editor/src/core/jce_editor_config.cpp"
                "${_output}"
            LINK jce_core jce_platform jce_tools_imgui
            INCLUDES "${CMAKE_SOURCE_DIR}/editor/src/core")
        target_compile_features(${_target} PRIVATE cxx_std_20)
        if(_kind STREQUAL "contract")
            set_tests_properties(${_target} PROPERTIES WILL_FAIL TRUE)
        endif()
    endforeach()
endfunction()
_jce_editor_register_hotkey_persistence_controls()
