# CMakeLists.txt — Build AOSP libhevc decoder as a static library.
#
# Custom CMake replacing the upstream one (which assumes GCC/Linux).
# Supports MSVC, GCC, and Clang on x86_64 and aarch64.

cmake_minimum_required(VERSION 3.20)
project(libhevcdec C)

set(HEVC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# ── Common (generic) sources ──────────────────────────────────────
set(LIBHEVC_COMMON_SRCS
    "${HEVC_ROOT}/common/ihevc_quant_tables.c"
    "${HEVC_ROOT}/common/ihevc_inter_pred_filters.c"
    "${HEVC_ROOT}/common/ihevc_weighted_pred.c"
    "${HEVC_ROOT}/common/ihevc_padding.c"
    "${HEVC_ROOT}/common/ihevc_deblk_edge_filter.c"
    "${HEVC_ROOT}/common/ihevc_deblk_tables.c"
    "${HEVC_ROOT}/common/ihevc_cabac_tables.c"
    "${HEVC_ROOT}/common/ihevc_common_tables.c"
    "${HEVC_ROOT}/common/ihevc_intra_pred_filters.c"
    "${HEVC_ROOT}/common/ihevc_chroma_intra_pred_filters.c"
    "${HEVC_ROOT}/common/ihevc_mem_fns.c"
    "${HEVC_ROOT}/common/ihevc_sao.c"
    "${HEVC_ROOT}/common/ihevc_trans_tables.c"
    "${HEVC_ROOT}/common/ihevc_recon.c"
    "${HEVC_ROOT}/common/ihevc_itrans.c"
    "${HEVC_ROOT}/common/ihevc_itrans_recon.c"
    "${HEVC_ROOT}/common/ihevc_iquant_recon.c"
    "${HEVC_ROOT}/common/ihevc_iquant_itrans_recon.c"
    "${HEVC_ROOT}/common/ihevc_itrans_recon_32x32.c"
    "${HEVC_ROOT}/common/ihevc_itrans_recon_16x16.c"
    "${HEVC_ROOT}/common/ihevc_itrans_recon_8x8.c"
    "${HEVC_ROOT}/common/ihevc_chroma_itrans_recon.c"
    "${HEVC_ROOT}/common/ihevc_chroma_iquant_recon.c"
    "${HEVC_ROOT}/common/ihevc_chroma_iquant_itrans_recon.c"
    "${HEVC_ROOT}/common/ihevc_chroma_recon.c"
    "${HEVC_ROOT}/common/ihevc_chroma_itrans_recon_16x16.c"
    "${HEVC_ROOT}/common/ihevc_chroma_itrans_recon_8x8.c"
    "${HEVC_ROOT}/common/ihevc_buf_mgr.c"
    "${HEVC_ROOT}/common/ihevc_disp_mgr.c"
    "${HEVC_ROOT}/common/ihevc_dpb_mgr.c"
    "${HEVC_ROOT}/common/ihevc_hbd_deblk_edge_filter.c"
    "${HEVC_ROOT}/common/ihevc_quant_iquant_ssd.c"
    "${HEVC_ROOT}/common/ihevc_resi_trans.c")

# ── Decoder (generic) sources ─────────────────────────────────────
set(LIBHEVCDEC_SRCS
    "${HEVC_ROOT}/decoder/ihevcd_version.c"
    "${HEVC_ROOT}/decoder/ihevcd_api.c"
    "${HEVC_ROOT}/decoder/ihevcd_decode.c"
    "${HEVC_ROOT}/decoder/ihevcd_nal.c"
    "${HEVC_ROOT}/decoder/ihevcd_bitstream.c"
    "${HEVC_ROOT}/decoder/ihevcd_parse_headers.c"
    "${HEVC_ROOT}/decoder/ihevcd_parse_slice_header.c"
    "${HEVC_ROOT}/decoder/ihevcd_parse_slice.c"
    "${HEVC_ROOT}/decoder/ihevcd_parse_residual.c"
    "${HEVC_ROOT}/decoder/ihevcd_cabac.c"
    "${HEVC_ROOT}/decoder/ihevcd_intra_pred_mode_prediction.c"
    "${HEVC_ROOT}/decoder/ihevcd_process_slice.c"
    "${HEVC_ROOT}/decoder/ihevcd_utils.c"
    "${HEVC_ROOT}/decoder/ihevcd_job_queue.c"
    "${HEVC_ROOT}/decoder/ihevcd_ref_list.c"
    "${HEVC_ROOT}/decoder/ihevcd_get_mv.c"
    "${HEVC_ROOT}/decoder/ihevcd_mv_pred.c"
    "${HEVC_ROOT}/decoder/ihevcd_mv_merge.c"
    "${HEVC_ROOT}/decoder/ihevcd_iquant_itrans_recon_ctb.c"
    "${HEVC_ROOT}/decoder/ihevcd_itrans_recon_dc.c"
    "${HEVC_ROOT}/decoder/ihevcd_common_tables.c"
    "${HEVC_ROOT}/decoder/ihevcd_boundary_strength.c"
    "${HEVC_ROOT}/decoder/ihevcd_deblk.c"
    "${HEVC_ROOT}/decoder/ihevcd_inter_pred.c"
    "${HEVC_ROOT}/decoder/ihevcd_sao.c"
    "${HEVC_ROOT}/decoder/ihevcd_ilf_padding.c"
    "${HEVC_ROOT}/decoder/ihevcd_fmt_conv.c")

# ── Threading source ──────────────────────────────────────────────
if(WIN32)
    list(APPEND LIBHEVC_COMMON_SRCS "${HEVC_ROOT}/common/ithread_win32.c")
else()
    list(APPEND LIBHEVC_COMMON_SRCS "${HEVC_ROOT}/common/ithread.c")
endif()

# ── Platform-specific SIMD sources ────────────────────────────────
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|x86|i[3-6]86)$")
    # x86 common SSSE3 + SSE4.2 intrinsics
    list(APPEND LIBHEVC_COMMON_SRCS
        "${HEVC_ROOT}/common/x86/ihevc_inter_pred_filters_ssse3_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_weighted_pred_ssse3_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_intra_pred_filters_ssse3_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_chroma_intra_pred_filters_ssse3_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_itrans_recon_ssse3_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_itrans_recon_16x16_ssse3_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_itrans_recon_32x32_ssse3_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_sao_ssse3_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_deblk_ssse3_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_padding_ssse3_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_mem_fns_ssse3_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_inter_pred_filters_sse42_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_weighted_pred_sse42_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_intra_pred_filters_sse42_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_chroma_intra_pred_filters_sse42_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_itrans_recon_sse42_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_16x16_itrans_recon_sse42_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_32x32_itrans_recon_sse42_intr.c"
        "${HEVC_ROOT}/common/x86/ihevc_tables_x86_intr.c")

    # x86 decoder SIMD
    list(APPEND LIBHEVCDEC_SRCS
        "${HEVC_ROOT}/decoder/x86/ihevcd_function_selector.c"
        "${HEVC_ROOT}/decoder/x86/ihevcd_function_selector_generic.c"
        "${HEVC_ROOT}/decoder/x86/ihevcd_function_selector_ssse3.c"
        "${HEVC_ROOT}/decoder/x86/ihevcd_function_selector_sse42.c"
        "${HEVC_ROOT}/decoder/x86/ihevcd_fmt_conv_ssse3_intr.c"
        "${HEVC_ROOT}/decoder/x86/ihevcd_it_rec_dc_ssse3_intr.c"
        "${HEVC_ROOT}/decoder/x86/ihevcd_it_rec_dc_sse42_intr.c")

elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    # aarch64 common C intrinsics
    list(APPEND LIBHEVC_COMMON_SRCS
        "${HEVC_ROOT}/common/arm/ihevc_intra_pred_filters_neon_intr.c"
        "${HEVC_ROOT}/common/arm/ihevc_quant_iquant_ssd_neon_intr.c"
        "${HEVC_ROOT}/common/arm/ihevc_weighted_pred_neon_intr.c"
        "${HEVC_ROOT}/common/arm/ihevc_resi_trans_neon_32x32.c"
        "${HEVC_ROOT}/common/arm/ihevc_resi_trans_neon.c"
        "${HEVC_ROOT}/common/arm/ihevc_intra_ref_substitution_a9q.c")

    # aarch64 common assembly
    set(LIBHEVC_COMMON_ASMS
        "${HEVC_ROOT}/common/arm64/ihevc_deblk_chroma_horz.s"
        "${HEVC_ROOT}/common/arm64/ihevc_deblk_chroma_vert.s"
        "${HEVC_ROOT}/common/arm64/ihevc_deblk_luma_horz.s"
        "${HEVC_ROOT}/common/arm64/ihevc_deblk_luma_vert.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_chroma_copy.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_chroma_copy_w16out.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_chroma_horz.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_chroma_horz_w16out.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_chroma_vert.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_chroma_vert_w16inp.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_chroma_vert_w16inp_w16out.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_chroma_vert_w16out.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_filters_luma_horz.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_filters_luma_vert.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_filters_luma_vert_w16inp.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_filters_luma_vert_w16out.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_luma_copy.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_luma_copy_w16out.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_luma_horz_w16out.s"
        "${HEVC_ROOT}/common/arm64/ihevc_inter_pred_luma_vert_w16inp_w16out.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_chroma_dc.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_chroma_horz.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_chroma_mode_18_34.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_chroma_mode_27_to_33.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_chroma_mode2.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_chroma_mode_3_to_9.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_chroma_planar.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_chroma_ver.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_filters_chroma_mode_11_to_17.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_filters_chroma_mode_19_to_25.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_filters_luma_mode_11_to_17.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_filters_luma_mode_19_to_25.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_luma_dc.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_luma_horz.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_luma_mode_18_34.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_luma_mode_27_to_33.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_luma_mode2.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_luma_mode_3_to_9.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_luma_planar.s"
        "${HEVC_ROOT}/common/arm64/ihevc_intra_pred_luma_vert.s"
        "${HEVC_ROOT}/common/arm64/ihevc_itrans_recon_16x16.s"
        "${HEVC_ROOT}/common/arm64/ihevc_itrans_recon_32x32.s"
        "${HEVC_ROOT}/common/arm64/ihevc_itrans_recon_4x4.s"
        "${HEVC_ROOT}/common/arm64/ihevc_itrans_recon_4x4_ttype1.s"
        "${HEVC_ROOT}/common/arm64/ihevc_itrans_recon_8x8.s"
        "${HEVC_ROOT}/common/arm64/ihevc_mem_fns.s"
        "${HEVC_ROOT}/common/arm64/ihevc_neon_macros.s"
        "${HEVC_ROOT}/common/arm64/ihevc_padding.s"
        "${HEVC_ROOT}/common/arm64/ihevc_sao_band_offset_chroma.s"
        "${HEVC_ROOT}/common/arm64/ihevc_sao_band_offset_luma.s"
        "${HEVC_ROOT}/common/arm64/ihevc_sao_edge_offset_class0_chroma.s"
        "${HEVC_ROOT}/common/arm64/ihevc_sao_edge_offset_class0.s"
        "${HEVC_ROOT}/common/arm64/ihevc_sao_edge_offset_class1_chroma.s"
        "${HEVC_ROOT}/common/arm64/ihevc_sao_edge_offset_class1.s"
        "${HEVC_ROOT}/common/arm64/ihevc_sao_edge_offset_class2_chroma.s"
        "${HEVC_ROOT}/common/arm64/ihevc_sao_edge_offset_class2.s"
        "${HEVC_ROOT}/common/arm64/ihevc_sao_edge_offset_class3_chroma.s"
        "${HEVC_ROOT}/common/arm64/ihevc_sao_edge_offset_class3.s"
        "${HEVC_ROOT}/common/arm64/ihevc_weighted_pred_bi_default.s"
        "${HEVC_ROOT}/common/arm64/ihevc_weighted_pred_bi.s"
        "${HEVC_ROOT}/common/arm64/ihevc_weighted_pred_uni.s")

    # aarch64 decoder
    list(APPEND LIBHEVCDEC_SRCS
        "${HEVC_ROOT}/decoder/arm/ihevcd_function_selector.c"
        "${HEVC_ROOT}/decoder/arm/ihevcd_function_selector_noneon.c"
        "${HEVC_ROOT}/decoder/arm64/ihevcd_function_selector_av8.c")
    list(APPEND LIBHEVC_COMMON_ASMS
        "${HEVC_ROOT}/decoder/arm64/ihevcd_fmt_conv_420sp_to_420p.s"
        "${HEVC_ROOT}/decoder/arm64/ihevcd_fmt_conv_420sp_to_420sp.s"
        "${HEVC_ROOT}/decoder/arm64/ihevcd_fmt_conv_420sp_to_rgba8888.s"
        "${HEVC_ROOT}/decoder/arm64/ihevcd_itrans_recon_dc_chroma.s"
        "${HEVC_ROOT}/decoder/arm64/ihevcd_itrans_recon_dc_luma.s")
endif()

# ── Static library target ─────────────────────────────────────────
add_library(libhevcdec STATIC
    ${LIBHEVC_COMMON_SRCS}
    ${LIBHEVC_COMMON_ASMS}
    ${LIBHEVCDEC_SRCS})

# ── Include directories ───────────────────────────────────────────
target_include_directories(libhevcdec PUBLIC
    "${HEVC_ROOT}/common"
    "${HEVC_ROOT}/decoder")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|x86|i[3-6]86)$")
    target_include_directories(libhevcdec PUBLIC
        "${HEVC_ROOT}/common/x86"
        "${HEVC_ROOT}/decoder/x86")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    target_include_directories(libhevcdec PUBLIC
        "${HEVC_ROOT}/common/arm"
        "${HEVC_ROOT}/common/arm64"
        "${HEVC_ROOT}/decoder/arm"
        "${HEVC_ROOT}/decoder/arm64")
endif()

# ── Preprocessor defines ──────────────────────────────────────────
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|x86|i[3-6]86)$")
    target_compile_definitions(libhevcdec PRIVATE
        X86
        DISABLE_AVX2
        DEFAULT_ARCH=D_ARCH_X86_SSE42)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    target_compile_definitions(libhevcdec PRIVATE
        ARMV8
        DEFAULT_ARCH=D_ARCH_ARMV8_GENERIC)
endif()

if(MSVC)
    target_compile_definitions(libhevcdec PRIVATE MSVC)
endif()

# ── Compiler flags ────────────────────────────────────────────────
if(MSVC)
    # MSVC: SSE4.2 intrinsics are always available on x64.
    # Suppress common warnings in third-party code.
    target_compile_options(libhevcdec PRIVATE
        /W0              # Suppress all warnings for vendored code
        /wd4244 /wd4267  # Conversion warnings
        /wd4018          # Signed/unsigned mismatch
    )
else()
    # GCC/Clang: enable SSE4.2 for x86
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|x86|i[3-6]86)$")
        target_compile_options(libhevcdec PRIVATE -msse4.2 -mno-avx)
    endif()
    target_compile_options(libhevcdec PRIVATE -w)  # Suppress warnings for vendored code
endif()

# Threading
find_package(Threads)
if(Threads_FOUND AND NOT WIN32)
    target_link_libraries(libhevcdec PRIVATE Threads::Threads)
endif()
