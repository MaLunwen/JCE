# Actual cluster cache admission, generation and lifetime regions through CPU
# ports. These controls create no graphics device and make no timing claim.
function(_jce_fcluster_cache_region source begin end output)
    string(FIND "${source}" "${begin}" first)
    string(FIND "${source}" "${begin}" first_last REVERSE)
    string(FIND "${source}" "${end}" last)
    string(FIND "${source}" "${end}" last_last REVERSE)
    if(first LESS 0 OR NOT first EQUAL first_last OR
       NOT last EQUAL last_last OR NOT last GREATER first)
        message(FATAL_ERROR "Foliage cache controls require unique complete ${output} boundaries")
    endif()
    math(EXPR length "${last} - ${first}")
    string(SUBSTRING "${source}" ${first} ${length} region)
    set(${output} "${region}" PARENT_SCOPE)
endfunction()

function(_jce_fcluster_cache_old_limit source output)
    string(REGEX MATCH
        "\\(int\\)\\(sizeof sr->fcluster_cache /[ \n]*sizeof sr->fcluster_cache\\[0\\]\\)"
        bound "${source}")
    if(bound STREQUAL "")
        message(FATAL_ERROR "Foliage lifetime control requires the actual array bound")
    endif()
    string(FIND "${source}" "${bound}" first)
    string(FIND "${source}" "${bound}" last REVERSE)
    if(NOT first EQUAL last)
        message(FATAL_ERROR "Foliage lifetime control requires one bound")
    endif()
    string(REPLACE "${bound}" "48" changed "${source}")
    set(${output} "${changed}" PARENT_SCOPE)
endfunction()

function(_jce_fcluster_cache_controls)
    set(owner_dir "${CMAKE_SOURCE_DIR}/engine/src/middleware/scene")
    foreach(name IN ITEMS jce_sr_internal.h jce_sr_environment.c jce_scene_renderer.c)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${owner_dir}/${name}")
        file(READ "${owner_dir}/${name}" ${name})
    endforeach()
    _jce_fcluster_cache_region("${jce_sr_internal.h}"
        "#define SR_FCLUSTER_CACHE_CAPACITY " "/* Ratio of cloud-base wind"
        capacity)
    _jce_fcluster_cache_region("${jce_sr_internal.h}"
        "    struct {\n        JceEntity  entity;\n        bool       used;\n        uint32_t   param_hash;"
        "    JceMesh              *fcluster_quad;" record)
    _jce_fcluster_cache_region("${jce_sr_environment.c}"
        "static int sr_fcluster_find_slot(" "/* Undo/redo and scene swaps recycle entity ids."
        admission)
    _jce_fcluster_cache_region("${jce_sr_environment.c}"
        "static float sr_foliage_rand(" "static const float sr_fcluster_card_uv"
        random)
    _jce_fcluster_cache_region("${jce_sr_environment.c}"
        "/* Return a cached leaf-card slot or -1; shadow views may build it before color. */"
        "static const char *sr_fcluster_submit_reason(" generation)
    _jce_fcluster_cache_region("${jce_sr_environment.c}"
        "    for (int i = 0; i < (int)(sizeof sr->fcluster_cache /"
        "    /* Meshlet indirects are entity-keyed too;" reset)
    _jce_fcluster_cache_region("${jce_scene_renderer.c}"
        "    for (int i = 0; i < (int)(sizeof sr->fcluster_cache /"
        "    /* Streaming LOD cross-fade uniform" destruction)
    set(header "${capacity}\ntypedef struct {\n${record}\n    bool fcluster_expanded_cards;\n} JceSceneRenderer;\n")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fcluster-cache")
    foreach(variant IN ITEMS current old_capacity partial_reset partial_destroy)
        set(selected_header "${header}")
        set(selected_reset "${reset}")
        set(selected_destruction "${destruction}")
        if(variant STREQUAL "old_capacity")
            if(NOT selected_header MATCHES "#define SR_FCLUSTER_CACHE_CAPACITY 64")
                message(FATAL_ERROR "Old foliage capacity control requires the current 64-slot budget")
            endif()
            string(REPLACE "#define SR_FCLUSTER_CACHE_CAPACITY 64"
                "#define SR_FCLUSTER_CACHE_CAPACITY 48" selected_header "${selected_header}")
        elseif(variant STREQUAL "partial_reset")
            _jce_fcluster_cache_old_limit("${reset}" selected_reset)
        elseif(variant STREQUAL "partial_destroy")
            _jce_fcluster_cache_old_limit("${destruction}" selected_destruction)
        endif()
        set(content "${selected_header}\n${random}\n${admission}\n${generation}\n")
        string(APPEND content "static int reset_clusters(JceSceneRenderer *sr)\n{\n    int dropped = 0;\n${selected_reset}\n    return dropped;\n}\n")
        string(APPEND content "static void destroy_clusters(JceSceneRenderer *sr)\n{\n${selected_destruction}\n}\n")
        set(path "${CMAKE_CURRENT_BINARY_DIR}/fcluster-cache/${variant}.inc.h")
        jce_test_write_if_changed("${path}" "${content}")
        set(target "test_jce_fcluster_cache_${variant}")
        set(kind unit)
        if(NOT variant STREQUAL "current")
            set(kind contract)
        endif()
        jce_add_unit_test(${target} KIND ${kind} SOURCES test_jce_fcluster_cache.c)
        target_compile_definitions(${target} PRIVATE JCE_TEST_FCLUSTER_OWNER="${path}")
        if(NOT MSVC)
            target_link_libraries(${target} PRIVATE m)
        endif()
        if(kind STREQUAL "contract")
            set_tests_properties(${target} PROPERTIES WILL_FAIL TRUE)
        endif()
    endforeach()
endfunction()
_jce_fcluster_cache_controls()
