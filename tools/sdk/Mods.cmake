# Optional native extension host and read-only content overlay. GPL-3.0-only.
if(NOT WIN32 OR NOT TARGET rexruntime)
  message(FATAL_ERROR "FM4_MODS requires the Windows runtime")
endif()
target_sources(rexruntime PRIVATE "${FM4_CONTROLLER_BRIDGE_DIR}/mod_runtime.cpp")
set_property(SOURCE "${FM4_CONTROLLER_BRIDGE_DIR}/mod_runtime.cpp" TARGET_DIRECTORY rexruntime APPEND PROPERTY
  INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/thirdparty/inja/third_party/include")
target_link_libraries(rexruntime PRIVATE bcrypt)
if(FM4_LIVE_SETTINGS)
  set_property(SOURCE "${FM4_CONTROLLER_BRIDGE_DIR}/live_settings.cpp" TARGET_DIRECTORY rexruntime APPEND PROPERTY
    COMPILE_DEFINITIONS FM4_MODS_UI=1)
endif()

function(fm4_mod_source target suffix output_name)
  get_target_property(entries ${target} SOURCES)
  get_target_property(base ${target} SOURCE_DIR)
  set(found "")
  foreach(entry IN LISTS entries)
    if(entry MATCHES "${suffix}$")
      if(found)
        message(FATAL_ERROR "Mods: ambiguous source ${suffix}")
      endif()
      set(found "${entry}")
    endif()
  endforeach()
  if(NOT found)
    message(FATAL_ERROR "Mods: missing source ${suffix}")
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
function(fm4_mod_replace variable before after)
  string(FIND "${${variable}}" "${before}" first)
  string(FIND "${${variable}}" "${before}" last REVERSE)
  if(first LESS 0 OR NOT first EQUAL last)
    message(FATAL_ERROR "Mods: anchor missing or ambiguous: ${before}")
  endif()
  string(REPLACE "${before}" "${after}" content "${${variable}}")
  set(${variable} "${content}" PARENT_SCOPE)
endfunction()
function(fm4_mod_publish target variable filename)
  set(output "${CMAKE_BINARY_DIR}/fm4-mods/${filename}")
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
    "${original_dir}" "${FM4_CONTROLLER_BRIDGE_DIR}" "${CMAKE_SOURCE_DIR}/src")
  string(FIND "${${variable}_input}" "${CMAKE_BINARY_DIR}/" generated)
  if(NOT generated EQUAL 0)
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${${variable}_input}")
  endif()
endfunction()
fm4_mod_source(rexfilesystem "virtual_file_system[.]cpp" vfs)
fm4_mod_replace(vfs "#include <rex/filesystem/vfs.h>" "#include <rex/filesystem/vfs.h>\n#include \"mod_runtime.h\"")
fm4_mod_replace(vfs [=[  auto* entry = device->ResolvePath(relative_path);]=]
  [=[  auto* entry = fm4::mods::Overlay(normalized_path);
  if (!entry) entry = device->ResolvePath(relative_path);]=])
fm4_mod_replace(vfs [=[  if (entry) {
    if (entry->attributes() & kFileAttributeDirectory && is_non_directory) {]=]
  [=[  if (auto* replacement = fm4::mods::Overlay(path, root_entry)) {
    if ((creation_disposition != FileDisposition::kOpen && creation_disposition != FileDisposition::kOpenIf) ||
        (desired_access & (FileAccess::kFileWriteData | FileAccess::kFileAppendData))) return X_STATUS_ACCESS_DENIED;
    entry = replacement;
    parent_entry = replacement->parent();
  }
  if (entry) {
    if (entry->attributes() & kFileAttributeDirectory && is_non_directory) {]=])
fm4_mod_publish(rexfilesystem vfs virtual_file_system.cpp)

fm4_mod_source(rexruntime "system/runtime[.]cpp|^runtime[.]cpp" runtime)
fm4_mod_replace(runtime "#include <rex/runtime.h>" "#include <rex/runtime.h>\n#include \"mod_runtime.h\"")
fm4_mod_replace(runtime "mount_path, abs_game_root, !REXCVAR_GET(allow_game_relative_writes)"
  "mount_path, abs_game_root, fm4::mods::Active() || !REXCVAR_GET(allow_game_relative_writes)")
fm4_mod_publish(rexruntime runtime runtime.cpp)

# The SDK enables all CPUs when configuring guest affinities. A mod session
# must retain the launcher's smaller process budget on this shared machine.
fm4_mod_source(rexcore "threading_win[.]cpp" threading)
fm4_mod_replace(threading "  SetProcessAffinityMask(process_handle, system_affinity_mask);"
  [=[  char mod_session[2] = {};
  if (GetEnvironmentVariableA("FM4_MODS", mod_session, sizeof(mod_session)) == 1 && mod_session[0] == '1') {
    system_affinity_mask &= process_affinity_mask & DWORD_PTR(0x0FFF);
  }
  SetProcessAffinityMask(process_handle, system_affinity_mask);]=])
fm4_mod_publish(rexcore threading threading_win.cpp)

# Mod menu input is neutralized through the same SDK input path as live settings.
fm4_mod_source(rexinput "input_system[.]cpp" input)
fm4_mod_replace(input "#include <rex/input/input_system.h>" "#include <rex/input/input_system.h>\n#include \"mod_runtime.h\"")
fm4_mod_replace(input "*out_state = merged;" "if (fm4::mods::Capturing()) merged.gamepad = {};\n    *out_state = merged;")
# Insert into the keystroke function by its signature, preserving other overlays.
fm4_mod_replace(input [=[                                   X_INPUT_KEYSTROKE* out_keystroke) {]=]
  [=[                                   X_INPUT_KEYSTROKE* out_keystroke) {
  if (fm4::mods::Capturing()) return X_ERROR_EMPTY;]=])
fm4_mod_publish(rexinput input input_system.cpp)
