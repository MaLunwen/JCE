cmake_minimum_required(VERSION 3.20)
project(JceBundleTemplateEmitter C CXX)
find_package(JCE REQUIRED)
add_executable(JceBundleTemplateEmitter src/main.c)
set_target_properties(JceBundleTemplateEmitter PROPERTIES C_STANDARD 99
    C_STANDARD_REQUIRED YES OUTPUT_NAME jce_bundle_template_emit)
target_link_libraries(JceBundleTemplateEmitter PRIVATE JCE::JCE)
jce_add_pak(JceBundleTemplateEmitter RESOURCE_DIRS resources NO_ENGINE_RESOURCES)
