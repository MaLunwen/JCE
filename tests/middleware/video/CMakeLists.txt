cmake_minimum_required(VERSION 3.20)

# jce_video carries JCE_ENABLE_PATENTED_CODECS=1 as a PUBLIC compile
# definition when the adapters are compiled in, so the test sees the same
# gate the engine does.
jce_add_unit_test(test_jce_video_patented_codecs
    SOURCES test_jce_video_patented_codecs.c
    LINK    jce_core jce_video jce_audio jce_resource)
target_include_directories(test_jce_video_patented_codecs PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/src/middleware/video")

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
