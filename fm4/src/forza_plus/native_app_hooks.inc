// Included by the proposed app C++ only under FM4_ENABLE_FORZA_PLUS_NATIVE.
// The launcher clears these env keys for stock launches; a child-only block is
// required so failed/previous requests cannot contaminate a later stock launch.
#include "native_hook_adapter.hpp"
#include "native_session.hpp"
#include "native_edition_gate.hpp"
#include <rex/runtime.h>
#include <fstream>
#include <rex/system/kernel_state.h>
#ifndef FM4_PFP_APP_CLASS
#define FM4_PFP_APP_CLASS Fm4App
#endif
namespace fm4_plus_proposal {
inline std::filesystem::path EnvironmentPath(const wchar_t* name) {
  const DWORD needed=GetEnvironmentVariableW(name,nullptr,0);if(!needed)return {};
  Require(needed<32768,"PFP environment path too long");std::wstring value(needed,L'\0');
  const DWORD copied=GetEnvironmentVariableW(name,value.data(),needed);
  Require(copied && copied<needed,"PFP environment path changed during read");value.resize(copied);return std::filesystem::path(value);
}
inline std::filesystem::path EditionRootForLease() {
  auto edition=EnvironmentPath(L"FM4_EDITION_ROOT");if(!edition.empty())return edition;
  // Preserve direct stock starts from a normal project/release tree too.
  // Installed launchers should always provide the explicit edition root.
  std::wstring image(32768,L'\0');DWORD length=GetModuleFileNameW(nullptr,image.data(),DWORD(image.size()));
  Require(length && length<image.size(),"Cannot identify current executable");image.resize(length);
  for(auto directory=std::filesystem::path(image).parent_path();!directory.empty();) {
    if(std::filesystem::is_regular_file(directory/L"project-manifest.toml"))return directory;
    auto parent=directory.parent_path();if(parent==directory)break;directory=parent;
  }
  throw std::runtime_error("Native edition lease needs launcher FM4_EDITION_ROOT or a project manifest ancestor");
}
// Startup failures can occur before the runtime logger exists. Record them where the
// launcher and user can find them: <edition>/.local/forza-plus/last-launch-error.txt,
// falling back to the executable folder.
inline void ReportLaunchFailure(const char* stage,const std::exception& error) noexcept {
  try {
    REXLOG_ERROR("{}: {}",stage,error.what());
    std::filesystem::path folder;
    try{folder=EditionRootForLease()/L".local"/L"forza-plus";}catch(...){}
    if(folder.empty()){
      std::wstring image(32768,L'\0');const DWORD length=GetModuleFileNameW(nullptr,image.data(),DWORD(image.size()));
      if(!length || length>=image.size())return;image.resize(length);folder=std::filesystem::path(image).parent_path();
    }
    std::error_code ignored;std::filesystem::create_directories(folder,ignored);
    std::ofstream output(folder/L"last-launch-error.txt",std::ios::binary|std::ios::app);
    output<<stage<<": "<<error.what()<<"\n";
    const std::string message=std::string("FM4 Forza Plus: ")+stage+": "+error.what()+"\n";
    OutputDebugStringA(message.c_str());
  } catch(...) {}
}
inline bool& ConfigureFailed(){static bool failed=false;return failed;}
}
extern "C" __declspec(dllexport) uint32_t Fm4ForzaPlusNativeProtocol(){return 1;}
extern "C" __declspec(dllexport) uint32_t Fm4ForzaPlusNativeProtocolV1(){return 1;}

bool FM4_PFP_APP_CLASS::SetupEnvironment() {
  try {
    const auto edition=fm4_plus_proposal::EditionRootForLease();
    fm4_plus_proposal::GameLease().Acquire(edition,
        fm4_plus_proposal::EnvironmentPath(L"FM4_LAUNCH_READY_EVENT").wstring(),
        fm4_plus_proposal::EnvironmentPath(L"FM4_LAUNCH_CANCEL_EVENT").wstring());
    const bool success=rex::ReXApp::SetupEnvironment();fm4_plus_proposal::GameLease().CheckCancellation();return success;
  }
  catch(const std::exception& error){fm4_plus_proposal::ReportLaunchFailure("Forza Plus edition lease",error);return false;}
}
void FM4_PFP_APP_CLASS::OnConfigurePaths(rex::PathConfig& paths) {
  try {
  auto& session=fm4_plus_proposal::Session();
  const auto generation=fm4_plus_proposal::EnvironmentPath(L"FM4_FORZA_PLUS_GENERATION");
  const auto edition=fm4_plus_proposal::EditionRootForLease();
  const auto claimed=fm4_plus_proposal::EnvironmentPath(L"FM4_FORZA_PLUS_EDITION_ROOT");
  if(!generation.empty() && !claimed.empty())fm4_plus_proposal::Require(
      fm4_plus_proposal::SamePath(claimed,edition),"Forza Plus edition differs from game lease identity");
  session.Configure(generation,edition);
  if(!session.enabled())return;
  paths.game_data_root=session.game();paths.user_data_root=session.user();
  paths.update_data_root.clear(); // Recognized mod update DATA already overlaid.
  paths.cache_root=session.user()/L"cache";paths.metadata_root=session.user()/L"metadata";
  } catch(const std::exception& error) {
    // OnConfigurePaths cannot fail directly; ConstructRuntime refuses to start the guest.
    fm4_plus_proposal::ReportLaunchFailure("Forza Plus generation verification",error);
    fm4_plus_proposal::ConfigureFailed()=true;
  }
}
bool FM4_PFP_APP_CLASS::ConstructRuntime(const rex::PathConfig& paths) {
  try {
    auto& session=fm4_plus_proposal::Session();
    fm4_plus_proposal::Require(!fm4_plus_proposal::ConfigureFailed(),"Forza Plus generation verification failed; see last-launch-error.txt");
    fm4_plus_proposal::GameLease().CheckCancellation();
    if(session.enabled())fm4_plus_proposal::Require(
        fm4_plus_proposal::SamePath(paths.game_data_root,session.game()) &&
        fm4_plus_proposal::SamePath(paths.user_data_root,session.user()),"PFP paths changed after receipt verification");
    return rex::ReXApp::ConstructRuntime(paths);
  }catch(const std::exception& error){fm4_plus_proposal::ReportLaunchFailure("Forza Plus native initialization",error);return false;}
}
void FM4_PFP_APP_CLASS::OnPostLoadXexImage() {
  fm4_plus_proposal::GameLease().CheckCancellation();
  auto& session=fm4_plus_proposal::Session();if(!session.enabled())return;
  fm4_plus_proposal::Require(runtime() && runtime()->kernel_state(),"PFP runtime unavailable");
  auto module=runtime()->kernel_state()->GetExecutableModule();
  fm4_plus_proposal::Require(bool(module),"PFP executable module unavailable");
  auto* memory=runtime()->memory();
  const auto& proof=session.VerifyLoadedOriginal(std::span<const uint8_t>(memory->TranslateVirtual(0x82EA0000),0x5771));
  const auto allocation=fm4_plus_proposal::InstallResourceBeforeGuestStart(*module,*memory,proof);
  REXLOG_INFO("PFP protected-file resource installed before guest start: address={:08X} bytes={}",allocation,proof.bytes().size());
}
#undef FM4_PFP_APP_CLASS
