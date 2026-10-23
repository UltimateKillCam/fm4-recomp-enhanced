# Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
# Add the project feedback bridge without modifying the pinned SDK checkout.
set(FM4_CONTROLLER_BRIDGE_DIR "${CMAKE_CURRENT_LIST_DIR}")
function(fm4_extend_sdk)
  include("${FM4_CONTROLLER_BRIDGE_DIR}/Runtime-Fixes.cmake")
  include("${FM4_CONTROLLER_BRIDGE_DIR}/Gpu-Completion.cmake")
  include("${FM4_CONTROLLER_BRIDGE_DIR}/Async-Submit.cmake")
  include("${FM4_CONTROLLER_BRIDGE_DIR}/Sun-Visibility.cmake")
  include("${FM4_CONTROLLER_BRIDGE_DIR}/Pass-Timing.cmake")
  include("${FM4_CONTROLLER_BRIDGE_DIR}/Submit-Interval.cmake")
  include("${FM4_CONTROLLER_BRIDGE_DIR}/Stub-Logging.cmake")
  include("${FM4_CONTROLLER_BRIDGE_DIR}/Page-Validity.cmake")
  include("${FM4_CONTROLLER_BRIDGE_DIR}/Presenter-Downscale.cmake")
  include("${FM4_CONTROLLER_BRIDGE_DIR}/Texture-Mip-Bias.cmake")
  if(WIN32 AND TARGET rexruntime)
    target_sources(rexruntime PRIVATE "${FM4_CONTROLLER_BRIDGE_DIR}/controller_bridge.cpp")
    target_sources(rexruntime PRIVATE "${FM4_CONTROLLER_BRIDGE_DIR}/quiet_window.cpp")
    if(TARGET rexui)
      set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/ui/window_sdl.cpp" TARGET_DIRECTORY rexui APPEND PROPERTY
        COMPILE_DEFINITIONS SDL_ShowWindow=Fm4QuietShowWindow SDL_ShowCursor=Fm4QuietShowCursor
        SDL_HideCursor=Fm4QuietHideCursor SDL_SetWindowRelativeMouseMode=Fm4QuietRelativeMouse
        SDL_CaptureMouse=Fm4QuietCaptureMouse)
    endif()
    target_sources(rexruntime PRIVATE "${FM4_CONTROLLER_BRIDGE_DIR}/temporal_bridge.cpp")
    target_link_libraries(rexruntime PRIVATE d3d12)
    set(fm4_ngx "${FM4_CONTROLLER_BRIDGE_DIR}/../../.local/streamline-current/external/ngx-sdk")
    if(EXISTS "${fm4_ngx}/include/nvsdk_ngx.h")
      set_property(SOURCE "${FM4_CONTROLLER_BRIDGE_DIR}/temporal_bridge.cpp" TARGET_DIRECTORY rexruntime APPEND PROPERTY
        COMPILE_DEFINITIONS FM4_HAS_NEURAL)
      set_property(SOURCE "${FM4_CONTROLLER_BRIDGE_DIR}/temporal_bridge.cpp" TARGET_DIRECTORY rexruntime APPEND PROPERTY
        INCLUDE_DIRECTORIES "${fm4_ngx}/include")
      target_link_libraries(rexruntime PRIVATE "${fm4_ngx}/lib/Windows_x86_64/nvsdk_ngx_d.lib")
    endif()
    # Optional separately installed vendor SDKs. No vendor files enter source
    # archives; the spatial renderer builds without either SDK.
    set(fm4_vendor_root "${FM4_CONTROLLER_BRIDGE_DIR}/../../.local")
    if(EXISTS "${fm4_vendor_root}/streamline-current/include/sl.h")
      target_compile_definitions(rexruntime PRIVATE FM4_HAS_STREAMLINE)
      target_include_directories(rexruntime PRIVATE "${fm4_vendor_root}/streamline-current/include")
    endif()
    set(fm4_ffx "${fm4_vendor_root}/fsr-sdk/Kits/FidelityFX")
    if(EXISTS "${fm4_ffx}/api/include/ffx_api_loader.h")
      target_compile_definitions(rexruntime PRIVATE FM4_HAS_FSR_API)
      target_include_directories(rexruntime PRIVATE "${fm4_ffx}/api/include" "${fm4_ffx}/upscalers/include")
    endif()
  endif()
endfunction()
cmake_language(DEFER CALL fm4_extend_sdk)
