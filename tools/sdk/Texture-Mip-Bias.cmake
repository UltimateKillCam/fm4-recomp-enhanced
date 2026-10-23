# Texture mip bias. Modified 2026-10-21. GPL-3.0-only.
# At 2x/3x resolution scale FM4's textures are sampled one or more mip levels sharper
# than the original 720p target, so fine tarmac grain and grass shimmer and speckle in
# motion. This build-local overlay of the D3D12 texture cache adds the cvar
# texture_mip_bias (default 0, clamped to -2..2): a sampler MipLODBias for mipmapped
# textures. Direct3D adds the sampler bias for explicit-LOD and gradient fetches alike,
# so it reaches every fetch the shader translator emits; base-map samplers (UI) are
# unaffected.
function(fm4_texture_mip_bias)
  if(NOT WIN32 OR NOT TARGET rexgpu-xenos)
    return()
  endif()
  set(fm4_input "${CMAKE_SOURCE_DIR}/src/graphics/d3d12/texture_cache.cpp")
  set(fm4_output "${CMAKE_BINARY_DIR}/fm4-texture-bias/texture_cache.cpp")
  file(READ "${fm4_input}" fm4_text)
  string(REPLACE "\r\n" "\n" fm4_text "${fm4_text}")
  macro(fm4_bias_swap old new what)
    string(FIND "${fm4_text}" "${old}" fm4_at)
    string(FIND "${fm4_text}" "${old}" fm4_last REVERSE)
    if(fm4_at LESS 0 OR NOT fm4_at EQUAL fm4_last)
      message(FATAL_ERROR "Texture mip bias: ${what} anchor missing or ambiguous")
    endif()
    string(REPLACE "${old}" "${new}" fm4_text "${fm4_text}")
  endmacro()
  fm4_bias_swap([=[REXCVAR_DEFINE_BOOL(d3d12_preserve_native_texture_pages, ]=]
    [=[REXCVAR_DEFINE_DOUBLE(texture_mip_bias, 0.0, "GPU",
                       "Extra mip LOD bias for mipmapped textures (-2..2); positive is softer and shimmers less.");
REXCVAR_DEFINE_BOOL(d3d12_preserve_native_texture_pages, ]=] "cvar")
  fm4_bias_swap([=[  // LOD biasing is performed in shaders.
  desc.MipLODBias = 0.0f;]=]
    [=[  // LOD biasing is performed in shaders; FM4 adds an optional global bias for
  // mipmapped textures (texture_mip_bias) to reduce shimmer at raised resolution.
  // Only for block-compressed formats (2026-10-23): FM4 fills some car paint textures in the
  // garage itself and their lower mips can be uninitialized (speckles).
  desc.MipLODBias = (parameters.mip_base_map || !parameters.mip_bias_allowed)
                        ? 0.0f
                        : float(std::clamp(double(REXCVAR_GET(texture_mip_bias)), -2.0, 2.0));]=] "sampler bias")
  if(EXISTS "${fm4_output}")
    file(READ "${fm4_output}" fm4_previous)
  endif()
  if(NOT fm4_previous STREQUAL fm4_text)
    file(WRITE "${fm4_output}" "${fm4_text}")
  endif()
  get_target_property(fm4_dir rexgpu-xenos SOURCE_DIR)
  get_target_property(fm4_sources rexgpu-xenos SOURCES)
  set(fm4_result)
  set(fm4_replaced 0)
  foreach(fm4_entry IN LISTS fm4_sources)
    if(IS_ABSOLUTE "${fm4_entry}")
      get_filename_component(fm4_abs "${fm4_entry}" ABSOLUTE)
    else()
      get_filename_component(fm4_abs "${fm4_dir}/${fm4_entry}" ABSOLUTE)
    endif()
    get_filename_component(fm4_target "${fm4_input}" ABSOLUTE)
    if(fm4_abs STREQUAL fm4_target)
      list(APPEND fm4_result "${fm4_output}")
      math(EXPR fm4_replaced "${fm4_replaced}+1")
    else()
      list(APPEND fm4_result "${fm4_entry}")
    endif()
  endforeach()
  if(NOT fm4_replaced EQUAL 1)
    message(FATAL_ERROR "Texture mip bias: expected exactly one d3d12/texture_cache.cpp source; found ${fm4_replaced}")
  endif()
  set_property(TARGET rexgpu-xenos PROPERTY SOURCES "${fm4_result}")
  set_property(SOURCE "${fm4_output}" TARGET_DIRECTORY rexgpu-xenos APPEND PROPERTY
    INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/src/graphics/d3d12" "${CMAKE_SOURCE_DIR}/src/graphics")
  set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fm4_input}")
endfunction()
fm4_texture_mip_bias()
