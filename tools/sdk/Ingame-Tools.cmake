# Optional local in-game tools. GPL-3.0-only.
if(NOT FM4_LIVE_SETTINGS)
  message(FATAL_ERROR "In-game tools require FM4_LIVE_SETTINGS=ON")
endif()
target_sources(rexruntime PRIVATE "${FM4_CONTROLLER_BRIDGE_DIR}/ingame_store.cpp"
  "${FM4_CONTROLLER_BRIDGE_DIR}/ingame_tools.cpp")
set_property(SOURCE "${FM4_CONTROLLER_BRIDGE_DIR}/live_settings.cpp" TARGET_DIRECTORY rexruntime APPEND PROPERTY
  COMPILE_DEFINITIONS FM4_INGAME_TOOLS=1)
foreach(source ingame_store.cpp ingame_tools.cpp)
  set_property(SOURCE "${FM4_CONTROLLER_BRIDGE_DIR}/${source}" TARGET_DIRECTORY rexruntime APPEND PROPERTY
    INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/thirdparty/inja/third_party/include")
endforeach()
target_link_libraries(rexruntime PRIVATE windowscodecs ole32 shell32 bcrypt)

function(fm4_tools_guard variable pattern)
  string(REGEX MATCHALL "${pattern}" matches "${${variable}}")
  list(LENGTH matches count)
  if(NOT count EQUAL 1)
    message(FATAL_ERROR "In-game tools: missing or ambiguous I/O entry ${pattern}")
  endif()
  list(GET matches 0 anchor)
  string(REPLACE "${anchor}" "${anchor}\n  fm4::tools::GuestIo fm4_tools_io;" result "${${variable}}")
  set(${variable} "${result}" PARENT_SCOPE)
endfunction()

fm4_live_source(rexruntime "system/kernel_state[.]cpp|^kernel_state[.]cpp" kernel)
fm4_live_replace(kernel "#include <rex/system/kernel_state.h>"
  "#include <rex/system/kernel_state.h>\n#include \"ingame_tools.h\"")
fm4_live_replace(kernel "  LoadAchievementsData();" "  LoadAchievementsData();\n  fm4::tools::Start(this);")
fm4_live_publish(rexruntime kernel kernel_state.cpp)

fm4_live_source(rexruntime "system/xfile[.]cpp|^xfile[.]cpp" file)
fm4_live_replace(file "#include <rex/system/xfile.h>"
  "#include <rex/system/xfile.h>\n#include \"ingame_tools.h\"")
fm4_live_replace(file [=[  if (XSUCCEEDED(result)) {
    position_ += bytes_written;
  }]=] [=[  if (XSUCCEEDED(result)) {
    position_ += bytes_written;
    fm4::tools::Written(entry());
  }]=])
foreach(method Write SetLength Rename)
  fm4_tools_guard(file "X_STATUS XFile::${method}\\([^{}]*\\) \\{")
endforeach()
fm4_live_replace(file "XFile::~XFile() {" "XFile::~XFile() {\n  fm4::tools::GuestIo fm4_tools_io;")
fm4_live_publish(rexruntime file xfile.cpp)

fm4_live_source(rexruntime "content_manager[.]cpp" content)
fm4_live_replace(content "#include <rex/system/xam/content_manager.h>"
  "#include <rex/system/xam/content_manager.h>\n#include \"ingame_tools.h\"")
fm4_live_replace(content "  content_data_ = data;"
  "  content_data_ = data;\n  fm4::tools::Package(root_name_, package_path_, false);")
fm4_live_replace(content "ContentPackage::~ContentPackage() {"
  "ContentPackage::~ContentPackage() {\n  fm4::tools::Package(root_name_, package_path_, true);")
foreach(method CreateContent OpenContent CloseContent SetContentThumbnail DeleteContent
    UnmountAndDeleteContent WriteContentHeaderFile)
  fm4_tools_guard(content "X_RESULT ContentManager::${method}\\([^{}]*\\) \\{")
endforeach()
fm4_live_publish(rexruntime content content_manager.cpp)

fm4_live_source(rexruntime "xboxkrnl_io[.]cpp" io)
fm4_live_replace(io "#include <rex/system/xfile.h>"
  "#include <rex/system/xfile.h>\n#include \"ingame_tools.h\"")
fm4_tools_guard(io "u32 NtCreateFile_entry\\([^{}]*\\) \\{")
fm4_tools_guard(io "u32 NtWriteFile_entry\\([^{}]*\\) \\{")
fm4_live_publish(rexruntime io xboxkrnl_io.cpp)
fm4_live_source(rexruntime "xboxkrnl_io_info[.]cpp" info)
fm4_live_replace(info "#include <rex/system/xfile.h>"
  "#include <rex/system/xfile.h>\n#include \"ingame_tools.h\"")
fm4_tools_guard(info "u32 NtSetInformationFile_entry\\([^{}]*\\) \\{")
fm4_live_publish(rexruntime info xboxkrnl_io_info.cpp)

fm4_live_source(rexruntime "xam/apps/xgi_app[.]cpp" xgi)
fm4_live_replace(xgi "#include <rex/kernel/xam/apps/xgi_app.h>"
  "#include <rex/kernel/xam/apps/xgi_app.h>\n#include \"ingame_tools.h\"\nREXCVAR_DECLARE(bool, fm4_local_achievements);")
fm4_live_replace(xgi [=[      uint32_t raw0 = buffer_length >= 4 ? memory::load_and_swap<uint32_t>(buffer + 0) : 0;
      uint32_t raw4 = buffer_length >= 8 ? memory::load_and_swap<uint32_t>(buffer + 4) : 0;]=]
  [=[      const bool local = REXCVAR_GET(fm4_local_achievements) && kernel_state_->title_id() == 0x4D530910;
      if (local && ((buffer_length && buffer_length != 8) || !buffer_ptr ||
          buffer_ptr > UINT32_MAX - 7 || !memory_->LookupHeap(buffer_ptr) ||
          !memory_->LookupHeap(buffer_ptr + 7))) return X_E_FAIL;
      uint32_t raw0 = (buffer_length >= 4 || local) ? memory::load_and_swap<uint32_t>(buffer + 0) : 0;
      uint32_t raw4 = (buffer_length >= 8 || local) ? memory::load_and_swap<uint32_t>(buffer + 4) : 0;]=])
fm4_live_replace(xgi "        uint32_t span_end = achievements_ptr + achievement_count * kEntryStride - 1;"
  [=[        if (local && achievements_ptr > UINT32_MAX - achievement_count * kEntryStride) return X_E_FAIL;
        uint32_t span_end = achievements_ptr + achievement_count * kEntryStride - 1;]=])
fm4_live_replace(xgi "          kernel_state_->UnlockAchievement(id);" [=[          if (REXCVAR_GET(fm4_local_achievements) && kernel_state_->title_id() == 0x4D530910) {
            const uint32_t user = memory::load_and_swap<uint32_t>(base + i * kEntryStride);
            fm4::tools::Achievement(kernel_state_, user, id);
          } else {
            kernel_state_->UnlockAchievement(id);
          }]=])
fm4_live_publish(rexruntime xgi xgi_app.cpp)

fm4_live_source(rexruntime "xam/xam_user[.]cpp" user)
fm4_live_replace(user "#include <rex/system/kernel_state.h>"
  "#include <rex/system/kernel_state.h>\n#include \"ingame_tools.h\"\nREXCVAR_DECLARE(bool, fm4_local_achievements);")
fm4_live_replace(user "  auto fill_unlock = [](" "  auto fill_unlock = [user_index](")
fm4_live_replace(user "    uint64_t ft = ks->GetAchievementUnlockTime(id);" [=[    uint64_t ft = (REXCVAR_GET(fm4_local_achievements) && ks->title_id() == 0x4D530910)
        ? fm4::tools::AchievementTime(user_index, id) : ks->GetAchievementUnlockTime(id);]=])
fm4_live_publish(rexruntime user xam_user.cpp)
