// Runtime graphics settings. GPL-3.0-only.
#define FM4_LIVE_SETTINGS_EXPORT
#include "live_settings.h"
#ifdef FM4_MODS_UI
#include "mod_runtime.h"
#endif
#ifdef FM4_INGAME_TOOLS
#include "ingame_tools.h"
#endif
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

REXCVAR_DEFINE_BOOL(fm4_live_settings, false, "Forza Motorsport 4",
                   "Enable F6 graphics settings and the private diagnostic request file")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DECLARE(bool, m3_present_lean);
REXCVAR_DEFINE_INT32(fm4_live_output_width, 0, "Forza Motorsport 4", "Saved output width; zero uses the launcher")
    .range(0, 3840);
REXCVAR_DEFINE_INT32(fm4_live_output_height, 0, "Forza Motorsport 4", "Saved output height; zero uses the launcher")
    .range(0, 2160);
REXCVAR_DEFINE_BOOL(fm4_live_fullscreen, false, "Forza Motorsport 4", "Saved live settings fullscreen state");

namespace fm4::settings {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
const char* const effects[] = {"bilinear", "fsr", "cas"};
const char* const posts[] = {"none", "fxaa", "fxaa_extreme"};
const char* const upscalers[] = {"Bilinear", "FSR1", "CAS"};
std::mutex mutex;
std::optional<Request> pending;
std::string status = "Changes apply between frames.";
std::atomic<int> frame_limit{0};
std::atomic<bool> active{false};
uint64_t serial = 0, diagnostic_serial = 0;
bool in_flight = false;
rex::ui::Window* host_window = nullptr;
rex::ui::Presenter* host_presenter = nullptr;
Request applied;
bool have_applied = false;
UINT_PTR idle_timer = 0;
void PollIdleDialog();

std::filesystem::path EnvPath(const char* name) {
  const char* value = std::getenv(name);
  return value && *value ? std::filesystem::path(value) : std::filesystem::path();
}
std::filesystem::path SettingsPath() {
  auto path = EnvPath("FM4_LIVE_SETTINGS_PATH");
  if (!path.empty()) return path;
  wchar_t executable[32768];
  auto size = GetModuleFileNameW(nullptr, executable, 32768);
  if (!size || size == 32768) return {};
  for (auto dir = std::filesystem::path(executable).parent_path(); !dir.empty();) {
    auto candidate = dir / ".local/enhancements/settings.json";
    if (std::filesystem::is_regular_file(candidate)) return candidate;
    auto parent = dir.parent_path();
    if (parent == dir) break;
    dir = parent;
  }
  return {};
}
std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("Settings file could not be opened");
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
bool Valid(const Request& r) {
  return r.scale >= 1 && r.scale <= 3 &&
      ((r.width == 1280 && r.height == 720) ||
       (r.width == 2560 && r.height == 1440) ||
       (r.width == 3840 && r.height == 2160)) &&
      r.effect >= 0 && r.effect < 3 && r.post >= 0 && r.post < 3 &&
      r.anisotropy >= -1 && r.anisotropy <= 5 && std::isfinite(r.mip_bias) &&
      r.mip_bias >= -2 && r.mip_bias <= 2 &&
      (r.fps == 0 || r.fps == 60 || r.fps == 120 ||
       r.fps == 144 || r.fps == 165 || r.fps == 240);
}
Request Current() {
  { std::lock_guard lock(mutex); if (have_applied) return applied; }
  Request r;
  r.scale = std::clamp(rex::cvar::Query<int32_t>("resolution_scale"), 1, 3);
  r.width = 1280 * r.scale; r.height = 720 * r.scale;
  try {
    auto doc = Json::parse(Read(SettingsPath()));
    auto profile = doc.value("profile", "Native");
    int output_scale = profile == "4K" ? 3 : profile == "1440p" ? 2 : 1;
    r.width = 1280 * output_scale; r.height = 720 * output_scale;
    r.fps = doc.value("frame_rate_limit", 0);
  } catch (...) {}
  r.fullscreen = rex::cvar::Query<bool>("fullscreen");
  auto width = REXCVAR_GET(fm4_live_output_width), height = REXCVAR_GET(fm4_live_output_height);
  if (width && height) { r.width = width; r.height = height; r.fullscreen = REXCVAR_GET(fm4_live_fullscreen); }
  r.anisotropy = rex::cvar::Query<int32_t>("anisotropic_override");
  r.mip_bias = rex::cvar::Query<double>("texture_mip_bias");
  auto effect = rex::cvar::GetFlagByName("present_effect");
  auto post = rex::cvar::GetFlagByName("swap_post_effect");
  for (int i = 0; i < 3; ++i) {
    if (effect == effects[i]) r.effect = i;
    if (post == posts[i]) r.post = i;
  }
  return r;
}
void Queue(Request r) {
  std::lock_guard lock(mutex);
  if (!Valid(r)) { status = "Invalid graphics settings; nothing changed."; return; }
  if (in_flight || pending) { status = "A graphics change is already pending."; return; }
  r.id = ++serial;
  pending = r;
  status = "Waiting for the next frame boundary...";
}

// Read the latest document so unrelated launcher fields survive. ReplaceFile
// keeps an exact backup and atomically publishes the complete new document.
void Save(const Request* r, const std::optional<Json>& next_launch = {}) {
  const auto path = SettingsPath();
  if (path.empty()) throw std::runtime_error("Launcher settings path was not found");
  auto lease = path; lease += L".live-lock";
  HANDLE handle = CreateFileW(lease.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Settings save is busy");
  struct Lease { HANDLE value; ~Lease() { CloseHandle(value); } } guard{handle};
  auto before = Read(path);
  auto doc = Json::parse(before);
  if (!doc.is_object()) throw std::runtime_error("Invalid launcher settings document");
  if (r) {
    doc["internal_scale"] = r->scale;
    doc["profile"] = r->width == 3840 ? "4K" : r->width >= 1920 ? "1440p" : "Native";
    doc["upscaler"] = upscalers[r->effect];
    doc["frame_rate_limit"] = r->fps;
    // Output dimensions are independent from the internal rendering profile.
    auto& overrides = doc["runtime_overrides"];
    if (overrides.is_null()) overrides = Json::object();
    if (!overrides.is_object()) throw std::runtime_error("Invalid runtime_overrides");
    overrides["window_width"] = r->width;
    overrides["window_height"] = r->height;
    overrides["fm4_live_output_width"] = r->width;
    overrides["fm4_live_output_height"] = r->height;
    overrides["fm4_live_fullscreen"] = r->fullscreen;
    overrides["swap_post_effect"] = posts[r->post];
    overrides["texture_mip_bias"] = r->mip_bias;
    overrides["anisotropic_override"] = r->anisotropy;
  }
  if (next_launch) for (auto it = next_launch->begin(); it != next_launch->end(); ++it)
    doc[it.key()] = it.value();
  auto temporary = path; temporary += L".live-" + std::to_wstring(GetCurrentProcessId()) +
      L"-" + std::to_wstring(GetTickCount64()) + L".tmp";
  auto backup = path; backup += L".before-live-" + std::to_wstring(GetTickCount64());
  {
    std::ofstream file(temporary, std::ios::binary);
    file << doc.dump(4) << '\n'; file.flush();
    if (!file) throw std::runtime_error("Settings write failed");
  }
  if (Read(path) != before) throw std::runtime_error("Launcher settings changed; retry save");
  if (!ReplaceFileW(path.c_str(), temporary.c_str(), backup.c_str(), 0, nullptr, nullptr))
    throw std::runtime_error("Atomic settings replacement failed");
}

void ResizeDisplay(const Request& r) {
  if (!host_window) return;
  host_window->SetFullscreen(r.fullscreen && !std::getenv("FM4_BACKGROUND"));
  // The headless harness must remain hidden. SDL resize dispatches the normal
  // Window resize event; the presenter waits for old backbuffers itself.
  SDL_Window* native = nullptr;
  int count = 0;
  auto** windows = SDL_GetWindows(&count);
  for (int i = 0; i < count; ++i)
    if (SDL_GetPointerProperty(SDL_GetWindowProperties(windows[i]),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr) == host_window->GetNativeWindowHandle())
      native = windows[i];
  if (native) {
    bool accepted = SDL_SetWindowSize(native, r.width, r.height);
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(native, &width, &height);
    REXLOG_INFO("FM4 live display: requested={}x{} accepted={} pixels={}x{} fullscreen={}",
        r.width, r.height, accepted, width, height, r.fullscreen);
  }
  SDL_free(windows);
  if (auto* presenter = host_presenter) {
    auto config = presenter->GetGuestOutputPaintConfigFromUIThread();
    using Effect = rex::ui::Presenter::GuestOutputPaintConfig::Effect;
    auto effect = Effect::kBilinear;
#if defined(REX_HAS_SPATIAL_FIDELITYFX)
    if (r.effect == 1) effect = Effect::kFsr;
    if (r.effect == 2) effect = Effect::kCas;
#endif
    config.SetEffect(effect);
    presenter->SetGuestOutputPaintConfigFromUIThread(config);
  }
}

class Dialog final : public rex::ui::ImGuiDialog {
 public:
  Dialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  void Toggle() {
    visible_ = !visible_; active.store(visible_);
    if (visible_) {
      value_ = Current();
      try {
        auto doc = Json::parse(Read(SettingsPath()));
        launch_textures_ = doc.value("enable_textures", true);
        if (doc.value("texture_mode", "auto") == "off") launch_textures_ = false;
        const char* choices[] = {"Bilinear", "FSR1", "CAS", "DLSS", "FSR"};
        for (int i = 0; i < 5; ++i) if (doc.value("upscaler", "Bilinear") == choices[i]) launch_effect_ = i;
      } catch (...) {}
    }
    UpdateAttachment();
  }
  void UpdateAttachment() {
    if (!idle_timer) return;
    bool needed = visible_;
#ifdef FM4_INGAME_TOOLS
    needed = needed || fm4::tools::HasToasts();
#endif
    if (needed) imgui_drawer()->AddDialog(this);
    else imgui_drawer()->RemoveDialog(this);
  }
  void PollIdle() {
    if (visible_) return;
    PollController(nullptr);
#ifdef FM4_INGAME_TOOLS
    fm4::tools::PollDiagnostic(visible_);
    if (!visible_) fm4::tools::TrainerClosed();
#endif
    active.store(visible_);
    UpdateAttachment();
  }
  void PollController(ImGuiIO* io) {
    const bool focused = host_window && !std::getenv("FM4_BACKGROUND") &&
        GetForegroundWindow() == host_window->GetNativeWindowHandle();
    int count = 0;
    auto* ids = focused ? SDL_GetGamepads(&count) : nullptr;
    SDL_Gamepad* pad = nullptr;
    if (focused) for (int i = 0; i < count; ++i) {
      if (auto* candidate = SDL_GetGamepadFromID(ids[i])) {
        if (!pad || SDL_GetGamepadPlayerIndex(candidate) == 0) pad = candidate;
      }
    }
    auto button = [pad](SDL_GamepadButton b) { return pad && SDL_GetGamepadButton(pad, b); };
    bool chord = button(SDL_GAMEPAD_BUTTON_BACK) && button(SDL_GAMEPAD_BUTTON_START);
    if (chord && !chord_) Toggle();
    chord_ = chord;
    if (visible_ && io) {
      io->ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
      io->BackendFlags |= ImGuiBackendFlags_HasGamepad;
    }
      const std::pair<ImGuiKey, SDL_GamepadButton> keys[] = {
        {ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},
        {ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH},
        {ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST},
        {ImGuiKey_GamepadL1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {ImGuiKey_GamepadR1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}};
      if (io) for (auto [key, source] : keys) io->AddKeyEvent(key, visible_ && !chord && button(source));
    SDL_free(ids);
  }
  void OnDraw(ImGuiIO& io) override {
    PollController(&io);
#ifdef FM4_INGAME_TOOLS
    fm4::tools::PollDiagnostic(visible_);
    active.store(visible_);
    fm4::tools::DrawToasts();
    if (!visible_) fm4::tools::TrainerClosed();
#endif
    if (!visible_) { UpdateAttachment(); return; }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { Toggle(); return; }
    ImGui::SetNextWindowSize(ImVec2(610, 490), ImGuiCond_FirstUseEver);
    bool tools_drawn = false;
    if (ImGui::Begin("FM4 graphics settings (F6)", &visible_)) {
      if (ImGui::BeginTabBar("Graphics pages")) {
#ifdef FM4_MODS_UI
        if (ImGui::BeginTabItem("Mods")) { fm4::mods::DrawActions(); ImGui::EndTabItem(); }
#endif
        if (ImGui::BeginTabItem("Display")) {
          int output = value_.width == 3840 ? 2 : value_.width == 2560 ? 1 : 0;
          if (ImGui::Combo("Output size", &output, "1280 x 720\0 2560 x 1440\0 3840 x 2160\0")) {
            const int widths[] = {1280, 2560, 3840};
            value_.width = widths[output]; value_.height = value_.width * 9 / 16;
          }
          ImGui::Checkbox("Borderless fullscreen", &value_.fullscreen);
          ImGui::TextWrapped("Fullscreen uses the desktop display mode. Output size controls the window in windowed mode.");
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Rendering")) {
          const char* temporal = std::getenv("FM4_TEMPORAL_MODE");
          if (temporal && std::string_view(temporal) != "SPATIAL")
            ImGui::TextWrapped("This temporal session requires Apply on next launch for graphics changes.");
          ImGui::SliderInt("Internal scale (native and legacy)", &value_.scale, 1, 3, "%dx");
          ImGui::Combo("Spatial upscaler", &value_.effect, "Bilinear\0FSR1\0CAS\0");
          ImGui::Combo("AA / post effect", &value_.post, "None\0FXAA\0FXAA extreme\0");
          float bias = float(value_.mip_bias);
          if (ImGui::SliderFloat("Texture mip bias", &bias, -2, 2, "%.2f")) value_.mip_bias = bias;
          int aniso = value_.anisotropy + 1;
          if (ImGui::Combo("Anisotropy", &aniso, "Game default\0Disabled\0 1x\0 2x\0 4x\0 8x\0 16x\0")) value_.anisotropy = aniso - 1;
          ImGui::Separator(); ImGui::TextUnformatted("Apply on next launch");
          ImGui::TextWrapped("DLSS / temporal FSR load vendor libraries at launch. Texture packs create a loader and require a restart.");
          ImGui::Combo("Launch upscaler", &launch_effect_, "Bilinear\0FSR1\0CAS\0DLSS\0FSR\0");
          ImGui::Checkbox("Texture pack enabled", &launch_textures_);
          if (ImGui::Button("Apply on next launch")) {
            try {
              const char* choices[] = {"Bilinear", "FSR1", "CAS", "DLSS", "FSR"};
              Save(&value_, Json{{"upscaler", choices[launch_effect_]}, {"enable_textures", launch_textures_},
                  {"texture_mode", launch_textures_ ? "on" : "off"}});
              std::lock_guard lock(mutex); status = "Next-launch settings saved.";
            } catch (const std::exception& e) { std::lock_guard lock(mutex); status = e.what(); }
          }
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Performance")) {
          const int limits[] = {0, 60, 120, 144, 165, 240};
          int index = 0; for (int i = 0; i < 6; ++i) if (value_.fps == limits[i]) index = i;
          if (ImGui::Combo("Frame-rate limit", &index, "Unlimited\0 60\0 120\0 144\0 165\0 240\0")) value_.fps = limits[index];
          ImGui::TextWrapped("Live caps pace host rendering without changing the simulation clock. Unlimited removes this host cap.");
          if (!rex::cvar::Query<bool>("gpu_unlimited_present")) {
            ImGui::TextWrapped("This session has a launch-time guest cap. Restart with Unlimited to use the full live range.");
            if (ImGui::Button("Save frame-rate limit for next launch")) {
              try { Save(nullptr, Json{{"frame_rate_limit", value_.fps}}); }
              catch (const std::exception& e) { std::lock_guard lock(mutex); status = e.what(); }
            }
          }
          ImGui::EndTabItem();
        }
#ifdef FM4_INGAME_TOOLS
        fm4::tools::DrawTabs();
        tools_drawn = true;
#endif
        ImGui::EndTabBar();
      }
      ImGui::Separator();
      if (ImGui::Button("Apply and save")) Queue(value_);
      ImGui::SameLine(); if (ImGui::Button("Close")) visible_ = false;
      std::string message; { std::lock_guard lock(mutex); message = status; }
      ImGui::TextWrapped("%s", message.c_str());
      ImGui::TextWrapped("Controller: Back + Start opens/closes; D-pad navigates, A selects, B returns.");
    }
    ImGui::End(); active.store(visible_);
#ifdef FM4_INGAME_TOOLS
    if (!visible_ || !tools_drawn) fm4::tools::TrainerClosed();
#endif
    UpdateAttachment();
  }
 private:
  bool visible_ = false, chord_ = false, launch_textures_ = true;
  int launch_effect_ = 0;
  Request value_;
};
std::unique_ptr<Dialog> dialog;
void PollIdleDialog() {
  if (dialog) dialog->PollIdle();
}
}

bool Take(Request& r) {
  if (!REXCVAR_GET(fm4_live_settings)) return false;
  static auto last_poll = Clock::time_point{};
  if (Clock::now() - last_poll > std::chrono::milliseconds(500)) {
    last_poll = Clock::now();
    auto input = EnvPath("FM4_LIVE_SETTINGS_INPUT");
    if (!input.empty() && std::filesystem::is_regular_file(input)) {
      try {
        auto doc = Json::parse(Read(input));
        auto id = doc.at("id").get<uint64_t>();
        if (id > diagnostic_serial) {
          auto value = Current();
          value.scale = doc.value("scale", value.scale);
          value.width = doc.value("width", value.width); value.height = doc.value("height", value.height);
          value.effect = doc.value("effect", value.effect); value.post = doc.value("post", value.post);
          value.mip_bias = doc.value("mip_bias", value.mip_bias);
          value.anisotropy = doc.value("anisotropy", value.anisotropy);
          value.fps = doc.value("fps", value.fps); value.persist = doc.value("persist", false);
          if (Valid(value)) { Queue(value); diagnostic_serial = id; }
        }
      } catch (const std::exception& e) { REXLOG_WARN("FM4 live request rejected: {}", e.what()); }
    }
  }
  std::lock_guard lock(mutex);
  if (!pending || in_flight) return false;
  r = *pending; pending.reset(); in_flight = true;
  return true;
}
void Complete(const Request& r, bool success) {
  std::string message = success ? "Applied." :
      "Could not apply live. Temporal sessions and higher guest frame caps require Apply on next launch; current settings retained.";
  if (success) {
    { std::lock_guard lock(mutex); applied = r; have_applied = true; }
    frame_limit.store(r.fps);
    {
      std::lock_guard lock(mutex);
      if (host_window) host_window->app_context().CallInUIThreadDeferred([r] { ResizeDisplay(r); });
    }
    try { if (r.persist) { Save(&r); message = "Applied and saved."; } }
    catch (const std::exception& e) { message = std::string("Applied; save failed: ") + e.what(); }
  }
  REXLOG_INFO("FM4 live settings {}: scale={} output={}x{} fps={} {}", r.id, r.scale, r.width, r.height, r.fps, message);
  std::lock_guard lock(mutex); status = message; in_flight = false;
}
void Pace() {
  static auto previous = Clock::now();
  const int fps = frame_limit.load(std::memory_order_relaxed);
  if (fps > 0) std::this_thread::sleep_until(previous + std::chrono::nanoseconds(1000000000 / fps));
  previous = Clock::now();
}
bool Capturing() { return active.load(std::memory_order_relaxed); }
void Attach(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer, rex::ui::Presenter* presenter) {
  if (!REXCVAR_GET(fm4_live_settings)) return;
  { std::lock_guard lock(mutex); host_window = window; host_presenter = presenter; }
  auto width = REXCVAR_GET(fm4_live_output_width), height = REXCVAR_GET(fm4_live_output_height);
  if (width && height) { auto initial = Current(); ResizeDisplay(initial); }
  dialog = std::make_unique<Dialog>(drawer);
  if (REXCVAR_GET(m3_present_lean)) {
    // Win32 dispatches this callback on the UI thread, including while SDL is
    // waiting for messages. No worker owns dialog or queues lifetime-sensitive
    // callbacks. Polling a closed panel must not submit a duplicate game frame.
    idle_timer = SetTimer(nullptr, 0, 32, [](HWND, UINT, UINT_PTR, DWORD) { PollIdleDialog(); });
    if (!idle_timer) REXLOG_WARN("FM4 settings idle timer unavailable; retaining continuous redraw");
    dialog->UpdateAttachment();
  }
  rex::ui::RegisterBind("bind_fm4_graphics", "F6", "Toggle FM4 graphics settings", [] { if (dialog) dialog->Toggle(); });
}
void Detach() {
  if (idle_timer) { KillTimer(nullptr, idle_timer); idle_timer = 0; }
#ifdef FM4_INGAME_TOOLS
  fm4::tools::Stop();
#endif
  { std::lock_guard lock(mutex); host_window = nullptr; host_presenter = nullptr; }
  rex::ui::UnregisterBind("bind_fm4_graphics");
  dialog.reset(); active.store(false);
}
}

extern "C" __declspec(dllexport) void Fm4LiveSettingsAttach(
    rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer, rex::ui::Presenter* presenter) {
  fm4::settings::Attach(window, drawer, presenter);
}
extern "C" __declspec(dllexport) void Fm4LiveSettingsDetach() { fm4::settings::Detach(); }
