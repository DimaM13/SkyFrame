cmake_minimum_required(VERSION 3.16)
project(SkyFrame VERSION 1.3.1 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Vulkan REQUIRED)

# Compile compute shaders to C++ headers for embedded inclusion
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_SOURCES
    "${SHADER_DIR}/warp_rgba.comp"
    "${SHADER_DIR}/downsample.comp"
    "${SHADER_DIR}/motion_field.comp"
    "${SHADER_DIR}/flow_filter.comp"
    "${SHADER_DIR}/warp_blend.comp"
)

find_program(GLSL_VALIDATOR glslangValidator HINTS /usr/bin /usr/local/bin)
if (NOT GLSL_VALIDATOR)
    find_program(GLSL_VALIDATOR glslc HINTS /usr/bin /usr/local/bin)
endif()

set(SPIRV_HEADERS "")
foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    string(REGEX REPLACE "[^a-zA-Z0-9_]" "_" SHADER_SAFE "${SHADER_NAME}")
    set(SPV_OUT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_SAFE}.spv")
    set(HEADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER_SAFE}_spv.h")
    add_custom_command(
        OUTPUT ${HEADER_OUT}
        COMMAND ${GLSL_VALIDATOR} -V ${SHADER} -o ${SPV_OUT}
        COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/scripts/spv_to_header.py" "${SPV_OUT}" "${HEADER_OUT}" "${SHADER_SAFE}"
        DEPENDS ${SHADER} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/spv_to_header.py"
        COMMENT "Compiling compute shader to C++ header: ${SHADER_NAME}"
    )
    list(APPEND SPIRV_HEADERS ${HEADER_OUT})
endforeach()

add_custom_target(SkyFrameShaders DEPENDS ${SPIRV_HEADERS})

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
    ${CMAKE_CURRENT_BINARY_DIR}
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
