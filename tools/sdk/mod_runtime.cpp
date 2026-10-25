// Optional mod session integration. GPL-3.0-only.
#include "mod_runtime.h"
#include "../mods/fm4_mod.h"
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/system/xmemory.h>
#include <rex/system/thread_state.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/lzx.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/windowed_app_context.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <span>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

namespace fm4::mods {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
std::recursive_mutex gate;
Json receipt;
fs::path session_root, user_root, game_root;
bool configured = false, failed = false, running = false, visible = false;
rex::Runtime* runtime = nullptr;
std::unique_ptr<rex::filesystem::HostPathDevice> overlay;
std::vector<HANDLE> leases;
uint64_t frame_number = 0, update_number = 0;
thread_local bool guest_callback = false;
struct Label { float x, y, height; std::string text; };
struct Action { std::string label; Fm4ModAction callback; bool pending = false; };
struct Plugin {
  std::string id;
  HMODULE module = nullptr;
  Fm4ModHost host{};
  Fm4ModCallbacks callbacks{};
  Json settings = Json::object();
  std::map<uint32_t, Label> labels;
  std::map<uint32_t, Action> actions;
  bool loaded = false;
  bool initialized = false;
};
std::vector<std::unique_ptr<Plugin>> plugins;
Json Load(const fs::path& path) { std::ifstream in(path, std::ios::binary); if (!in) throw std::runtime_error("Cannot read mod session file"); return Json::parse(in); }
void Require(bool test, const char* message) { if (!test) throw std::runtime_error(message); }
fs::path Env(const char* key) { auto* value = std::getenv(key); return value && *value ? fs::u8path(value) : fs::path(); }
bool Enabled() { auto* value = std::getenv("FM4_MODS"); return value && std::string_view(value) == "1"; }
bool Same(const fs::path& a, const fs::path& b) {
  if (a.empty() || b.empty()) return false;
  std::error_code error;
  bool result = fs::equivalent(a, b, error);
  if (!error) return result;
  auto left = fs::weakly_canonical(a, error).wstring(); if (error) return false;
  auto right = fs::weakly_canonical(b, error).wstring(); if (error) return false;
  return CompareStringOrdinal(left.data(), int(left.size()), right.data(), int(right.size()), TRUE) == CSTR_EQUAL;
}
fs::path Child(const fs::path& root, const std::string& name) {
  Require(!name.empty() && name.find('\\') == std::string::npos && name.find(':') == std::string::npos, "Invalid session path");
  fs::path result = root;
  for (const auto& part : fs::u8path(name)) {
    Require(part != ".." && part != "." && !part.has_root_path(), "Session traversal");
    result /= part;
    auto attributes = GetFileAttributesW(result.c_str());
    Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "Missing or linked session file");
  }
  return result;
}
std::string Hash(const fs::path& path, bool lease = false) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(file != INVALID_HANDLE_VALUE, "Cannot lease mod input");
  BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
  std::string result;
  try {
    Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA256 unavailable");
    Require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0, "SHA256 creation failed");
    uint8_t buffer[65536], sum[32]; DWORD count = 0;
    for (;;) {
      Require(ReadFile(file, buffer, sizeof(buffer), &count, nullptr) != FALSE, "Session read failed");
      if (!count) break;
      Require(BCryptHashData(hash, buffer, count, 0) >= 0, "SHA256 update failed");
    }
    Require(BCryptFinishHash(hash, sum, sizeof(sum), 0) >= 0, "SHA256 finish failed");
    static constexpr char hex[] = "0123456789abcdef";
    for (auto b : sum) { result += hex[b >> 4]; result += hex[b & 15]; }
  } catch (...) {
    if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); CloseHandle(file); throw;
  }
  BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
  if (lease) leases.push_back(file); else CloseHandle(file);
  return result;
}
void Save(const fs::path& path, const Json& data) {
  auto stage = path; stage += ".pending";
  std::ofstream out(stage, std::ios::binary | std::ios::trunc);
  Require(bool(out), "Cannot save mod setting"); out << data.dump(2); out.close();
  Require(bool(out), "Cannot finish mod setting");
  if (fs::exists(path)) {
    auto backup = path; backup += "." + std::to_string(GetTickCount64()) + ".backup";
    Require(ReplaceFileW(path.c_str(), stage.c_str(), backup.c_str(), 0, nullptr, nullptr) != FALSE, "Cannot publish mod setting");
  } else Require(MoveFileW(stage.c_str(), path.c_str()) != FALSE, "Cannot publish mod setting");
}
Plugin* Owner(Fm4ModHandle handle) {
  for (auto& p : plugins) if (p.get() == handle) return p.get();
  return nullptr;
}
bool Text(const char* value, size_t maximum) { return value && strnlen(value, maximum + 1) <= maximum; }
std::vector<uint8_t> Sha(std::span<const uint8_t> bytes) {
  BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
  Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA256 unavailable");
  auto status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
  if (status < 0) { BCryptCloseAlgorithmProvider(algorithm, 0); throw std::runtime_error("SHA256 creation failed"); }
  std::vector<uint8_t> result(32);
  status = BCryptHashData(hash, const_cast<uint8_t*>(bytes.data()), ULONG(bytes.size()), 0);
  if (status >= 0) status = BCryptFinishHash(hash, result.data(), ULONG(result.size()), 0);
  BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
  Require(status >= 0, "SHA256 failed"); return result;
}
uint32_t Be(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Put(std::vector<uint8_t>& bytes, uint32_t value) { for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(uint8_t(value >> shift)); }
void PrepareProtectedResource() {
  // Recalculate only the replaced protected files, preserving the original
  // game's complete indexed file set and checks for every untouched input.
  auto* memory = runtime->memory();
  auto module = runtime->kernel_state()->GetExecutableModule();
  Require(bool(module) && module->entry_point() == 0x8275F9F0, "Unsupported mod executable entry point");
  rex::xex2_opt_resource_info* directory = nullptr;
  Require(module->GetOptHeader(rex::XEX_HEADER_RESOURCE_INFO, &directory) == 0 && directory, "Missing protected resource directory");
  const auto* header = reinterpret_cast<const uint8_t*>(module->xex_header());
  const auto* begin = reinterpret_cast<const uint8_t*>(directory);
  auto header_size = uint32_t(module->xex_header()->header_size);
  Require(begin >= header && size_t(begin - header) <= header_size && header_size - size_t(begin - header) >= 4 &&
      directory->size >= 4 && (directory->size - 4) % sizeof(rex::xex2_resource) == 0 &&
      directory->size <= header_size - size_t(begin - header), "Invalid protected resource directory");
  rex::xex2_resource* selected = nullptr;
  for (uint32_t i = 0; i < (directory->size - 4) / sizeof(rex::xex2_resource); ++i) {
    auto& item = directory->resources[i];
    if (std::memcmp(item.name, "0D163575", 8) == 0) { Require(!selected, "Duplicate protected resource"); selected = &item; }
  }
  Require(selected && selected->address == 0x82EA0000 && selected->size == 0x5771, "Unknown protected resource location");
  std::span<const uint8_t> original(memory->TranslateVirtual<uint8_t*>(selected->address), selected->size);
  const uint8_t expected[] = {0x99,0x48,0xf4,0x05,0x3e,0x2e,0xbf,0x91,0xb3,0xc9,0x98,0xcb,0x5d,0xd4,0x9d,0x56,0x58,0xf5,0x0c,0xb9,0x63,0x1a,0x35,0x4e,0xa8,0x6e,0x2d,0x4c,0x5a,0x51,0x2c,0x98};
  auto original_hash = Sha(original);
  Require(std::equal(original_hash.begin(), original_hash.end(), expected), "Original protected resource identity mismatch");
  Require(Be(original.data()) == 32768 && Be(original.data() + 4) == 66, "Protected resource version mismatch");
  constexpr uint32_t count = 66, index_end = 8 + count * 8;
  std::vector<uint8_t> index, payload; uint32_t changed = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t offset = Be(original.data() + 8 + i * 8), size = Be(original.data() + 12 + i * 8);
    Require(offset < original.size() - index_end, "Protected resource offset invalid");
    size_t at = index_end + offset, end = at;
    while (end < original.size() && original[end]) ++end;
    Require(end < original.size(), "Protected resource name is unterminated");
    std::string name(reinterpret_cast<const char*>(original.data() + at), end - at);
    std::replace(name.begin(), name.end(), '\\', '/');
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    Require(!name.empty() && !name.starts_with('/') && name.find(':') == std::string::npos && name.find("..") == std::string::npos, "Unsafe protected filename");
    size_t hashes = size_t((uint64_t(size) + 32767) / 32768) * 32;
    Require(end + 1 + hashes <= original.size(), "Protected resource hash bounds invalid");
    Put(index, uint32_t(payload.size()));
    auto replacement = session_root / "overlay" / fs::u8path(name);
    if (fs::is_regular_file(replacement)) {
      std::ifstream in(replacement, std::ios::binary); Require(bool(in), "Cannot read protected overlay");
      auto new_size = fs::file_size(replacement); Require(new_size <= (256u << 20), "Protected overlay too large");
      Put(index, uint32_t(new_size));
      payload.insert(payload.end(), original.begin() + at, original.begin() + end + 1);
      uint8_t block[32768];
      for (uint64_t read = 0; read < new_size;) {
        auto bytes = size_t(std::min<uint64_t>(sizeof(block), new_size - read));
        in.read(reinterpret_cast<char*>(block), bytes); Require(size_t(in.gcount()) == bytes, "Protected overlay read failed");
        auto hash = Sha(std::span<const uint8_t>(block, bytes)); payload.insert(payload.end(), hash.begin(), hash.end()); read += bytes;
      }
      ++changed;
    } else {
      Put(index, size); payload.insert(payload.end(), original.begin() + at, original.begin() + end + 1 + hashes);
    }
  }
  if (!changed) return;
  std::vector<uint8_t> updated; Put(updated, 32768); Put(updated, count);
  updated.insert(updated.end(), index.begin(), index.end()); updated.insert(updated.end(), payload.begin(), payload.end());
  Require(updated.size() <= (1u << 20) && module->guest_xex_header(), "Protected overlay resource exceeds bounds");
  auto descriptor_offset = reinterpret_cast<const uint8_t*>(selected) - header;
  auto* guest = reinterpret_cast<rex::xex2_resource*>(memory->TranslateVirtual(module->guest_xex_header()) + descriptor_offset);
  Require(std::memcmp(guest, selected, sizeof(*guest)) == 0, "Protected host/guest directories disagree");
  auto address = memory->SystemHeapAlloc(uint32_t(updated.size()), 16); Require(address != 0, "Cannot allocate mod protected resource");
  std::memcpy(memory->TranslateVirtual(address), updated.data(), updated.size());
  selected->address = address; selected->size = uint32_t(updated.size());
  guest->address = address; guest->size = uint32_t(updated.size());
  REXLOG_INFO("FM4 mod protected resource prepared before guest start: {} changed files", changed);
}
void Log(Fm4ModHandle h, uint32_t level, const char* text) {
  std::lock_guard lock(gate); auto* p = Owner(h); if (!p || !Text(text, 4096)) return;
  if (level >= 2) REXLOG_WARN("Mod {}: {}", p->id, text); else REXLOG_INFO("Mod {}: {}", p->id, text);
}
uint8_t* Memory(uint32_t address, uint32_t size, bool write) {
  if (!running || !runtime || !runtime->memory() || !address || !size || size > (1u << 20) || uint64_t(address) + size > 0x100000000ull) return nullptr;
  if (write && address < 0x83000000 && uint64_t(address) + size > 0x82000000) return nullptr;
  auto* memory = runtime->memory();
  for (uint64_t at = address, end = uint64_t(address) + size; at < end;) {
    auto* heap = memory->LookupHeap(uint32_t(at)); rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(uint32_t(at) & ~(heap->page_size() - 1), &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) ||
        !(info.protect & (write ? rex::memory::kMemoryProtectWrite : rex::memory::kMemoryProtectRead))) return nullptr;
    uint64_t next = uint64_t(info.base_address) + info.region_size;
    if (next <= at) return nullptr;
    at = std::min(next, end);
  }
  return memory->TranslateVirtual<uint8_t*>(address);
}
// Faults from concurrent guest frees/protection changes become an error status.
bool Copy(void* to, const void* from, size_t bytes) {
  __try { std::memcpy(to, from, bytes); return true; }
  __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int32_t Read(Fm4ModHandle h, uint32_t address, void* out, uint32_t count) {
  std::lock_guard lock(gate); if (!Owner(h) || !out) return FM4_MOD_INVALID;
  auto* bytes = Memory(address, count, false); return bytes && Copy(out, bytes, count) ? FM4_MOD_OK : FM4_MOD_INVALID;
}
int32_t Write(Fm4ModHandle h, uint32_t address, const void* in, uint32_t count) {
  std::lock_guard lock(gate); if (!Owner(h) || !in || !guest_callback) return FM4_MOD_UNAVAILABLE;
  auto* bytes = Memory(address, count, true); return bytes && Copy(bytes, in, count) ? FM4_MOD_OK : FM4_MOD_INVALID;
}
int32_t Call(Fm4ModHandle h, const char* symbol, const uint32_t* args, uint32_t count, uint64_t* result) {
  std::lock_guard lock(gate);
  if (!Owner(h) || !guest_callback || !runtime || !result) return FM4_MOD_UNAVAILABLE;
  if (!Text(symbol, 80) || std::string_view(symbol) != "presentation.set_interval" || count != 2 || !args || args[1] > 3) return FM4_MOD_INVALID;
  // Verified FM4 setter: stw r4,0x36a0(r3); blr. No arbitrary dispatch.
  constexpr uint32_t address = 0x826E5F88;
  const uint8_t expected[] = {0x90,0x83,0x36,0xa0,0x4e,0x80,0x00,0x20};
  auto* code = Memory(address, sizeof(expected), false);
  if (!code || std::memcmp(code, expected, sizeof(expected)) || uint64_t(args[0]) + 0x36a4 > UINT32_MAX || !Memory(args[0] + 0x36a0, 4, true)) return FM4_MOD_UNAVAILABLE;
  auto* thread = rex::runtime::ThreadState::Get();
  if (!thread || !runtime->function_dispatcher()->GetFunction(address)) return FM4_MOD_UNAVAILABLE;
  auto saved = *thread->context(); uint64_t arguments[] = {args[0], args[1]};
  try { *result = runtime->function_dispatcher()->Execute(thread, address, arguments, 2); }
  catch (...) { *thread->context() = saved; return FM4_MOD_UNAVAILABLE; }
  *thread->context() = saved;
  return FM4_MOD_OK;
}
int32_t SettingGet(Fm4ModHandle h, const char* key, char* out, uint32_t count) {
  std::lock_guard lock(gate); auto* p = Owner(h);
  if (!p || !Text(key, 128) || !out || !count || !p->settings.contains(key)) return FM4_MOD_INVALID;
  auto value = p->settings[key].get<std::string>(); if (value.size() >= count) return FM4_MOD_INVALID;
  std::memcpy(out, value.c_str(), value.size() + 1); return FM4_MOD_OK;
}
int32_t SettingSet(Fm4ModHandle h, const char* key, const char* value) {
  std::lock_guard lock(gate); auto* p = Owner(h);
  if (!p || !Text(key, 128) || !*key || !Text(value, 4096)) return FM4_MOD_INVALID;
  try {
    auto path = user_root / "mod-settings.json";
    auto all = fs::is_regular_file(path) ? Load(path) : Json::object();
    all[p->id][key] = value; Save(path, all); p->settings[key] = value; return FM4_MOD_OK;
  } catch (...) { return FM4_MOD_UNAVAILABLE; }
}
int32_t Hud(Fm4ModHandle h, uint32_t id, float x, float y, float height, const char* text) {
  std::lock_guard lock(gate); auto* p = Owner(h);
  if (!p || !Text(text, 2048) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(height) || x < 0 || y < 0 || x > 1280 || y > 720 || height < 6 || height > 100) return FM4_MOD_INVALID;
  if (!*text) { p->labels.erase(id); return FM4_MOD_OK; }
  if (!p->labels.contains(id) && p->labels.size() >= 64) return FM4_MOD_INVALID;
  p->labels[id] = {x, y, height, text}; return FM4_MOD_OK;
}
int32_t AddAction(Fm4ModHandle h, uint32_t id, const char* text, Fm4ModAction callback) {
  std::lock_guard lock(gate); auto* p = Owner(h);
  if (!p || !Text(text, 256)) return FM4_MOD_INVALID;
  if (!callback || !*text) { p->actions.erase(id); return FM4_MOD_OK; }
  if (!p->actions.contains(id) && p->actions.size() >= 64) return FM4_MOD_INVALID;
  p->actions[id] = {text, callback}; return FM4_MOD_OK;
}
int32_t ReplaceString(Fm4ModHandle h, const char* table, uint32_t id, const char* text) {
  std::lock_guard lock(gate); auto* p = Owner(h);
  if (!p || !Text(table, 128) || !Text(text, 16384)) return FM4_MOD_INVALID;
  bool bound = false;
  for (const auto& mod : receipt["mods"]) if (mod["id"] == p->id)
    for (const auto& item : mod.value("strings", Json::array()))
      if (item["table"] == table && item["id"] == id) bound = true;
  if (!bound) return FM4_MOD_INVALID;
  try {
    auto path = user_root / "mod-string-requests.json";
    auto all = fs::is_regular_file(path) ? Load(path) : Json::object();
    all[p->id][table][std::to_string(id)] = text; Save(path, all); return FM4_MOD_RESTART;
  } catch (...) { return FM4_MOD_UNAVAILABLE; }
}
void Tick(bool update, double delta) {
  if (!running) return;
  uint64_t index = update ? ++update_number : ++frame_number;
  const bool was_guest = guest_callback; guest_callback = update;
  for (auto& p : plugins) if (p->loaded) {
    try {
      if (update && p->callbacks.update) p->callbacks.update(p->callbacks.state, index, delta);
      if (!update && p->callbacks.frame) p->callbacks.frame(p->callbacks.state, index, delta);
      if (!update) {
        std::vector<Fm4ModAction> pending;
        for (auto& [id, action] : p->actions) if (action.pending) { action.pending = false; pending.push_back(action.callback); }
        for (auto action : pending) action(p->callbacks.state);
      }
    } catch (...) { p->loaded = false; p->labels.clear(); p->actions.clear(); REXLOG_ERROR("Mod {} callback disabled after exception", p->id); }
  }
  guest_callback = was_guest;
}
class Dialog final : public rex::ui::ImGuiDialog {
 public:
  explicit Dialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  void OnDraw(ImGuiIO& io) override {
    std::lock_guard lock(gate); if (!running) return;
    if (std::getenv("FM4_MOD_SAMPLE_CHECK")) {
      static bool requested = false;
      auto stop = Env("FM4_MOD_STOP_FILE");
      if (!requested && !stop.empty() && fs::is_regular_file(stop) && runtime && runtime->app_context()) {
        requested = true;
        auto* context = runtime->app_context();
        context->CallInUIThreadDeferred([context] { context->QuitFromUIThread(); });
      }
    }
    Tick(false, double(io.DeltaTime));
    for (const auto& p : plugins) for (const auto& [id, label] : p->labels) {
      ImGui::GetForegroundDrawList()->AddText(ImGui::GetFont(), label.height * io.DisplaySize.y / 720.0f,
          ImVec2(label.x * io.DisplaySize.x / 1280.0f, label.y * io.DisplaySize.y / 720.0f), IM_COL32_WHITE, label.text.c_str());
    }
    if (visible) {
      ImGui::SetNextWindowSize(ImVec2(450, 300), ImGuiCond_FirstUseEver);
      if (ImGui::Begin("FM4 mods (F8)", &visible)) DrawActions();
      ImGui::End();
    }
  }
};
std::unique_ptr<Dialog> dialog;
}

void Configure(rex::PathConfig& paths) {
  std::lock_guard lock(gate);
  if (!Enabled()) return;
  auto path = Env("FM4_MOD_SESSION"); if (path.empty()) return;
  try {
    Require(Env("FM4_FORZA_PLUS_GENERATION").empty(), "Combined Forza Plus/mod sessions are not supported");
    session_root = fs::canonical(path).parent_path();
    receipt = Load(path);
    Require(receipt.at("protocol") == 1 && receipt.at("session").get<std::string>() == session_root.filename().string(), "Mod receipt protocol/identity mismatch");
    Require(!receipt.at("mods").empty(), "Empty mod receipt");
    game_root = fs::u8path(receipt.at("game").get<std::string>());
    Require(Same(paths.game_data_root, game_root), "Mod session belongs to another game folder");
    Require(Hash(game_root / "default.xex", true) == receipt.at("game_sha256").get<std::string>(), "Mod session executable mismatch");
    auto outputs = receipt.at("outputs");
    for (const auto& [name, expected] : outputs.items())
      Require(Hash(Child(session_root, name), true) == expected.get<std::string>(), "Prepared mod output changed");
    for (const auto& file : fs::recursive_directory_iterator(session_root / "overlay"))
      if (file.is_regular_file()) Require(outputs.contains(fs::relative(file.path(), session_root).generic_string()), "Unlisted overlay file");
    bool isolate = false;
    for (const auto& mod : receipt.at("mods")) {
      isolate |= mod.value("gameplay", false) || mod.contains("database") || mod.contains("native") || mod.contains("files") || mod.contains("archives");
      for (const auto& text : mod.value("strings", Json::array())) isolate |= text.contains("path");
    }
    Require(!isolate || receipt.at("isolated_saves").get<bool>(), "Gameplay mods require isolated saves");
    user_root = fs::u8path(receipt.at("user_root").get<std::string>());
    if (receipt.at("isolated_saves").get<bool>()) {
      auto expected = session_root.parent_path().parent_path() / "profiles" / session_root.filename();
      fs::create_directories(expected);
      Require(Same(user_root, expected), "Mod save path escaped session profile");
      paths.user_data_root = expected;
      paths.cache_root = expected / "cache"; paths.metadata_root = expected / "metadata";
    } else Require(Same(paths.user_data_root, user_root), "Visual mod stock save path differs");
    Require(paths.update_data_root.empty(), "Mod overlays currently require the base executable without a title-update mount");
    auto pack = receipt.at("textures").get<std::string>();
    if (!pack.empty()) Require(_putenv_s("FM4_TEXTURE_PACK", Child(session_root, pack).string().c_str()) == 0, "Cannot configure mod textures");
    overlay = std::make_unique<rex::filesystem::HostPathDevice>("\\Device\\Fm4Mods", Child(session_root, "overlay"), true);
    Require(overlay->Initialize(), "Cannot mount mod overlay");
    configured = true;
  } catch (const std::exception& error) {
    failed = true; REXLOG_ERROR("FM4 mod session rejected: {}", error.what());
    OutputDebugStringA(error.what());
    if (!session_root.empty()) { std::ofstream out(session_root.parent_path().parent_path() / "last-error.txt"); out << error.what(); }
  }
}
bool CheckPaths(const rex::PathConfig& paths) {
  std::lock_guard lock(gate);
  if (failed) return false;
  return !configured || (Same(paths.game_data_root, game_root) && Same(paths.user_data_root, user_root));
}
void Attach(rex::ui::ImGuiDrawer* drawer) {
  std::lock_guard lock(gate); if (!configured || !drawer) return;
  dialog = std::make_unique<Dialog>(drawer);
  rex::ui::RegisterBind("bind_fm4_mods", "F8", "Toggle mod actions", [] { std::lock_guard lock(gate); visible = !visible; });
}
void Start(rex::Runtime* instance) {
  std::lock_guard lock(gate); if (!configured || failed) return;
  runtime = instance; running = true;
  PrepareProtectedResource();
  if (std::getenv("FM4_MOD_SAMPLE_CHECK")) {
    uint32_t checked = 0;
    for (const auto& [name, hash] : receipt.at("outputs").items()) if (name.starts_with("overlay/")) {
      auto guest_path = std::string("game:\\") + name.substr(8);
      auto* entry = runtime->file_system()->ResolvePath(guest_path);
      Require(IsOverlay(entry) && entry->is_read_only(), "Mod VFS resolution check failed");
      rex::filesystem::File* file = nullptr; rex::filesystem::FileAction action{};
      auto status = runtime->file_system()->OpenFile(nullptr, guest_path, rex::filesystem::FileDisposition::kOpen,
          rex::filesystem::FileAccess::kFileReadData, false, true, &file, &action);
      Require(status == 0 && file, "Mod VFS read-open check failed"); file->Destroy(); file = nullptr;
      status = runtime->file_system()->OpenFile(nullptr, guest_path, rex::filesystem::FileDisposition::kOverwrite,
          rex::filesystem::FileAccess::kFileWriteData, false, true, &file, &action);
      Require(status != 0 && !file, "Mod VFS write-denial check failed");
      ++checked;
    }
    REXLOG_INFO("FM4 mod VFS checks passed: {} read-only replacements", checked);
  }
  auto settings_path = user_root / "mod-settings.json";
  Json saved = fs::is_regular_file(settings_path) ? Load(settings_path) : Json::object();
  for (const auto& item : receipt.at("plugins")) {
    auto owned = std::make_unique<Plugin>(); auto* p = owned.get(); p->id = item.at("id");
    p->settings = item.value("settings", Json::object());
    if (saved.contains(p->id)) p->settings.update(saved[p->id]);
    p->host = {sizeof(Fm4ModHost), FM4_MOD_ABI, p, p->id.c_str(), Log, Read, Write, Call,
        SettingGet, SettingSet, Hud, AddAction, ReplaceString};
    p->callbacks.bytes = sizeof(Fm4ModCallbacks); p->callbacks.abi = FM4_MOD_ABI;
    plugins.push_back(std::move(owned));
    try {
      p->module = LoadLibraryExW(Child(session_root, item.at("library")).c_str(), nullptr,
          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
      Require(p->module != nullptr, "Cannot load native mod library");
      auto entry = reinterpret_cast<Fm4ModEntry>(GetProcAddress(p->module, "Fm4ModStart"));
      Require(entry && entry(&p->host, &p->callbacks) == FM4_MOD_OK, "Native mod entry rejected");
      Require(p->callbacks.abi == FM4_MOD_ABI && p->callbacks.bytes == sizeof(Fm4ModCallbacks), "Native mod ABI mismatch");
      p->loaded = p->initialized = true; REXLOG_INFO("FM4 mod loaded: {}", p->id);
    } catch (const std::exception& error) {
      p->labels.clear(); p->actions.clear();
      REXLOG_ERROR("FM4 mod {} rejected: {}", p->id, error.what());
      // Fail the requested session rather than silently running partial gameplay changes.
      running = false; throw;
    }
  }
  REXLOG_INFO("FM4 mod session {} active; isolated saves={}", receipt.at("session").get<std::string>(), receipt.at("isolated_saves").get<bool>());
}
void Stop() {
  std::lock_guard lock(gate); running = false;
  rex::ui::UnregisterBind("bind_fm4_mods"); dialog.reset(); visible = false;
  for (auto it = plugins.rbegin(); it != plugins.rend(); ++it) {
    auto& p = **it;
    if (p.initialized && p.callbacks.unload) { try { p.callbacks.unload(p.callbacks.state); } catch (...) {} }
    p.actions.clear(); p.labels.clear(); if (p.module) FreeLibrary(p.module);
  }
  plugins.clear(); runtime = nullptr;
  for (auto file : leases) CloseHandle(file); leases.clear();
  // VFS entries may still be referenced until the runtime has stopped. Retain
  // the read-only device for the process lifetime; it is never hot reloaded.
}
rex::filesystem::Entry* Overlay(std::string_view path, rex::filesystem::Entry* root) {
  if (!configured || failed || !overlay) return nullptr;
  std::string name(path);
  if (root && name.find(':') == std::string::npos && !name.starts_with("\\Device\\")) name = root->absolute_path() + "\\" + name;
  std::replace(name.begin(), name.end(), '/', '\\');
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  const std::string prefixes[] = {"\\device\\harddisk0\\partition1\\", "game:\\", "d:\\"};
  bool found = false;
  for (const auto& prefix : prefixes) if (name.starts_with(prefix)) { name.erase(0, prefix.size()); found = true; break; }
  if (!found && (name.find(':') != std::string::npos || name.starts_with('\\'))) return nullptr;
  if (name.empty() || name == "default.xex" || name.find("..") != std::string::npos) return nullptr;
  auto* entry = overlay->ResolvePath(name);
  return entry && !(entry->attributes() & rex::filesystem::kFileAttributeDirectory) ? entry : nullptr;
}
bool IsOverlay(rex::filesystem::Entry* entry) { return entry && overlay && entry->device() == overlay.get(); }
bool Active() { return configured && !failed; }
bool Capturing() { std::lock_guard lock(gate); return running && visible; }
void DrawActions() {
  std::lock_guard lock(gate);
  if (!configured) { ImGui::TextUnformatted("No mods enabled for this session."); return; }
  for (const auto& mod : receipt.value("mods", Json::array())) ImGui::Text("%s  %s", mod.at("name").get_ref<const std::string&>().c_str(), mod.at("version").get_ref<const std::string&>().c_str());
  for (auto& p : plugins) {
    ImGui::PushID(p->id.c_str());
    for (auto& [id, action] : p->actions) { ImGui::PushID(int(id)); if (ImGui::Button(action.label.c_str())) action.pending = true; ImGui::PopID(); }
    ImGui::PopID();
  }
}
}
extern "C" __declspec(dllexport) void Fm4ModsUpdate(double delta) {
  std::lock_guard lock(fm4::mods::gate);
  if (std::isfinite(delta) && delta >= 0 && delta <= 1) fm4::mods::Tick(true, delta);
}
extern "C" __declspec(dllexport) void Fm4ModsConfigure(rex::PathConfig* paths) { fm4::mods::Configure(*paths); }
extern "C" __declspec(dllexport) bool Fm4ModsCheckPaths(const rex::PathConfig* paths) { return fm4::mods::CheckPaths(*paths); }
extern "C" __declspec(dllexport) void Fm4ModsStart(rex::Runtime* runtime) { fm4::mods::Start(runtime); }
extern "C" __declspec(dllexport) void Fm4ModsAttach(rex::ui::ImGuiDrawer* drawer) { fm4::mods::Attach(drawer); }
extern "C" __declspec(dllexport) void Fm4ModsStop() { fm4::mods::Stop(); }
extern "C" __declspec(dllexport) int32_t Fm4ModsDecodeLzx(
    const void* compressed, uint32_t compressed_size, void* decoded, uint32_t decoded_size) {
  if (!compressed || !decoded || !compressed_size || !decoded_size ||
      compressed_size > (256u << 20) || decoded_size > (256u << 20)) return -1;
  return lzx_decompress(compressed, compressed_size, decoded, decoded_size, 131072, nullptr, 0);
}
