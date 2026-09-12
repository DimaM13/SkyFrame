cmake_minimum_required(VERSION 3.16)
project(SkyFrame VERSION 1.0.7 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Vulkan REQUIRED)

# Shader compilation
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_SOURCES
    "${SHADER_DIR}/warp_rgba.comp"
    "${SHADER_DIR}/downsample.comp"
    "${SHADER_DIR}/warp_blend.comp"
)

find_program(GLSL_VALIDATOR glslangValidator HINTS /usr/bin /usr/local/bin)
if (NOT GLSL_VALIDATOR)
    find_program(GLSL_VALIDATOR glslc HINTS /usr/bin /usr/local/bin)
endif()

set(SPIRV_OUTPUTS "")
foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SPV_OUT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT ${SPV_OUT}
        COMMAND ${GLSL_VALIDATOR} -V ${SHADER} -o ${SPV_OUT}
        DEPENDS ${SHADER}
        COMMENT "Compiling compute shader: ${SHADER_NAME}"
    )
    list(APPEND SPIRV_OUTPUTS ${SPV_OUT})
endforeach()

add_custom_target(SkyFrameShaders DEPENDS ${SPIRV_OUTPUTS})

# Vulkan Layer Library
add_library(VkLayer_skyframe SHARED
    layer/skyframe_layer.cpp
    layer/vulkan_warper.cpp
    layer/flow_ncnn.cpp
    layer/frame_pacer.cpp
)

add_dependencies(VkLayer_skyframe SkyFrameShaders)

target_include_directories(VkLayer_skyframe PRIVATE
    ${Vulkan_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/layer
)

target_link_libraries(VkLayer_skyframe PRIVATE
    Vulkan::Vulkan
    pthread
)

set_target_properties(VkLayer_skyframe PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "VkLayer_skyframe"
)

# Configure Vulkan layer manifest
configure_file(
    layer/vk_layer_skyframe.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/vk_layer_skyframe.json
    @ONLY
)
