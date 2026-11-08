# Actual shared Core codec bodies, preserving old output as a byte oracle.
function(jce_trace_json_span_controls)
    set(owner "${CMAKE_SOURCE_DIR}/engine/src/os/core/jce_trace.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${owner}")
    file(READ "${owner}" source)
    string(FIND "${source}" "static bool trace_json_reserve(" begin)
    string(FIND "${source}" "uint32_t jce_trace_capture_generation(void)" end)
    string(FIND "${source}" "bool jce_trace_chrome_encode_batch_into(" batch_begin)
    string(FIND "${source}" "bool jce_trace_chrome_encode_batch(" batch_end)
    if(begin LESS 0 OR end LESS_EQUAL begin OR batch_begin LESS end OR batch_end LESS_EQUAL batch_begin)
        message(FATAL_ERROR "Shared trace codec function boundaries changed")
    endif()
    math(EXPR length "${end} - ${begin}")
    string(SUBSTRING "${source}" ${begin} ${length} helpers)
    math(EXPR length "${batch_end} - ${batch_begin}")
    string(SUBSTRING "${source}" ${batch_begin} ${length} batch)
    set(codec "${helpers}\n${batch}")
    string(FIND "${codec}" "static bool trace_json_append_escaped(" escape_begin)
    string(FIND "${codec}" "static const char *trace_category_name(" escape_end)
    if(escape_begin LESS 0 OR escape_end LESS_EQUAL escape_begin)
        message(FATAL_ERROR "Shared trace escape owner boundaries changed")
    endif()
    math(EXPR length "${escape_end} - ${escape_begin}")
    string(SUBSTRING "${codec}" ${escape_begin} ${length} current_escape)
    # Original complete escaping owner. Only this function and the original
    # literal formatting wire differ in the durable ordinary negative/oracle.
    set(old_escape [=[static bool trace_json_append_escaped(TraceJsonBuffer *buffer,
                                      const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    if (!trace_json_append(buffer, "\""))
        return false;
    while (*cursor) {
        char temp[8];

        if (*cursor == '"' || *cursor == '\\') {
            temp[0] = '\\';
            temp[1] = (char)*cursor;
            temp[2] = '\0';
        } else if (*cursor < 0x20u) {
            SDL_snprintf(temp, sizeof(temp), "\\u%04x", (unsigned)*cursor);
        } else {
            temp[0] = (char)*cursor;
            temp[1] = '\0';
        }
        if (!trace_json_append(buffer, temp))
            return false;
        cursor++;
    }
    return trace_json_append(buffer, "\"");
}

]=])
    string(REPLACE "${current_escape}" "${old_escape}" old_codec "${codec}")
    # Restore the original append body too: an old/new timing comparison must
    # not charge the old path for an extra new explicit-length helper call.
    string(FIND "${old_codec}" "static bool trace_json_append(" append_begin)
    string(FIND "${old_codec}" "static bool trace_json_append_format(" append_end)
    if(append_end LESS 0)
        string(FIND "${old_codec}" "static bool trace_json_append_escaped(" append_end)
    endif()
    if(append_begin LESS 0 OR append_end LESS_EQUAL append_begin)
        message(FATAL_ERROR "Shared trace append owner boundaries changed")
    endif()
    math(EXPR length "${append_end} - ${append_begin}")
    string(SUBSTRING "${old_codec}" ${append_begin} ${length} current_append)
    set(old_append [=[static bool trace_json_append(TraceJsonBuffer *buffer, const char *text)
{
    size_t length = SDL_strlen(text);

    if (!trace_json_reserve(buffer, length + 1u))
        return false;
    SDL_memcpy(buffer->data + buffer->size, text, length);
    buffer->size += length;
    buffer->data[buffer->size] = '\0';
    return true;
}

]=])
    string(REPLACE "${current_append}" "${old_append}" old_codec "${old_codec}")
    set(new_prefix [=[    if (!trace_json_append(json, "{\"name\":") ||
        !trace_json_append_escaped(json, event_name))]=])
    set(old_prefix [=[    length = SDL_snprintf(prefix, sizeof(prefix), "{\"name\":");
    if (length < 0 || !trace_json_append(json, prefix) ||
        !trace_json_append_escaped(json, event_name))]=])
    string(FIND "${old_codec}" "${new_prefix}" new_at)
    string(FIND "${old_codec}" "${old_prefix}" old_at)
    if(new_at LESS 0 AND old_at LESS 0)
        message(FATAL_ERROR "Shared trace literal-prefix owner changed")
    endif()
    if(NOT new_at LESS 0)
        string(REPLACE "${new_prefix}" "${old_prefix}" old_codec "${old_codec}")
    endif()
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/trace-json-span")
    set(current_path "${dir}/current.inc")
    set(old_path "${dir}/old_character_appends.inc")
    jce_test_write_if_changed("${current_path}" "${codec}")
    jce_test_write_if_changed("${old_path}" "${old_codec}")
    set(headers "#include <SDL3/SDL.h>\n#include <string.h>\n#include \"os/core/jce_memory.h\"\n#include \"os/core/jce_trace_capture_internal.h\"\n")
    foreach(mode IN ITEMS current old)
        set(selected "${current_path}")
        if(mode STREQUAL "old")
            set(selected "${old_path}")
        endif()
        set(ref_path "${dir}/${mode}_reference.c")
        jce_test_write_if_changed("${ref_path}"
            "${headers}#define jce_trace_chrome_encode_batch_into ${mode}_reference_encode_into\n#include \"${selected}\"\n")
    endforeach()
    foreach(mode IN ITEMS current old_character_appends)
        set(name test_jce_trace_json_span)
        set(kind unit)
        set(selected "${current_path}")
        if(NOT mode STREQUAL "current")
            string(APPEND name _${mode})
            set(kind contract)
            set(selected "${old_path}")
        endif()
        jce_add_unit_test(${name} KIND ${kind}
            SOURCES test_jce_trace_json_span.c "${dir}/current_reference.c" "${dir}/old_reference.c"
            LINK jce_core sdl::sdl
            INCLUDES "${CMAKE_SOURCE_DIR}/engine/src")
        target_compile_definitions(${name} PRIVATE JCE_TEST_TRACE_JSON_CODEC_OWNER="${selected}")
        if(NOT mode STREQUAL "current")
            set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
        endif()
    endforeach()
endfunction()
jce_trace_json_span_controls()
