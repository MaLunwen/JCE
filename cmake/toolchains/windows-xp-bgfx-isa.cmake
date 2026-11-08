# Configure the pinned XP provider without changing its sources or installed files.
function(jce_xp_bgfx_use_sse2)
  if(NOT TARGET bx OR NOT TARGET bimg OR NOT TARGET bgfx OR
     NOT CMAKE_SYSTEM_NAME STREQUAL "Windows" OR
     NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "The XP bgfx ISA policy requires the reviewed Windows Clang BX target")
  endif()

  set(vendor_isa "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-msse4.2>")
  foreach(property IN ITEMS COMPILE_OPTIONS INTERFACE_COMPILE_OPTIONS)
    get_target_property(options bx "${property}")
    list(FIND options "${vendor_isa}" position)
    if(position EQUAL -1)
      message(FATAL_ERROR "The pinned BX ${property} ISA option has changed")
    endif()
    list(REMOVE_AT options "${position}")
    if(vendor_isa IN_LIST options)
      message(FATAL_ERROR "The pinned BX ${property} has duplicate ISA options")
    endif()
    set_property(TARGET bx PROPERTY "${property}" "${options}")
  endforeach()

  set(header "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/windows-xp-bgfx-simd.hpp")
  if(NOT EXISTS "${header}")
    message(FATAL_ERROR "The verified XP BX SIMD selector is missing")
  endif()
  target_compile_options(bx PUBLIC
    "$<BUILD_INTERFACE:-msse2>"
    "$<BUILD_INTERFACE:$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include \"${header}\">>")
  # ASTC has a separate, real SSE2 implementation. Its C++ sources share the
  # BX usage flags, so select that supported implementation explicitly.
  target_compile_definitions(bimg PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:ASTCENC_SSE=20>")

  set(vulkan_probe "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/windows-xp-bgfx-vulkan.cpp")
  if(NOT EXISTS "${vulkan_probe}")
    message(FATAL_ERROR "The verified XP Vulkan probe is missing")
  endif()
  target_sources(bgfx PRIVATE "${vulkan_probe}")
endfunction()

cmake_language(DEFER CALL jce_xp_bgfx_use_sse2)
