# SkyFrame layer (own identity, phase-1 alias over vetted lsfg-vk 2.0 core)
#
# Phase 1 (this release): no C++ fork. VK_LAYER_SKYFRAME_frame_generation
# manifests point at liblsfg-vk-layer.so; activation via ENABLE_SKYFRAME=1
# through skyframe-run. See decky-plugin/main.py sync_vulkan_layer().
#
# Phase 2 (planned fork, GPL lineage only):
#   base = lsfg-vk v2 GPL tree at upstream commit 8b0da266 (same as MAKO lineage),
#   NOT the CC BY-NC-ND 4.0 final 2.0.0 source (derivatives forbidden).
#   1. copy engine/lsfg/lsfg-vk-layer -> engine/skyframe/layer-src
#   2. rename namespace lsfgvk::layer -> skyframe::layer, log prefix, layer name
#      VK_LAYER_LSFGVK_frame_generation -> VK_LAYER_SKYFRAME_frame_generation
#   3. split swapchain.cpp -> create/resources/profile/present/status (MAKO layout)
#   4. add frame_generation_enabled live-flag + adaptive scheduler + stats shm
#      writing /tmp/skyframe_stats.json {base_fps, output_fps, gen_ms}
#   5. build libskyframe.so (+_32.so), swap library_path in manifests.
cmake_minimum_required(VERSION 3.16)

# Alias manifest template for documentation / manual installs.
# Real manifests are generated at runtime by decky-plugin/main.py so that
# library paths always match ~/.local/lib on the Deck.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_SKYFRAME_frame_generation.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_SKYFRAME_frame_generation.json"
    @ONLY)
