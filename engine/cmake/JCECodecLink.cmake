# Pin the canonical codec implementation without modifying upstream sources.
# bimg's embedded dav1d exports the same names. Archive order alone is not
# enough when another dependency first introduces codec references mid-scan.
function(jce_force_codec_archive TARGET ARCHIVE)
    if(MSVC)
        target_link_options(${TARGET} INTERFACE "/WHOLEARCHIVE:${ARCHIVE}")
    elseif(APPLE)
        target_link_options(${TARGET} INTERFACE "LINKER:-force_load,${ARCHIVE}")
    else()
        target_link_options(${TARGET} INTERFACE
            "LINKER:--whole-archive,${ARCHIVE},--no-whole-archive")
    endif()
endfunction()
