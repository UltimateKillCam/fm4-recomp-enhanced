// Local in-game tools storage. GPL-3.0-only.
#include "ingame_store.h"
#include "ingame_io.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace fm4::tools {
namespace {
using Microsoft::WRL::ComPtr;
struct Handle {
  HANDLE h = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE h) : h(h) {}
  Handle(Handle&& other) noexcept : h(other.h) { other.h = INVALID_HANDLE_VALUE; }
  Handle(const Handle&) = delete;
  ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
void Require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
std::string Stamp() {
  static std::atomic<uint64_t> serial{0};
  SYSTEMTIME t; GetSystemTime(&t);
  std::ostringstream s;
  s << std::setfill('0') << std::setw(4) << t.wYear << std::setw(2) << t.wMonth
    << std::setw(2) << t.wDay << '-' << std::setw(2) << t.wHour << std::setw(2)
    << t.wMinute << std::setw(2) << t.wSecond << '-' << std::setw(3) << t.wMilliseconds
    << '-' << GetCurrentProcessId() << '-' << GetTickCount64() << '-' << ++serial;
  return s.str();
}
bool Plain(const fs::path& p) {
  auto a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}
std::vector<fs::path> Files(const fs::path& root) {
  std::vector<fs::path> result;
  if (!fs::exists(root)) return result;
  Require(Plain(root), "Reparse-point save roots are not supported");
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    Require(Plain(e.path()), "Reparse points are not supported in save data");
    if (e.is_regular_file()) result.push_back(e.path());
  }
  std::sort(result.begin(), result.end());
  return result;
}
std::string Hash(const std::vector<uint8_t>& bytes) {
  BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
  Require(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0,
          "SHA256 initialization failed");
  struct Alg { BCRYPT_ALG_HANDLE h; ~Alg() { BCryptCloseAlgorithmProvider(h, 0); } } a{alg};
  Require(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0, "SHA256 creation failed");
  struct H { BCRYPT_HASH_HANDLE h; ~H() { BCryptDestroyHash(h); } } guard{hash};
  std::array<uint8_t,32> digest{};
  Require(bytes.size() <= ULONG_MAX && BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
      ULONG(bytes.size()), 0) == 0 && BCryptFinishHash(hash, digest.data(), ULONG(digest.size()), 0) == 0,
      "SHA256 failed");
  std::ostringstream out; out << std::hex << std::setfill('0');
  for (auto b : digest) out << std::setw(2) << unsigned(b);
  return out.str();
}
std::vector<uint8_t> ReadHandle(HANDLE h, size_t max = 128u * 1024 * 1024) {
  LARGE_INTEGER n{};
  Require(GetFileSizeEx(h, &n) && n.QuadPart >= 0 && uint64_t(n.QuadPart) <= max,
          "Save file exceeds the storage safety limit");
  std::vector<uint8_t> bytes(size_t(n.QuadPart));
  DWORD count = 0;
  Require(bytes.empty() || (ReadFile(h, bytes.data(), DWORD(bytes.size()), &count, nullptr) &&
      count == bytes.size()), "Save file read failed");
  return bytes;
}
std::vector<uint8_t> ReadBytes(const fs::path& p) {
  Require(Plain(p), "Invalid or linked save file");
  Handle h(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                      FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  Require(h.h != INVALID_HANDLE_VALUE, "Save is busy; close the game or wait for its write to finish");
  return ReadHandle(h.h);
}
void Put(const fs::path& p, const std::vector<uint8_t>& bytes) {
  fs::create_directories(p.parent_path());
  Handle h(CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  Require(h.h != INVALID_HANDLE_VALUE, "Could not create new storage file");
  DWORD count = 0;
  Require((bytes.empty() || (WriteFile(h.h, bytes.data(), DWORD(bytes.size()), &count, nullptr) &&
      count == bytes.size())) && FlushFileBuffers(h.h), "Storage write failed");
}
bool SafeName(const std::string& name) {
  return !name.empty() && name.size() < 120 && name.find_first_not_of("0123456789-") == std::string::npos;
}
void CheckParents(const fs::path& p) {
  for (auto q = fs::absolute(p); !q.empty();) {
    if (fs::exists(q)) Require(Plain(q), "Linked storage directories are not supported");
    auto parent = q.parent_path(); if (parent == q) break; q = parent;
  }
}
struct Image { size_t start = 0, length = 0; UINT width = 0, height = 0; const char* extension = ".jpg"; };
size_t JpegEnd(const std::vector<uint8_t>& bytes,size_t start) {
  size_t pos=start+2;bool scan=false;
  while(pos+1<bytes.size()) {
    if(scan) {
      while(pos+1<bytes.size()) {
        if(bytes[pos++]!=0xff)continue;
        auto marker=bytes[pos];
        if(marker==0 || (marker>=0xd0 && marker<=0xd7)) {++pos;continue;}
        --pos;scan=false;break;
      }
    }
    if(pos+1>=bytes.size() || bytes[pos++]!=0xff)return 0;
    while(pos<bytes.size() && bytes[pos]==0xff)++pos;
    if(pos>=bytes.size())return 0;
    auto marker=bytes[pos++];if(marker==0xd9)return pos;
    if(marker==1 || (marker>=0xd0 && marker<=0xd7))continue;
    if(pos+2>bytes.size())return 0;
    size_t length=size_t(bytes[pos])*256+bytes[pos+1];
    if(length<2 || pos+length>bytes.size())return 0;
    pos+=length;scan=marker==0xda;
  }
  return 0;
}
// The guest may wrap its encoded image in metadata. WIC validates the embedded
// stream and dimensions; the largest complete image excludes embedded thumbnails.
Image FindImage(std::vector<uint8_t>& bytes) {
  Image best;
  ComPtr<IWICImagingFactory> factory;
  Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
          IID_PPV_ARGS(&factory))), "Image decoder is unavailable");
  unsigned candidates = 0;
  for (size_t start = 0; start + 12 <= bytes.size() && candidates < 128; ++start) {
    bool jpeg = bytes[start] == 0xff && bytes[start+1] == 0xd8 && bytes[start+2] == 0xff;
    constexpr uint8_t png[] = {137,80,78,71,13,10,26,10};
    bool is_png = std::equal(std::begin(png), std::end(png), bytes.begin()+start);
    if (!jpeg && !is_png) continue;
    ++candidates;
    size_t end = 0;
    if (jpeg) {
      end=JpegEnd(bytes,start);
    } else {
      size_t pos = start + 8;
      while (pos + 12 <= bytes.size()) {
        uint32_t n = uint32_t(bytes[pos])<<24 | uint32_t(bytes[pos+1])<<16 |
                     uint32_t(bytes[pos+2])<<8 | bytes[pos+3];
        if (uint64_t(pos) + 12 + n > bytes.size()) break;
        bool last = std::equal(bytes.begin()+pos+4,bytes.begin()+pos+8,"IEND");
        pos += size_t(n)+12;
        if (last && n == 0) { end = pos; break; }
      }
    }
    if (!end) continue;
    ComPtr<IWICStream> stream; ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0, height = 0;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(
        bytes.data()+start, DWORD(end-start))) || FAILED(factory->CreateDecoderFromStream(
        stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) ||
        width < 640 || height < 360 || width > 8192 || height > 8192) continue;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
        WICBitmapPaletteTypeCustom))) continue;
    std::vector<uint8_t> pixels(size_t(width)*height*4);
    if (FAILED(converter->CopyPixels(nullptr, width*4, UINT(pixels.size()), pixels.data()))) continue;
    if (uint64_t(width)*height > uint64_t(best.width)*best.height)
      best = {start, end-start, width, height, is_png ? ".png" : ".jpg"};
    start = end - 1;
  }
  return best;
}
}
Json ReadJson(const fs::path& p) {
  if (!fs::exists(p)) return Json::object();
  auto b = ReadBytes(p); return Json::parse(b.begin(), b.end());
}
void WriteJson(const fs::path& p, const Json& doc) {
  CheckParents(p.parent_path());
  auto tmp = p; tmp += ".pending-" + Stamp();
  auto text = doc.dump(2) + "\n";
  Put(tmp, {text.begin(),text.end()});
  if (fs::exists(p)) {
    auto old = p; old += ".previous-" + Stamp();
    Require(ReplaceFileW(p.c_str(), tmp.c_str(), old.c_str(), 0, nullptr, nullptr),
            "Could not replace state atomically");
  } else fs::rename(tmp,p);
}
bool Within(const fs::path& child, const fs::path& parent) {
  auto c = fs::absolute(child).lexically_normal().native();
  auto p = fs::absolute(parent).lexically_normal().native();
  std::transform(c.begin(),c.end(),c.begin(),::towlower);
  std::transform(p.begin(),p.end(),p.begin(),::towlower);
  if (!p.empty() && p.back() != L'\\') p += L'\\';
  return c.size() > p.size() && c.compare(0,p.size(),p) == 0;
}
Store::Store(fs::path s, fs::path t, fs::path p, fs::path a, std::string x, unsigned k)
    : saves(fs::absolute(s).lexically_normal()), state(fs::absolute(t).lexically_normal()),
      photos(fs::absolute(p).lexically_normal()), archive(fs::absolute(a).lexically_normal()),
      keep(std::clamp(k,2u,32u)), profile(std::move(x)) {
  Require(!Within(state,saves) && !Within(saves,state) && state != saves &&
      !Within(photos,saves) && !Within(saves,photos) && photos != saves &&
      !Within(archive,saves) && !Within(saves,archive) && archive != saves,
      "Tool storage must be outside the save directory");
  for (auto& root : {saves,state,photos,archive}) CheckParents(root);
}
std::vector<std::string> Store::Backups() const {
  std::vector<std::string> names;
  auto root = state / "backups";
  if (fs::exists(root)) for (auto& e : fs::directory_iterator(root))
    if (e.is_directory() && Plain(e.path()) && SafeName(e.path().filename().string()) &&
        fs::is_regular_file(e.path()/"manifest.json")) names.push_back(e.path().filename().string());
  std::sort(names.rbegin(),names.rend()); return names;
}
std::string Store::Backup() {
  auto files = Files(saves);
  if (files.empty()) return {};
  Require(files.size() <= 8192, "Too many files for a save snapshot");
  std::vector<Handle> leases;
  for (auto& p : files) {
    leases.emplace_back(CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    Require(leases.back().h != INVALID_HANDLE_VALUE, "Save is busy; snapshot deferred");
  }
  auto name = Stamp(); auto temp = state/"backups"/("pending-"+name);
  Json manifest{{"version",1},{"profile",profile},{"files",Json::object()}};
  uint64_t total = 0;
  for (size_t i=0;i<files.size();++i) {
    auto bytes = ReadHandle(leases[i].h); total += bytes.size();
    Require(total <= 1024ull*1024*1024, "Save snapshot exceeds 1 GB");
    auto rel = files[i].lexically_relative(saves);
    Put(temp/"data"/rel,bytes);
    manifest["files"][rel.generic_string()] = {{"size",bytes.size()},{"sha256",Hash(bytes)}};
  }
  Require(files == Files(saves), "Save file list changed during snapshot; snapshot deferred");
  WriteJson(temp/"manifest.json",manifest);
  fs::rename(temp,state/"backups"/name);
  auto names = Backups();
  for (size_t i=keep;i<names.size();++i) {
    fs::create_directories(archive/"rotated"/profile);
    fs::rename(state/"backups"/names[i],archive/"rotated"/profile/names[i]);
  }
  return name;
}
void Store::RequestRestore(const std::string& name) {
  Require(SafeName(name), "Invalid backup name");
  auto names = Backups(); Require(std::find(names.begin(),names.end(),name)!=names.end(),"Backup not found");
  WriteJson(state/"restore.json",{{"profile",profile},{"backup",name}});
}
bool Store::RestoreAtLaunch() {
  auto request = state/"restore.json";
  auto journal = state/"restore-journal.json";
  if(fs::exists(journal)) {
    auto doc=ReadJson(journal);
    Require(doc.at("profile").get<std::string>()==profile,"Restore journal profile mismatch");
    auto previous_name=doc.at("previous").get<std::string>();
    auto staged_name=doc.at("staged").get<std::string>();
    Require(previous_name.starts_with("before-restore-") && SafeName(previous_name.substr(15)) &&
      staged_name.starts_with("restore-") && SafeName(staged_name.substr(8)),"Invalid restore journal");
    auto previous=saves.parent_path()/previous_name, staged=saves.parent_path()/staged_name;
    if(!fs::exists(saves) && fs::exists(previous))fs::rename(previous,saves);
    else if(fs::exists(saves) && !fs::exists(staged)) {
      if(fs::exists(request))fs::rename(request,state/("restore-applied-"+Stamp()+".json"));
      fs::rename(journal,state/("restore-finished-"+Stamp()+".json"));return true;
    }
    fs::rename(journal,state/("restore-recovered-"+Stamp()+".json"));
  }
  if (!fs::exists(request)) return false;
  auto r = ReadJson(request);
  Require(r.at("profile").get<std::string>()==profile,"Restore profile mismatch");
  auto name = r.at("backup").get<std::string>(); Require(SafeName(name),"Invalid restore name");
  auto source = state/"backups"/name;
  auto m = ReadJson(source/"manifest.json");
  Require(m.at("version")==1 && m.at("profile").get<std::string>()==profile && m.at("files").is_object() &&
      !m.at("files").empty(),"Invalid restore manifest");
  auto staged = saves.parent_path()/("restore-"+Stamp());
  for (auto& [key,item] : m.at("files").items()) {
    fs::path rel(key); Require(!rel.is_absolute() && key.find(':')==std::string::npos &&
      Within(source/"data"/rel,source/"data"),"Invalid restore path");
    for(const auto& component:rel)Require(component!=".." && component!=".","Invalid restore component");
    auto bytes = ReadBytes(source/"data"/rel);
    Require(bytes.size()==item.at("size").get<uint64_t>() && Hash(bytes)==item.at("sha256").get<std::string>(),"Backup verification failed");
    Put(staged/rel,bytes);
  }
  // Acquire every current file without sharing writes/deletes before making
  // any rename. A second process with an open writer blocks the restore.
  std::vector<Handle> leases;
  for (auto& p : Files(saves)) {
    leases.emplace_back(CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,0,nullptr));
    Require(leases.back().h != INVALID_HANDLE_VALUE,"Save is busy; restore refused");
  }
  auto previous = saves.parent_path()/("before-restore-"+Stamp());
  WriteJson(journal,{{"profile",profile},{"previous",previous.filename().string()},
                    {"staged",staged.filename().string()}});
  // Windows refuses an ancestor directory move while descendants are open.
  // The caller holds the profile session lease and runs before guest launch.
  leases.clear();
  bool existed = fs::exists(saves);
  if (existed) fs::rename(saves,previous);
  try { fs::rename(staged,saves); }
  catch (...) { if (existed) fs::rename(previous,saves); throw; }
  fs::rename(request,state/("restore-applied-"+Stamp()+".json"));
  fs::rename(journal,state/("restore-finished-"+Stamp()+".json"));
  return true;
}
void Store::RememberPhotoPackage(const fs::path& package) {
  Require(Within(package,saves),"Photo package is outside this profile");
  auto registry=ReadJson(state/"photo-packages.json");
  auto key=package.lexically_relative(saves).generic_string();
  if (registry.contains(key)) return;
  registry[key]=true; WriteJson(state/"photo-packages.json",registry);
}
std::vector<fs::path> Store::PhotoPackages() const {
  std::vector<fs::path> out;
  auto registry=ReadJson(state/"photo-packages.json");
  for (auto& [key,value] : registry.items()) {
    auto p=saves/fs::path(key); if (Within(p,saves) && fs::is_directory(p)) out.push_back(p);
  }
  if (fs::is_directory(saves)) for (auto& type : fs::directory_iterator(saves))
    if(type.is_directory() && Plain(type.path()) && type.path().filename()!="Headers")
      for (auto& e : fs::directory_iterator(type.path())) {
        auto name=e.path().filename().string();
        std::transform(name.begin(),name.end(),name.begin(),::tolower);
        if (e.is_directory() && name.starts_with("photo") &&
            std::find(out.begin(),out.end(),e.path())==out.end()) out.push_back(e.path());
      }
  return out;
}
unsigned Store::ExportPhotos(const fs::path& package) {
  Require(Within(package,saves),"Photo package is outside this profile");
  auto ledger=ReadJson(state/"photo-exports.json"); unsigned count=0;
  auto files=Files(package);
  for (auto& path : files) {
    if (path.filename()=="__thumbnail.png") continue;
    std::vector<uint8_t> bytes;
    {auto lease=io::TrySnapshot();
      Require(lease.owns_lock(),"Guest file operation in progress; photo export deferred");
      bytes=ReadBytes(path);
    }
    auto image=FindImage(bytes);
    if (!image.length) continue;
    std::vector<uint8_t> encoded(bytes.begin()+image.start,bytes.begin()+image.start+image.length);
    auto hash=Hash(encoded); if (ledger.contains(hash)) continue;
    auto output=photos/profile/(hash+image.extension);
    CheckParents(output.parent_path());
    if (!fs::exists(output)) Put(output,encoded);
    else Require(Hash(ReadBytes(output))==hash,"Photo export name collision");
    ledger[hash]={{"source",path.lexically_relative(saves).generic_string()},
      {"output",output.generic_string()},{"width",image.width},{"height",image.height}};
    ++count;
  }
  if (count) WriteJson(state/"photo-exports.json",ledger);
  return count;
}
Json Store::Unlocks() const {
  auto doc=ReadJson(state/"achievements.json");
  Require(doc.is_object() && doc.size()<=8192,"Invalid achievement state");
  for(auto& [key,value]:doc.items()) {
    Require(!key.empty() && key.size()<=10 && key.find_first_not_of("0123456789")==std::string::npos &&
        std::stoull(key)>0 && std::stoull(key)<=UINT32_MAX && value.is_number_unsigned(),
        "Invalid achievement unlock entry");
  }
  return doc;
}
bool Store::Unlock(uint32_t id,uint64_t filetime) {
  auto doc=Unlocks();auto key=std::to_string(id); if(doc.contains(key)) return false;
  doc[key]=filetime;WriteJson(state/"achievements.json",doc);return true;
}
double ScaledDelta(double measured,double speed,bool armed) {
  if (!armed || !std::isfinite(measured) || measured < 0 || !std::isfinite(speed)) return measured;
  return measured*std::clamp(speed,0.25,2.0);
}
}
