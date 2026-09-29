# Pins and hashes live in contracts/vendor-sources.json. Python uses system TLS.
if(CMAKE_HOST_WIN32)
    set(_jce_cache_base "$ENV{LOCALAPPDATA}")
elseif(CMAKE_HOST_APPLE)
    set(_jce_cache_base "$ENV{HOME}/Library/Caches")
elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
    set(_jce_cache_base "$ENV{XDG_CACHE_HOME}")
else()
    set(_jce_cache_base "$ENV{HOME}/.cache")
endif()
file(TO_CMAKE_PATH "${_jce_cache_base}/JCE/upstream-sources" _jce_source_cache)
# Keep upstream sample projects outside IDE discovery in the JCE workspace.
# Migrate only our former default and preserve external cache overrides.
# Existing source trees are never cleaned or repaired.
if(NOT DEFINED JCE_VENDOR_CACHE OR
   JCE_VENDOR_CACHE STREQUAL "${CMAKE_SOURCE_DIR}/build/dependencies")
    set(JCE_VENDOR_CACHE "${_jce_source_cache}" CACHE PATH
        "Cache of verified, unmodified upstream source downloads" FORCE)
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

function(jce_vendor_source NAME OUT_DIR)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/build/fetch_vendor_sources.py" "${NAME}"
            --cache "${JCE_VENDOR_CACHE}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _directory
        ERROR_VARIABLE _error OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "JCE: cannot acquire pristine ${NAME}: ${_error}")
    endif()
    set(${OUT_DIR} "${_directory}" PARENT_SCOPE)
    if(NOT TARGET "jce_vendor_verify_${NAME}")
        add_custom_target("jce_vendor_verify_${NAME}"
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/tools/build/fetch_vendor_sources.py" "${NAME}"
                --cache "${JCE_VENDOR_CACHE}" --verify-only
            COMMENT "Verifying pristine upstream ${NAME}" VERBATIM)
    endif()
endfunction()

jce_vendor_source(minimp4 JCE_MINIMP4_SOURCE_DIR)

jce_vendor_source(stb_image JCE_STB_IMAGE_SOURCE_DIR)
