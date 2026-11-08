cmake_minimum_required(VERSION 3.20)

# jce_video carries JCE_ENABLE_PATENTED_CODECS=1 as a PUBLIC compile
# definition when the adapters are compiled in, so the test sees the same
# gate the engine does.
jce_add_unit_test(test_jce_video_patented_codecs
    SOURCES test_jce_video_patented_codecs.c
    LINK    jce_core jce_video jce_audio jce_resource)
target_include_directories(test_jce_video_patented_codecs PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/video")
target_compile_definitions(test_jce_video_patented_codecs PRIVATE
    JCE_TEST_COLOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

jce_add_unit_test(test_jce_av1_pipeline
    SOURCES test_jce_av1_pipeline.c
    LINK jce_core jce_video)
target_include_directories(test_jce_av1_pipeline PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/video")

jce_add_unit_test(test_jce_audio_stream_seek
    SOURCES test_jce_audio_stream_seek.c
    LINK jce_core jce_video)
target_include_directories(test_jce_audio_stream_seek PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/video")

jce_add_unit_test(test_jce_mp4_source_limits
    SOURCES test_jce_mp4_source_limits.c
    LINK jce_core jce_video)
target_include_directories(test_jce_mp4_source_limits PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/video")

jce_add_unit_test(test_jce_mp4_bframes
    SOURCES test_jce_mp4_bframes.c
    LINK jce_core jce_video jce_audio jce_resource)
target_compile_definitions(test_jce_mp4_bframes PRIVATE
    JCE_TEST_BFRAME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# Owned H264 planes retain legacy pixels and actual reordered timestamps.
jce_add_unit_test(test_jce_h264_planes
    SOURCES test_jce_h264_planes.c
    LINK jce_core jce_video)
target_include_directories(test_jce_h264_planes PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/video"
    "${CMAKE_SOURCE_DIR}/engine/src")
target_compile_definitions(test_jce_h264_planes PRIVATE
    JCE_TEST_BFRAME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# Actual public CPU decode/seek/worker lifecycle, no renderer/window/audio device.
jce_add_unit_test(test_jce_video_lifecycle_trace
    KIND integration
    SOURCES test_jce_video_lifecycle_trace.c
    LINK jce_core jce_video jce_audio jce_resource
    INCLUDES "${CMAKE_SOURCE_DIR}/engine/src/middleware/video"
        "${CMAKE_SOURCE_DIR}/engine/src")
target_compile_definitions(test_jce_video_lifecycle_trace PRIVATE
    JCE_TEST_VIDEO_TRACE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
