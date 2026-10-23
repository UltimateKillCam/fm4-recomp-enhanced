// Runtime launch-receipt verifier. Modified 2026-10-23.
#pragma once
#include "native_protected_verifier.hpp"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <optional>

namespace fm4_plus_proposal {
using Json=nlohmann::json;
inline std::filesystem::path JsonPath(const Json& value){
  const auto text=value.get<std::string>();Require(!text.empty() && text.size()<32768,"Receipt path length invalid");
  const int needed=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
  Require(needed>0,"Receipt path is not valid UTF8");std::wstring wide(size_t(needed),L'\0');
  Require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),wide.data(),needed)==needed,"Receipt path conversion failed");return std::filesystem::path(wide);
}
inline Json ReadJson(const std::filesystem::path& path) {
  auto file=OpenStable(path);const auto size=FileSize(file.get());Require(size>0 && size<=(64u<<20),"Receipt size invalid");
  std::vector<uint8_t> bytes(size_t(size),uint8_t{});ReadExact(file.get(),bytes);return Json::parse(bytes);
}
inline bool SamePath(const std::filesystem::path& a,const std::filesystem::path& b) {
  const auto x=a.lexically_normal().wstring(),y=b.lexically_normal().wstring();
  return CompareStringOrdinal(x.data(),int(x.size()),y.data(),int(y.size()),TRUE)==CSTR_EQUAL;
}
inline void VerifyManifest(const std::filesystem::path& root,const Json& description) {
  Require(description.at("kind")=="directory","Receipt directory kind mismatch");
  const auto& wanted=description.at("files");Require(wanted.is_object(),"Receipt file manifest malformed");size_t count=0;
  RejectReparse(root);
  // Walk the extended-length form so deep content paths stay reachable; report paths relative to it.
  const std::filesystem::path long_root(LongPath(root));
  for(const auto& item:std::filesystem::recursive_directory_iterator(long_root)) {
    RejectReparse(item.path());if(!item.is_regular_file())continue;
    const auto relative=item.path().lexically_relative(long_root).generic_u8string();
    const std::string key(reinterpret_cast<const char*>(relative.data()),relative.size());
    Require(wanted.contains(key),"Unreceipted prepared file");++count;
    auto file=OpenStable(item.path());const auto& entry=wanted.at(key);
    Require(FileSize(file.get())==entry.at("size").get<uint64_t>() && Hex(HashFile(file.get()))==entry.at("sha256").get<std::string>(),"Prepared file differs from complete receipt");
  }
  Require(count==wanted.size(),"Prepared receipt file missing");
}
class NativeSession {
  bool initialized_=false,enabled_=false;
  std::filesystem::path generation_,game_,user_;
  Json receipt_;
  std::optional<VerifiedResource> proof_;
 public:
  bool enabled()const{return enabled_;}
  const std::filesystem::path& game()const{return game_;}
  const std::filesystem::path& user()const{return user_;}
  // Called during path setup, before presentation/runtime construction. The
  // launcher supplies an immutable generation directory, not a mod XEX path.
  void Configure(const std::filesystem::path& generation,const std::filesystem::path& expected_edition) {
    if(initialized_){Require(SamePath(generation_,generation) && (generation.empty() || enabled_),"PFP mode cannot change/retry a failed load inside an existing process");return;}
    initialized_=true;generation_=generation;
    if(generation.empty())return; // Off ignores all remembered mod paths.
    RejectReparse(generation);receipt_=ReadJson(generation/L"receipt.json");
    Require(receipt_.at("schema")=="fm4-plus-receipt/1" && receipt_.at("options").at("enabled")==true,"PFP receipt protocol mismatch");
    const auto id=receipt_.at("generation_id").get<std::string>();
    Require(id.size()==32 && id.find_first_not_of("0123456789abcdef")==std::string::npos,"PFP generation ID invalid");
    const auto store=JsonPath(receipt_.at("store"));const auto edition=JsonPath(receipt_.at("edition_root"));
    Require(!expected_edition.empty() && SamePath(edition,expected_edition),"PFP receipt belongs to a different launcher edition root");
    Require(SamePath(generation,store/L"versions"/id),"PFP generation outside owned store");
    const auto owner=ReadJson(generation/L"owner.json");
    Require(owner.at("protocol")=="fm4-plus-generation/1" && owner.at("generation")==id && SamePath(JsonPath(owner.at("store")),store),"PFP generation ownership invalid");
    const auto mode=receipt_.at("options").at("mode").get<std::string>();
    Require(mode=="normal" || mode=="hard" || mode=="sandbox" || mode=="vanilla","PFP mode invalid");
    game_=generation/L"game";user_=edition/L".local/user/.fm4-plus"/mode;
    RejectReparse(user_);Require(!std::filesystem::exists(user_/L".fm4-plus-pending.json"),"PFP publication recovery pending");
    const auto active=ReadJson(store/L"active.json");
    Require(active.at("protocol")=="fm4-plus-publication/1" && active.at("generation")==id && SamePath(JsonPath(active.at("user")),user_),"PFP active receipt differs");
    const auto store_owner=ReadJson(store/L".fm4-plus-store.json");
    Require(store_owner.at("protocol")=="fm4-plus-store/1" && SamePath(JsonPath(store_owner.at("edition_root")),edition) && SamePath(JsonPath(store_owner.at("store")),store),"PFP store ownership invalid");
    VerifyManifest(game_,receipt_.at("game_manifest"));VerifyManifest(generation/L"content",receipt_.at("content_manifest"));
    // The runtime's marketplace path is the separate per-mode user root.
    // Verify each selected whole unit plus exact genuine header, not merely
    // the generation's unused staging content copy.
    const auto content_owner=ReadJson(user_/L".fm4-plus-content.json");
    Require(content_owner.at("protocol")=="fm4-plus-publication/1" && content_owner.at("generation")==id && SamePath(JsonPath(content_owner.at("user")),user_) && SamePath(JsonPath(content_owner.at("store")),store),"PFP content ownership invalid");
    Require(content_owner.at("units").size()==receipt_.at("content_units").size(),"PFP content unit count differs");
    for(const auto& package:receipt_.at("packages")) {
      if(package.at("registration_kind")=="local_mod_directory") {
        Require(receipt_.at("options").value("allow_headerless_local_mod_packages",false) && package.at("package_license_mask")==0,"Headerless local-mod option/provenance invalid");
        const auto name=package.at("package").get<std::string>();
        Require((name.size()==40 || name.size()==42) && name.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos,"Local mod package identity invalid");
        // The launcher writes a standard license-free header so FM4 mounts the package (2026-10-23).
      }
    }
    for(const auto& entry:receipt_.at("content_units")) {
      const auto name=entry.get<std::string>();const auto relative=Relative(name);
      Require(name.starts_with("0000000000000000/4D530910/00000002/") || name.starts_with("0000000000000000/4D530910/Headers/00000002/"),"PFP unit outside marketplace data");
      const auto& wanted=content_owner.at("units").at(name);const auto path=user_/relative;
      if(wanted.at("kind")=="directory")VerifyManifest(path,wanted);
      else {
        auto file=OpenStable(path);Require(FileSize(file.get())==wanted.at("size").get<uint64_t>() && Hex(HashFile(file.get()))==wanted.at("sha256").get<std::string>(),"PFP native content header differs");
      }
    }
    // Cross-check stable user's individual content files against the immutable
    // generation receipt too, so an edited content-owner marker cannot redefine
    // the expected package/header hashes.
    for(auto item=receipt_.at("content_manifest").at("files").begin();item!=receipt_.at("content_manifest").at("files").end();++item) {
      auto file=OpenStable(user_/Relative(item.key()));Require(FileSize(file.get())==item.value().at("size").get<uint64_t>() && Hex(HashFile(file.get()))==item.value().at("sha256").get<std::string>(),"PFP published content differs from generation");
    }
    enabled_=true;
  }
  const VerifiedResource& VerifyLoadedOriginal(std::span<const uint8_t> original_table) {
    Require(enabled_ && !proof_.has_value(),"PFP resource lifecycle invalid");
    proof_.emplace(VerifyProtectedFiles(game_,generation_/L"protected-resource.bin",original_table));return *proof_;
  }
};
// One bounded process-lifetime owner retains all protected input handles until
// process teardown. ReXApp OnShutdown is not a full guest-drain guarantee.
inline NativeSession& Session(){static NativeSession* value=new NativeSession;return *value;}
} // namespace fm4_plus_proposal
