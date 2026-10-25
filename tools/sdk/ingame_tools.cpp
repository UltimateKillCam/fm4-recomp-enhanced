// Optional in-game tools. GPL-3.0-only.
#include "ingame_tools.h"
#include "ingame_store.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/filesystem/devices/host_path_entry.h>
#include <imgui.h>
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

REXCVAR_DEFINE_BOOL(fm4_photo_export, false, "FM4 tools", "Export FM4 saved photo payloads to the PC")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(fm4_photo_directory, "", "FM4 tools", "Photo folder; empty uses Pictures/FM4 Enhanced")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fm4_save_backups, false, "FM4 tools", "Back up profile/title saves before launch and while idle")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fm4_backup_count, 8, "FM4 tools", "Active save backup generations").range(2,32);
REXCVAR_DEFINE_INT32(fm4_backup_minutes, 10, "FM4 tools", "Idle save backup interval in minutes").range(1,120);
REXCVAR_DEFINE_BOOL(fm4_trainer, false, "FM4 tools", "Allow the separately armed trainer page")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fm4_local_achievements, false, "FM4 tools", "Track FM4 guest achievements per local profile")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace fm4::tools {
namespace {
using Clock=std::chrono::steady_clock;
std::mutex mutex;
std::condition_variable wake;
std::unique_ptr<Store> store;
std::thread worker;
bool stopping=false, snapshot=false, export_all=false, dirty=false;
Clock::time_point last_write{}, last_backup{};
std::set<fs::path> packages, remember;
std::deque<std::pair<uint32_t,uint64_t>> unlocks;
std::vector<rex::system::AchievementInfo> metadata;
Json achieved=Json::object();
std::vector<std::string> backups;
std::deque<std::string> toasts;
std::string status="Tools are disabled in this session.";
std::string restore;
std::string diagnostic_tab;
std::mutex diagnostic_mutex;
bool diagnostic_has_state=false, diagnostic_visible=false, diagnostic_armed=false;
double diagnostic_speed=1;
int credits=100000;
std::atomic<bool> trainer_active{false};
std::atomic<double> trainer_speed{1};
std::atomic<int64_t> pending_credits{-1};
HANDLE session=INVALID_HANDLE_VALUE;
std::string Label(uint32_t id) {
  for(auto& a:metadata) if(a.id==id) return a.label;
  return "Achievement "+std::to_string(id);
}
fs::path PhotoDirectory() {
  auto value=REXCVAR_GET(fm4_photo_directory); if(!value.empty()) return fs::path(value);
  PWSTR p=nullptr; fs::path result;
  if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures,0,nullptr,&p))) { result=p;CoTaskMemFree(p); }
  if(result.empty()) throw std::runtime_error("Pictures folder is unavailable; set fm4_photo_directory");
  return result/"FM4 Enhanced";
}
bool OtherGameRunning() {
  HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
  if(snapshot==INVALID_HANDLE_VALUE)return true;
  PROCESSENTRY32W e{};e.dwSize=sizeof(e);bool found=false;
  if(Process32FirstW(snapshot,&e))do {
    if(e.th32ProcessID!=GetCurrentProcessId() && _wcsicmp(e.szExeFile,L"fm4.exe")==0)found=true;
  }while(Process32NextW(snapshot,&e));
  CloseHandle(snapshot);return found;
}
void SetStatus(const std::string& text) { std::lock_guard lock(mutex);status=text;REXLOG_INFO("FM4 tools: {}",text); }
void Work() {
  SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
  auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
  while(true) {
    bool fixture_visible=false;PollDiagnostic(fixture_visible);
    bool backup=false, all=false;
    std::set<fs::path> photos, registrations;
    std::deque<std::pair<uint32_t,uint64_t>> events;
    {
      std::unique_lock lock(mutex);
      wake.wait_for(lock,std::chrono::seconds(1));
      if(stopping && unlocks.empty() && packages.empty() && remember.empty()) break;
      auto now=Clock::now();
      bool quiet=stopping || now-last_write>std::chrono::seconds(5);
      backup=!stopping && REXCVAR_GET(fm4_save_backups) && quiet && (snapshot || dirty ||
          now-last_backup>std::chrono::minutes(REXCVAR_GET(fm4_backup_minutes)));
      if(backup) { snapshot=false; dirty=false;last_backup=now; }
      all=export_all; export_all=false;
      if(quiet) { photos.swap(packages);registrations.swap(remember); }
      events.swap(unlocks);
    }
    try {
      if(backup) {
        store->keep=REXCVAR_GET(fm4_backup_count);
        std::string name;
        {auto lease=io::TrySnapshot();
          if(!lease.owns_lock())throw std::runtime_error("Guest file operation in progress; snapshot deferred");
          name=store->Backup();
        }
        { std::lock_guard lock(mutex);backups=store->Backups(); }
        SetStatus(name.empty()?"No save data exists yet.":"Save backup created: "+name);
      }
    } catch(const std::exception& e) { SetStatus(e.what()); }
    try {
      if(REXCVAR_GET(fm4_photo_export) && (all || !photos.empty() || !registrations.empty())) {
        for(auto& p:registrations) store->RememberPhotoPackage(p);
        if(all) for(auto& p:store->PhotoPackages()) photos.insert(p);
        unsigned count=0;for(auto& p:photos) count+=store->ExportPhotos(p);
        if(all || !photos.empty()) SetStatus("Exported "+std::to_string(count)+" new saved photos. Unsupported encodings are retained in the save.");
      }
    } catch(const std::exception& e) {
      SetStatus(e.what());std::lock_guard lock(mutex);
      if(!stopping) {packages.insert(photos.begin(),photos.end());remember.insert(registrations.begin(),registrations.end());}
    }
    for(auto [id,time]:events) {
      try {
        bool added=store->Unlock(id,time);
        auto persisted=store->Unlocks();std::lock_guard lock(mutex);
        for(auto& [key,value]:persisted.items())achieved[key]=value;
        if(added) { if(toasts.size()<32) toasts.push_back(Label(id));
          REXLOG_INFO("FM4 tools achievement: profile={} id={} persisted",store->profile,id); }
      } catch(const std::exception& e) {
        SetStatus(e.what());std::lock_guard lock(mutex);if(!stopping)unlocks.emplace_back(id,time);
      }
    }
  }
  if(SUCCEEDED(com)) CoUninitialize();
}
}
GuestIo::GuestIo() : io::Reader(REXCVAR_GET(fm4_save_backups)||REXCVAR_GET(fm4_photo_export)) {}
void Start(rex::system::KernelState* kernel) {
  if(kernel->title_id()!=0x4D530910 || !(REXCVAR_GET(fm4_photo_export)||REXCVAR_GET(fm4_save_backups)||
      REXCVAR_GET(fm4_trainer)||REXCVAR_GET(fm4_local_achievements))) return;
  Stop();
  bool pending_restore=false;
  try {
    auto profile=fmt::format("{:016X}",kernel->user_profile()->xuid());
    auto root=kernel->emulator()->user_data_root();
    auto saves=root/profile/"4D530910";
    auto state=root/".fm4-tools"/profile;
    pending_restore=REXCVAR_GET(fm4_save_backups) &&
        (fs::exists(state/"restore.json")||fs::exists(state/"restore-journal.json"));
    fs::create_directories(state);
    session=CreateFileW((state/"session.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,
                        OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(session==INVALID_HANDLE_VALUE) throw std::runtime_error("This profile already has an in-game tools session");
    auto next=std::make_unique<Store>(saves,state,REXCVAR_GET(fm4_photo_export)?PhotoDirectory():
        root/".fm4-tools-unused-photos",root/".fm4-tools-archive",profile,REXCVAR_GET(fm4_backup_count));
    if(REXCVAR_GET(fm4_save_backups)) {
      if(pending_restore && OtherGameRunning())
        throw std::runtime_error("Close the other FM4 process before restoring this profile");
      if(next->RestoreAtLaunch()) REXLOG_INFO("FM4 tools: verified restore applied before guest launch");
      pending_restore=false;
      try {next->Backup();}catch(const std::exception& e) {REXLOG_WARN("FM4 launch backup deferred: {}",e.what());}
    }
    { std::lock_guard lock(mutex);
      store=std::move(next);
      achieved=REXCVAR_GET(fm4_local_achievements)?store->Unlocks():Json::object();
      metadata=kernel->loaded_achievements();backups=store->Backups();
      stopping=false;export_all=REXCVAR_GET(fm4_photo_export);
      last_write=last_backup=Clock::now();status="In-game tools ready for profile "+profile;
    }
    worker=std::thread(Work);
    REXLOG_INFO("FM4 tools: session ready profile={} saves={}",profile,saves.string());
  } catch(const std::exception& e) {SetStatus(e.what());store.reset();
    if(session!=INVALID_HANDLE_VALUE) {CloseHandle(session);session=INVALID_HANDLE_VALUE;}
    if(pending_restore)throw;
  }
}
void Stop() {
  TrainerClosed();
  pending_credits.store(-1);
  {std::lock_guard lock(mutex);stopping=true;} wake.notify_all();
  if(worker.joinable()) worker.join();
  if(session!=INVALID_HANDLE_VALUE) {CloseHandle(session);session=INVALID_HANDLE_VALUE;}
  std::lock_guard lock(mutex);store.reset();packages.clear();remember.clear();unlocks.clear();toasts.clear();
  metadata.clear();achieved=Json::object();backups.clear();snapshot=export_all=dirty=false;
}
void Package(std::string_view name,const fs::path& path,bool closed) {
  if(!REXCVAR_GET(fm4_photo_export)) return;
  std::string lower(name);std::transform(lower.begin(),lower.end(),lower.begin(),::tolower);
  if(lower!="photo" && lower!="photo:") return;
  std::lock_guard lock(mutex);if(!store || !Within(path,store->saves)) return;
  remember.insert(path);if(closed) packages.insert(path);last_write=Clock::now();
}
void Written(rex::filesystem::Entry* entry) {
  if(!REXCVAR_GET(fm4_save_backups) && !REXCVAR_GET(fm4_photo_export)) return;
  auto* host=dynamic_cast<rex::filesystem::HostPathEntry*>(entry);if(!host)return;
  std::lock_guard lock(mutex);if(!store || !Within(host->host_path(),store->saves))return;
  last_write=Clock::now();dirty=true;
  if(REXCVAR_GET(fm4_photo_export)) {
    for(auto& p:remember)if(Within(host->host_path(),p))packages.insert(p);
    auto rel=host->host_path().lexically_relative(store->saves);
    auto part=rel.begin();
    if(part!=rel.end()) {auto type=*part++;if(part!=rel.end()) {
      auto name=part->string();std::transform(name.begin(),name.end(),name.begin(),::tolower);
      if(name.starts_with("photo")) {auto package=store->saves/type/(*part);remember.insert(package);packages.insert(package);}
    }}
  }
}
void Achievement(rex::system::KernelState* kernel,uint32_t user,uint32_t id) {
  if(!REXCVAR_GET(fm4_local_achievements)||user!=0||kernel->title_id()!=0x4D530910)return;
  std::lock_guard lock(mutex);if(!store || unlocks.size()>=256)return;
  if(!id)return;
  FILETIME ft;GetSystemTimeAsFileTime(&ft);
  auto key=std::to_string(id);
  if(achieved.contains(key))return;
  auto time=(uint64_t(ft.dwHighDateTime)<<32)|ft.dwLowDateTime;
  achieved[key]=time;unlocks.emplace_back(id,time);wake.notify_one();
}
uint64_t AchievementTime(uint32_t user,uint32_t id) {
  std::lock_guard lock(mutex);if(user!=0||!store)return 0;
  auto it=achieved.find(std::to_string(id));return it==achieved.end()?0:it->get<uint64_t>();
}
void TrainerClosed() {
  trainer_active.store(false);trainer_speed.store(1);
}
void PollDiagnostic(bool& visible) {
  const char* path=std::getenv("FM4_TOOLS_INPUT");
  if(!std::getenv("FM4_BACKGROUND")||!path||!*path)return;
  std::lock_guard diagnostic_lock(diagnostic_mutex);
  auto apply=[&] {
    if(!diagnostic_has_state)return;
    visible=diagnostic_visible;
    if(REXCVAR_GET(fm4_trainer) && diagnostic_tab=="Trainer" && visible) {
      trainer_active.store(diagnostic_armed);trainer_speed.store(diagnostic_speed);
    } else TrainerClosed();
  };
  apply();
  static auto last=Clock::time_point{};static uint64_t serial=0;
  if(Clock::now()-last<std::chrono::milliseconds(250))return;last=Clock::now();
  try {
    auto doc=ReadJson(fs::path(path));auto id=doc.value("id",uint64_t(0));if(id<=serial)return;
    const auto page=doc.value("page",std::string(""));
    double requested_speed=doc.value("speed",1.0);
    const int64_t requested_credits=doc.value("credits",int64_t(-1));
    if(!std::isfinite(requested_speed)||requested_speed<0.25||requested_speed>2 ||
       requested_credits < -1 || requested_credits>999999999)
      throw std::runtime_error("Invalid private tools fixture request");
    diagnostic_tab=page;diagnostic_visible=doc.value("visible",false);
    diagnostic_armed=doc.value("armed",false);diagnostic_speed=requested_speed;
    diagnostic_has_state=true;apply();
    if(REXCVAR_GET(fm4_trainer) && page=="Trainer" && visible) {
      bool arm=diagnostic_armed;
      trainer_active.store(arm);trainer_speed.store(requested_speed);
      if(arm && requested_credits>=0)pending_credits.store(requested_credits);
    } else TrainerClosed();
    {std::lock_guard lock(mutex);
      if(REXCVAR_GET(fm4_save_backups) && doc.value("backup",false))snapshot=true;
      if(REXCVAR_GET(fm4_photo_export) && doc.value("export",false))export_all=true;
    }
    serial=id;wake.notify_one();
    REXLOG_INFO("FM4 tools fixture {}: visible={} page={} armed={} speed={}",id,visible,page,
      trainer_active.load(),trainer_speed.load());
  } catch(const std::exception& e) {SetStatus(e.what());}
}
void DrawTabs() {
  if(!(REXCVAR_GET(fm4_photo_export)||REXCVAR_GET(fm4_save_backups)||
       REXCVAR_GET(fm4_trainer)||REXCVAR_GET(fm4_local_achievements)))return;
  {std::lock_guard lock(mutex);if(!store){ImGui::TextWrapped("%s",status.c_str());return;}}
  bool trainer_open=false;
  std::string requested_tab;{std::lock_guard lock(diagnostic_mutex);requested_tab=diagnostic_tab;}
  auto flags=[&](const char* page) {return requested_tab==page?ImGuiTabItemFlags_SetSelected:ImGuiTabItemFlags_None;};
  if(REXCVAR_GET(fm4_photo_export) && ImGui::BeginTabItem("Photos",nullptr,flags("Photos"))) {
    ImGui::TextWrapped("FM4 photo mode saves are exported with their original framing, effects and encoded dimensions. Big Shots retain their saved resolution.");
    ImGui::TextWrapped("PC folder: %s",PhotoDirectory().string().c_str());
    if(ImGui::Button("Export stored photos")) {std::lock_guard lock(mutex);export_all=true;wake.notify_one();}
    ImGui::TextWrapped("Only complete JPEG/PNG payloads are supported. Open a pre-existing photo in FM4 once if its package has an unfamiliar name.");
    ImGui::EndTabItem();
  }
  if(REXCVAR_GET(fm4_save_backups) && ImGui::BeginTabItem("Save backups",nullptr,flags("Save backups"))) {
    ImGui::TextWrapped("Launch, idle timer and after-save snapshots. Restore is applied before the next guest launch; the complete previous save folder is retained.");
    if(ImGui::Button("Back up when idle")) {std::lock_guard lock(mutex);snapshot=true;wake.notify_one();}
    std::vector<std::string> names;{std::lock_guard lock(mutex);names=backups;}
    ImGui::BeginChild("Backup generations",ImVec2(0,180),true);
    for(auto& name:names)if(ImGui::Selectable(name.c_str(),restore==name))restore=name;
    ImGui::EndChild();
    if(!restore.empty() && ImGui::Button("Restore selected backup...")) ImGui::OpenPopup("Confirm save restore");
    if(ImGui::BeginPopupModal("Confirm save restore",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted(restore.c_str());
      ImGui::TextWrapped("Restore this generation on next launch? Current saves will be preserved beside the title save folder. Exit FM4 normally, then restart with backups enabled.");
      if(ImGui::Button("Confirm restore on restart")) {
        try {std::lock_guard lock(mutex);if(store)store->RequestRestore(restore);status="Restore queued for next launch.";}
        catch(const std::exception& e) {SetStatus(e.what());}
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();ImGui::EndPopup();
    }
    ImGui::EndTabItem();
  }
  if(REXCVAR_GET(fm4_trainer) && ImGui::BeginTabItem("Trainer",nullptr,flags("Trainer"))) {
    trainer_open=true;
    bool armed=trainer_active.load();float speed=float(trainer_speed.load());
    ImGui::TextWrapped("Trainer changes affect this local save. Keep a backup. Closing this page disarms speed changes. A queued credits change is a single action and remains pending for the next game credits read.");
    ImGui::Checkbox("Arm trainer for this page",&armed);
    ImGui::BeginDisabled(!armed);
    ImGui::SliderFloat("Game speed",&speed,0.25f,2.0f,"%.2fx");
    ImGui::InputInt("Credits",&credits,10000,100000);
    credits=std::clamp(credits,0,999999999);
    if(ImGui::Button("Queue credits change")) pending_credits.store(credits);
    ImGui::TextWrapped("Close F6 and return to a menu that reads credits to apply through FM4's own setter.");
    ImGui::EndDisabled();
    if(pending_credits.load()>=0 && ImGui::Button("Cancel pending credits change"))pending_credits.store(-1);
    ImGui::TextWrapped("Time of day is selected with the track in FM4; a live lighting switch is not verified. Use FM4 photo mode for a free camera.");
    trainer_speed.store(speed);trainer_active.store(armed);
    ImGui::EndTabItem();
  }
  if(!trainer_open)TrainerClosed();
  if(REXCVAR_GET(fm4_local_achievements) && ImGui::BeginTabItem("Achievements",nullptr,flags("Achievements"))) {
    std::lock_guard lock(mutex);
    size_t total=metadata.size();
    auto known=[](uint32_t id){return std::any_of(metadata.begin(),metadata.end(),
        [id](const auto& a){return a.id==id;});};
    for(auto& [key,value]:achieved.items())if(!known(uint32_t(std::stoul(key))))++total;
    ImGui::Text("Local profile unlocks: %zu / %zu",achieved.size(),total);
    ImGui::BeginChild("Achievement list",ImVec2(0,250));
    for(auto& a:metadata) {
      bool done=achieved.contains(std::to_string(a.id));
      ImGui::TextWrapped("%s %s (%u G)",done?"Unlocked":"Locked",a.label.c_str(),a.gamerscore);
      ImGui::TextWrapped("%s",(done?a.description:a.unachieved_description).c_str());
      ImGui::Separator();
    }
    for(auto& [key,value]:achieved.items())if(!known(uint32_t(std::stoul(key)))) {
      ImGui::Text("Unlocked achievement %s",key.c_str());
      ImGui::TextWrapped("Title metadata for this guest unlock is not available.");ImGui::Separator();
    }
    ImGui::EndChild();ImGui::EndTabItem();
  }
  std::string text;{std::lock_guard lock(mutex);if(!store)return;text=status;}
  ImGui::TextWrapped("%s",text.c_str());
}
void DrawToasts() {
  if(!REXCVAR_GET(fm4_local_achievements))return;
  static std::string text;static auto until=Clock::time_point{};
  if(Clock::now()>=until) {
    std::lock_guard lock(mutex);text.clear();
    if(!toasts.empty()) {text=toasts.front();toasts.pop_front();until=Clock::now()+std::chrono::seconds(5);}
  }
  if(text.empty())return;
  auto* viewport=ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x+viewport->WorkSize.x-20,viewport->WorkPos.y+20),
                          ImGuiCond_Always,ImVec2(1,0));
  ImGui::SetNextWindowBgAlpha(0.9f);
  ImGui::Begin("FM4 achievement notification",nullptr,ImGuiWindowFlags_NoDecoration|
      ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::TextUnformatted("Achievement unlocked");ImGui::TextUnformatted(text.c_str());ImGui::End();
}
}
extern "C" __declspec(dllexport) double Fm4ToolsScaleDelta(double measured) {
  return fm4::tools::ScaledDelta(measured,fm4::tools::trainer_speed.load(),fm4::tools::trainer_active.load());
}
extern "C" __declspec(dllexport) bool Fm4ToolsTrainerActive() {return fm4::tools::trainer_active.load();}
extern "C" __declspec(dllexport) int64_t Fm4ToolsTakeCredits() {
  return fm4::tools::pending_credits.exchange(-1);
}
extern "C" __declspec(dllexport) bool Fm4ToolsHasCredits() {
  return REXCVAR_GET(fm4_trainer) && fm4::tools::pending_credits.load()>=0;
}
