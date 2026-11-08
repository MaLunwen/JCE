cmake_minimum_required(VERSION 3.20)
project(TriggerGraphLab C CXX)

find_package(JCE REQUIRED)
add_executable(TriggerGraphLab src/main.c)
set_target_properties(TriggerGraphLab PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_definitions(TriggerGraphLab PRIVATE JCE_APP_AUTHORED_CAMERA_ONLY=1)
target_link_libraries(TriggerGraphLab PRIVATE JCE::JCE)
jce_script_enable(TriggerGraphLab LANGUAGES lua REQUIRED)
jce_target_embed_pak(TriggerGraphLab RESOURCE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/resources")
