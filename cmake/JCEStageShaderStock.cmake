# Install-time only. Caller supplies explicit current-build provenance.
foreach(_required JCE_SHADER_STOCK_PYTHON JCE_SHADER_STOCK_SCRIPT JCE_SHADER_STOCK_BOM JCE_SHADER_STOCK_ROOT JCE_SHADER_STOCK_OUT JCE_SHADER_STOCK_PROFILES)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "Missing shader stock input: ${_required}")
    endif()
endforeach()
execute_process(COMMAND "${JCE_SHADER_STOCK_PYTHON}" "${JCE_SHADER_STOCK_SCRIPT}"
    --bom "${JCE_SHADER_STOCK_BOM}" --root "${JCE_SHADER_STOCK_ROOT}"
    --out "${JCE_SHADER_STOCK_OUT}" --profiles ${JCE_SHADER_STOCK_PROFILES}
    RESULT_VARIABLE _stock_result)
if(NOT _stock_result EQUAL 0)
    message(FATAL_ERROR "Verified shader stock staging failed: ${_stock_result}")
endif()

# stage_stock returns only after verify_stock has checked every address and blob.
# Register logical installation paths, not DESTDIR-prefixed physical paths.
file(READ "${JCE_SHADER_STOCK_OUT}/manifest.json" _stock_manifest)
string(JSON _stock_file_count LENGTH "${_stock_manifest}" files)
if(DEFINED JCE_SHADER_STOCK_MANIFEST_ROOT)
    set(_stock_manifest_root "${JCE_SHADER_STOCK_MANIFEST_ROOT}")
else()
    set(_stock_manifest_root "${JCE_SHADER_STOCK_OUT}")
endif()
if("${_stock_manifest_root}" STREQUAL "")
    message(FATAL_ERROR "Empty shader stock manifest root")
endif()
set(_stock_install_files "${_stock_manifest_root}/manifest.json")
math(EXPR _stock_last "${_stock_file_count} - 1")
foreach(_stock_index RANGE 0 ${_stock_last})
    string(JSON _stock_address GET "${_stock_manifest}" files ${_stock_index} address)
    # CMake lists cannot faithfully register paths containing list separators.
    if("${_stock_address}" MATCHES "[;\r\n]")
        message(FATAL_ERROR "Shader stock address cannot enter install manifest")
    endif()
    list(APPEND _stock_install_files "${_stock_manifest_root}/${_stock_address}")
endforeach()
list(APPEND CMAKE_INSTALL_MANIFEST_FILES ${_stock_install_files})
