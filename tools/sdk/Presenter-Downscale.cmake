# Presenter area-filter downscale. Modified 2026-10-21. GPL-3.0-only.
# Supersampling draws above the output size (for example 3840x2160 shown at 2560x1440).
# The SDK's guest output resample takes one bilinear tap per output pixel, which skips
# source texels at non-integer ratios and makes fine texture such as tarmac shimmer.
# This build-local overlay of d3d12_presenter.cpp swaps the two bilinear pixel shaders
# for shaders/guest_output_area.ps.hlsl (precompiled headers beside it): same root
# constants, texture and sampler; one tap when not downscaling, an averaged grid of
# taps over each output pixel's source footprint when downscaling.
function(fm4_presenter_downscale)
  if(NOT WIN32 OR NOT TARGET rexui)
    return()
  endif()
  set(fm4_shaders "${FM4_CONTROLLER_BRIDGE_DIR}/shaders")
  foreach(fm4_header fm4_guest_output_area_ps.h fm4_guest_output_area_dither_ps.h)
    if(NOT EXISTS "${fm4_shaders}/${fm4_header}")
      message(FATAL_ERROR "Presenter downscale: ${fm4_header} is missing")
    endif()
  endforeach()
  set(fm4_input "${CMAKE_SOURCE_DIR}/src/ui/d3d12/d3d12_presenter.cpp")
  set(fm4_output "${CMAKE_BINARY_DIR}/fm4-presenter/d3d12_presenter.cpp")
  file(READ "${fm4_input}" fm4_text)
  string(REPLACE "\r\n" "\n" fm4_text "${fm4_text}")
  foreach(fm4_pair
      "guest_output_bilinear_dither_ps.h|fm4_guest_output_area_dither_ps.h"
      "guest_output_bilinear_ps.h|fm4_guest_output_area_ps.h")
    string(REPLACE "|" ";" fm4_pair "${fm4_pair}")
    list(GET fm4_pair 0 fm4_old)
    list(GET fm4_pair 1 fm4_new)
    set(fm4_line "#include \"ui/shaders/bytecode/d3d12_5_1/${fm4_old}\"")
    string(FIND "${fm4_text}" "${fm4_line}" fm4_at)
    string(FIND "${fm4_text}" "${fm4_line}" fm4_last REVERSE)
    if(fm4_at LESS 0 OR NOT fm4_at EQUAL fm4_last)
      message(FATAL_ERROR "Presenter downscale: include of ${fm4_old} missing or ambiguous")
    endif()
    string(REPLACE "${fm4_line}" "#include \"${fm4_shaders}/${fm4_new}\"" fm4_text "${fm4_text}")
  endforeach()
  if(EXISTS "${fm4_output}")
    file(READ "${fm4_output}" fm4_previous)
  endif()
  if(NOT fm4_previous STREQUAL fm4_text)
    file(WRITE "${fm4_output}" "${fm4_text}")
  endif()
  get_target_property(fm4_ui_sources rexui SOURCES)
  set(fm4_overlaid)
  set(fm4_replaced 0)
  foreach(fm4_source IN LISTS fm4_ui_sources)
    if(fm4_source STREQUAL "d3d12/d3d12_presenter.cpp" OR fm4_source STREQUAL "${fm4_input}")
      list(APPEND fm4_overlaid "${fm4_output}")
      math(EXPR fm4_replaced "${fm4_replaced}+1")
    else()
      list(APPEND fm4_overlaid "${fm4_source}")
    endif()
  endforeach()
  if(NOT fm4_replaced EQUAL 1)
    message(FATAL_ERROR "Presenter downscale: rexui source list does not match the reviewed overlay")
  endif()
  set_property(TARGET rexui PROPERTY SOURCES "${fm4_overlaid}")
  set_property(SOURCE "${fm4_output}" TARGET_DIRECTORY rexui APPEND PROPERTY
    INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/src/ui/d3d12" "${CMAKE_SOURCE_DIR}/src/ui")
  set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${fm4_input}" "${fm4_shaders}/fm4_guest_output_area_ps.h" "${fm4_shaders}/fm4_guest_output_area_dither_ps.h")
endfunction()
fm4_presenter_downscale()
