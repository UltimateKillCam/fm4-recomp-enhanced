// CPU-only native resource export from the user's original XEX through the
// existing SDK loader. No bundled decrypted data, crypto key or GPU backend.
#include "native_protected_verifier.hpp"
#include <rex/runtime.h>
#include <rex/kernel/init.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/logging.h>
#include <iostream>

extern "C" __declspec(dllexport) uint32_t Fm4ForzaPlusResourceExportProtocolV1(){return 1;}
int wmain(int argc,wchar_t** argv) {
  using namespace fm4_plus_proposal;
  try {
    Require(argc==3,"usage: fm4-pfp-resource-export <edition-root> <original-game-root>");
    const auto edition=std::filesystem::absolute(argv[1]).lexically_normal();
    const auto game=std::filesystem::absolute(argv[2]).lexically_normal();RejectReparse(edition);RejectReparse(game);
    auto original_xex=OpenStable(game/L"default.xex");
    Require(Hex(HashFile(original_xex.get()))=="b34dfb88978819cccda6dafd755e7139970a58fbf6707dfbcba06846857f52bd","Export requires the original native recompilation XEX");
    const auto cache=edition/L".local/forza-plus/resource-export";
    const auto cache_name=cache.wstring(),game_name=game.wstring();
    auto within=[](const std::wstring& child,const std::wstring& parent){return child.size()>=parent.size() && CompareStringOrdinal(child.data(),int(parent.size()),parent.data(),int(parent.size()),TRUE)==CSTR_EQUAL && (child.size()==parent.size() || child[parent.size()]==L'\\');};
    Require(!within(cache_name,game_name) && !within(game_name,cache_name),"Private export cache overlaps original game input");
    auto existing=cache;while(!std::filesystem::exists(existing))existing=existing.parent_path();RejectReparse(existing);
    std::filesystem::create_directories(cache);RejectReparse(cache);
    const auto output=cache/L"native-original-resource.bin";
    if(std::filesystem::exists(output)) {
      auto file=OpenStable(output);Require(Hex(HashFile(file.get()))=="9948f4053e2ebf91b3c998cb5dd49d5658f50cb9631a354ea86e2d4c5a512c98","Existing resource cache is not the pinned original");
      std::cout<<"resource_export=verified_existing_cache\n";return 0;
    }
    const auto scratch=cache/(L"work-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    Require(std::filesystem::create_directory(scratch),"Cannot create isolated CPU export scratch");
    rex::InitLoggingEarly();
    {
      // This exact tool-mode loader sequence is already used by the SDK's
      // project_recompiler. No graphics/audio/input factories or SDL context.
      rex::Runtime runtime(game,scratch);
      rex::RuntimeConfig config;config.kernel_init=rex::kernel::InitializeKernel;config.tool_mode=true;
      Require(runtime.Setup(std::move(config))==rex::X_STATUS(0),"CPU XEX runtime setup failed");
      Require(runtime.is_tool_mode(),"Resource export must remain CPU tool mode");
      Require(runtime.LoadXexImage("game:\\default.xex")==rex::X_STATUS(0),"Original XEX CPU load failed");
      auto module=runtime.kernel_state()->GetExecutableModule();Require(bool(module) && module->entry_point()==0x8275F9F0,"Original module identity differs");
      rex::xex2_opt_resource_info* directory=nullptr;
      Require(module->GetOptHeader(rex::XEX_HEADER_RESOURCE_INFO,&directory)==rex::X_STATUS(0) && directory,"Missing original resource directory");
      bool found=false;
      for(uint32_t i=0;i<(directory->size-4)/sizeof(rex::xex2_resource);++i) {
        const auto& entry=directory->resources[i];if(std::memcmp(entry.name,"0D163575",8)!=0)continue;
        Require(!found && entry.address==0x82EA0000 && entry.size==0x5771,"Original resource descriptor differs");found=true;
      }
      Require(found,"Original protected resource missing");
      const auto bytes=std::span<const uint8_t>(runtime.memory()->TranslateVirtual(0x82EA0000),0x5771);
      Require(Hex(Sha(bytes))=="9948f4053e2ebf91b3c998cb5dd49d5658f50cb9631a354ea86e2d4c5a512c98","Loaded original resource hash differs");
      const auto temporary=scratch/L"native-original-resource.bin";
      OwnedHandle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
      Require(file.get()!=INVALID_HANDLE_VALUE,"Cannot create private export output");DWORD written=0;
      Require(WriteFile(file.get(),bytes.data(),DWORD(bytes.size()),&written,nullptr) && written==bytes.size() && FlushFileBuffers(file.get()),"Cannot write complete private resource export");file.reset();
      Require(MoveFileExW(temporary.c_str(),output.c_str(),MOVEFILE_WRITE_THROUGH)!=0,"Cannot publish private native resource cache");
      // No PrepareModuleLaunch/LaunchModule/resume or temporal/render calls.
    }
    std::cout<<"resource_export=cpu_tool_mode entries=66 bytes=22385\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 2;}
}
