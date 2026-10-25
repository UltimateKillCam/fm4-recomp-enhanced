# Runtime graphics settings. GPL-3.0-only.
# Compose the final sources produced by other overlays, without changing their inputs.
if(NOT WIN32 OR NOT TARGET rexgpu-xenos OR NOT TARGET rexruntime)
  return()
endif()
function(fm4_live_source target suffix output_name)
  get_target_property(entries ${target} SOURCES)
  get_target_property(base ${target} SOURCE_DIR)
  set(found "")
  foreach(entry IN LISTS entries)
    if(entry MATCHES "${suffix}$")
      if(found)
        message(FATAL_ERROR "Live settings: ambiguous ${suffix}")
      endif()
      set(found "${entry}")
    endif()
  endforeach()
  if(NOT found)
    message(FATAL_ERROR "Live settings: missing ${suffix}")
  endif()
  if(IS_ABSOLUTE "${found}")
    set(input "${found}")
  else()
    set(input "${base}/${found}")
  endif()
  file(READ "${input}" content)
  string(REPLACE "\r\n" "\n" content "${content}")
  set(${output_name} "${content}" PARENT_SCOPE)
  set(${output_name}_input "${input}" PARENT_SCOPE)
  set(${output_name}_entry "${found}" PARENT_SCOPE)
endfunction()
function(fm4_live_replace variable before after)
  string(FIND "${${variable}}" "${before}" first)
  string(FIND "${${variable}}" "${before}" last REVERSE)
  if(first LESS 0 OR NOT first EQUAL last)
    message(FATAL_ERROR "Live settings: anchor missing or ambiguous: ${before}")
  endif()
  string(REPLACE "${before}" "${after}" result "${${variable}}")
  set(${variable} "${result}" PARENT_SCOPE)
endfunction()
function(fm4_live_publish target variable filename)
  set(output "${CMAKE_BINARY_DIR}/fm4-live-settings/${filename}")
  if(EXISTS "${output}")
    file(READ "${output}" previous)
  endif()
  if(NOT previous STREQUAL "${${variable}}")
    file(WRITE "${output}" "${${variable}}")
  endif()
  get_target_property(entries ${target} SOURCES)
  list(FIND entries "${${variable}_entry}" index)
  list(REMOVE_AT entries ${index})
  list(INSERT entries ${index} "${output}")
  set_property(TARGET ${target} PROPERTY SOURCES "${entries}")
  get_filename_component(original_dir "${${variable}_input}" DIRECTORY)
  set_property(SOURCE "${output}" TARGET_DIRECTORY ${target} APPEND PROPERTY INCLUDE_DIRECTORIES
    "${original_dir}" "${FM4_CONTROLLER_BRIDGE_DIR}" "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/graphics/d3d12")
  # Earlier overlays regenerate build-local inputs during configure. Depending
  # on those outputs would make Ninja reconfigure repeatedly.
  string(FIND "${${variable}_input}" "${CMAKE_BINARY_DIR}/" generated)
  if(NOT generated EQUAL 0)
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${${variable}_input}")
  endif()
endfunction()

target_sources(rexruntime PRIVATE "${FM4_CONTROLLER_BRIDGE_DIR}/live_settings.cpp")
set_property(SOURCE "${FM4_CONTROLLER_BRIDGE_DIR}/live_settings.cpp" TARGET_DIRECTORY rexruntime APPEND PROPERTY
  INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/thirdparty/inja/third_party/include")

fm4_live_source(rexgpu-xenos "d3d_command_processor[.]cpp|d3d12/command_processor[.]cpp" gpu)
fm4_live_replace(gpu "#include <rex/graphics/d3d12/command_processor.h>"
  "#include <rex/graphics/d3d12/command_processor.h>\n#include \"live_settings.h\"\nREXCVAR_DECLARE(bool, fm4_live_settings);")
fm4_live_replace(gpu [=[      shared_memory_->ClearCache();
    }
  }

  return true;
}]=] [=[      shared_memory_->ClearCache();
    }
#include "live_settings_frame.inc"
  }

  return true;
}]=])
fm4_live_publish(rexgpu-xenos gpu d3d_command_processor.cpp)

# Capability publication counts live initialized owners during transactional replacement.
fm4_live_source(rexgpu-xenos "d3d12/render_target_cache[.]cpp" rt)
fm4_live_replace(rt "return native_render_target_capabilities.load(std::memory_order_acquire);"
  "return native_render_target_capabilities.load(std::memory_order_acquire) ? 1 : 0;")
fm4_live_replace(rt [=[  native_render_targets_active_ = false;
  native_render_target_capabilities.store(0, std::memory_order_release);]=]
  "  native_render_targets_active_ = false;")
fm4_live_replace(rt "native_render_target_capabilities.store(1, std::memory_order_release);"
  "native_render_target_capabilities.fetch_add(1, std::memory_order_release);")
fm4_live_replace(rt [=[  native_render_target_capabilities.store(0, std::memory_order_release);
  native_render_targets_active_ = false;]=]
  [=[  if (native_render_targets_active_) native_render_target_capabilities.fetch_sub(1, std::memory_order_release);
  native_render_targets_active_ = false;]=])
fm4_live_publish(rexgpu-xenos rt render_target_cache.cpp)

# Neutralize guest controls while the overlay owns the controller/keyboard.
fm4_live_source(rexinput "input_system[.]cpp" input)
fm4_live_replace(input "#include <rex/input/input_system.h>"
  "#include <rex/input/input_system.h>\n#define FM4_LIVE_SETTINGS_EXPORT\n#include \"live_settings.h\"")
fm4_live_replace(input [=[  if (out_state) {
    *out_state = merged;
  }]=] [=[  if (out_state) {
    if (fm4::settings::Capturing()) merged.gamepad = {};
    *out_state = merged;
  }]=])
fm4_live_replace(input [=[                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");]=] [=[                                   X_INPUT_KEYSTROKE* out_keystroke) {
  SCOPE_profile_cpu_f("hid");
  if (fm4::settings::Capturing()) return X_ERROR_EMPTY;]=])
fm4_live_publish(rexinput input input_system.cpp)
