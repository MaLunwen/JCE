# CMakeLists.txt — Build Cisco openh264 *decoder only* as a static lib.
#
# Custom CMake replacing upstream meson (which doesn't handle emscripten).
# Pure C++ build — no NASM/SIMD — works everywhere (slower but portable).

cmake_minimum_required(VERSION 3.20)
project(openh264dec CXX)

set(OH_ROOT "${JCE_OPENH264_SOURCE_DIR}")

# ── Common sources (decoder needs these) ─────────────────────────
file(GLOB OH_COMMON_SRCS
    "${OH_ROOT}/codec/common/src/*.cpp")

# ── Decoder core sources ─────────────────────────────────────────
file(GLOB OH_DEC_CORE_SRCS
    "${OH_ROOT}/codec/decoder/core/src/*.cpp")

# ── Decoder API plus ─────────────────────────────────────────────
set(OH_DEC_PLUS_SRCS
    "${OH_ROOT}/codec/decoder/plus/src/welsDecoderExt.cpp")

add_library(openh264dec STATIC
    ${OH_COMMON_SRCS}
    ${OH_DEC_CORE_SRCS}
    ${OH_DEC_PLUS_SRCS})

# ── Includes ─────────────────────────────────────────────────────
target_include_directories(openh264dec PUBLIC
    "${OH_ROOT}/codec/api/wels"
    "${OH_ROOT}/codec/api"
)
target_include_directories(openh264dec PRIVATE
    "${OH_ROOT}/codec"
    "${OH_ROOT}/codec/common/inc"
    "${OH_ROOT}/codec/decoder/core/inc"
    "${OH_ROOT}/codec/decoder/plus/inc")

# ── Defines: pure-C path, no SIMD ────────────────────────────────
target_compile_definitions(openh264dec PRIVATE
    HAVE_CACHE_LINE_ALIGN=0
    GENERIC=1
    STACK_ALIGN=8)
if(WIN32)
    target_compile_definitions(openh264dec PRIVATE
        _WIN32 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ── Compiler options ─────────────────────────────────────────────
if(MSVC)
    target_compile_options(openh264dec PRIVATE
        /W0 /EHsc /wd4244 /wd4267 /wd4018 /wd4305)
else()
    target_compile_options(openh264dec PRIVATE -w -fno-exceptions -fno-rtti)
endif()

# ── Provide a <wels/codec_api.h> path (mirrors Cisco install layout) ──
set(_WELS_DIR "${CMAKE_CURRENT_BINARY_DIR}/include/wels")
file(MAKE_DIRECTORY "${_WELS_DIR}")
file(GLOB OH_API_HDRS "${OH_ROOT}/codec/api/wels/*.h")
foreach(_h IN LISTS OH_API_HDRS)
    get_filename_component(_n "${_h}" NAME)
    configure_file("${_h}" "${_WELS_DIR}/${_n}" COPYONLY)
endforeach()
target_include_directories(openh264dec PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}/include")

# Threads
find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(openh264dec PRIVATE Threads::Threads)
endif()

# Alias to mirror conan target name
add_library(openh264::openh264 ALIAS openh264dec)

add_dependencies(openh264dec jce_vendor_verify_openh264)
