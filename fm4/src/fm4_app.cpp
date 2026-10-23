// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
#include "generated/default/fm4_init.h"

#include "fm4_app.h"
#ifdef FM4_ENABLE_FORZA_PLUS_NATIVE
#include <rex/logging.h>
#include "forza_plus/native_app_hooks.inc"
#endif
#include "../../tools/wheel/quiet_capture.h"
#include <rex/system/interfaces/graphics.h>
#ifdef _WIN32
#include "wheel_driver.h"
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>

REXCVAR_DEFINE_STRING(
    fm4_disc2_content_root, "", "Forza Motorsport 4",
    "Directory containing legally obtained FM4 Disc 2 content packages");
REXCVAR_DEFINE_BOOL(fm4_auto_install_disc2_content, true, "Forza Motorsport 4",
                    "Install validated FM4 Disc 2 content packages at startup");

namespace {

constexpr uint32_t kFm4TitleId = 0x4D530910;
constexpr std::string_view kDisc2DropDirectory = "disc2-content";

// The four Disc 2 packages, or a downloadable content package named by its 42-digit
// content ID. Either way the STFS header must still be FM4 marketplace content.
// DLC support added 2026-10-23.
bool IsDisc2PackageName(std::string_view name) {
  if (name == "4d53091000000001" || name == "4d53091000000002" ||
      name == "4d53091000000003" || name == "4d53091000000004") {
    return true;
  }
  return name.size() == 42 &&
         name.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
}

std::string Hex8(uint32_t value) {
  std::ostringstream stream;
  stream << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
         << value;
  return stream.str();
}

std::string Disc2EnvironmentRoot() {
#ifdef _WIN32
  char *value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, "FM4_DISC2_ROOT") != 0 || !value) {
    return {};
  }
  std::string root(value);
  std::free(value);
  return root;
#else
  const char *value = std::getenv("FM4_DISC2_ROOT");
  return value ? value : "";
#endif
}

std::filesystem::path
InstalledContentPath(const std::filesystem::path &user_data_root,
                     const std::filesystem::path &package_path) {
  return user_data_root / "0000000000000000" / Hex8(kFm4TitleId) / "00000002" /
         package_path.filename();
}

std::filesystem::path
InstalledHeaderPath(const std::filesystem::path &user_data_root,
                    const std::filesystem::path &package_path) {
  return user_data_root / "0000000000000000" / Hex8(kFm4TitleId) / "Headers" /
         "00000002" / (package_path.filename().string() + ".header");
}

bool IsInstalled(const std::filesystem::path &user_data_root,
                 const std::filesystem::path &package_path) {
  return std::filesystem::is_directory(
             InstalledContentPath(user_data_root, package_path)) &&
         std::filesystem::is_regular_file(
             InstalledHeaderPath(user_data_root, package_path));
}

std::vector<std::filesystem::path>
DiscoverSourceDirectories(const std::filesystem::path &app_root,
                          const std::filesystem::path &user_data_root) {
  std::vector<std::filesystem::path> directories;
  auto add_directory = [&](std::filesystem::path directory) {
    if (directory.empty()) {
      return;
    }
    std::error_code ec;
    directory = std::filesystem::absolute(directory, ec);
    if (ec) {
      return;
    }
    for (const auto &existing : directories) {
      if (std::filesystem::equivalent(existing, directory, ec)) {
        return;
      }
      ec.clear();
    }
    directories.push_back(std::move(directory));
  };

  const std::string environment_root = Disc2EnvironmentRoot();
  if (!environment_root.empty()) {
    add_directory(environment_root);
  }

  const std::string configured_root = REXCVAR_GET(fm4_disc2_content_root);
  if (!configured_root.empty()) {
    add_directory(configured_root);
  }
  add_directory(app_root / kDisc2DropDirectory);
  add_directory(user_data_root / kDisc2DropDirectory);
  return directories;
}

} // namespace

void Fm4App::OnPreSetup(rex::RuntimeConfig& config) {
#ifdef _WIN32
  // Windows 11 puts background windows in efficiency mode (EcoQoS), slowing the
  // emulator threads while the game is not focused: audio starves and frames drop.
  PROCESS_POWER_THROTTLING_STATE throttling{};
  throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  throttling.StateMask = 0;
  const bool unthrottled = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
      &throttling, sizeof(throttling)) != FALSE;
  REXLOG_INFO("FM4 background power throttling {}", unthrottled ? "disabled" : "unchanged (not supported)");
  const auto profile = user_data_root() / "wheel.ini";
  config.input_factory = [profile](bool tool_mode) {
    return fm4::wheel::CreateInputSystem(tool_mode, profile);
  };
#endif
}

void Fm4App::OnShutdown() {
  StopQuietCapture();
#ifdef _WIN32
    fm4::wheel::ShutdownFeedback();
#endif
}

bool Fm4App::OnWindowCloseRequested() {
  StopQuietCapture();
  #ifdef _WIN32
    fm4::wheel::ShutdownFeedback();
  #endif
  return true;
}

void Fm4App::OnPostLaunchModule(rex::system::XThread*) {
  auto* graphics=runtime()?runtime()->graphics_system():nullptr;
  quiet_capture_=fm4::quiet::Capture(graphics?graphics->presenter():nullptr);
}
void Fm4App::StopQuietCapture() {
  if(quiet_capture_) { quiet_capture_->request_stop(); quiet_capture_.reset(); }
}

void Fm4App::OnPostSetup() {
#ifdef FM4_ENABLE_FORZA_PLUS_NATIVE
  // Prepared native content is receipt-owned; the stock auto-importer must
  // not mutate it after pre-entry validation.
  if (fm4_plus_proposal::Session().enabled()) return;
#endif
  if (!REXCVAR_GET(fm4_auto_install_disc2_content) || !runtime() ||
      !runtime()->kernel_state() ||
      !runtime()->kernel_state()->content_manager()) {
    return;
  }

  const uint32_t title_id = runtime()->kernel_state()->title_id();
  if (title_id != kFm4TitleId) {
    REXLOG_WARN("Skipping FM4 Disc 2 install; running title ID is {:08X}",
                title_id);
    return;
  }

  const auto user_data_root = runtime()->user_data_root();
  const auto source_directories = DiscoverSourceDirectories(
      rex::filesystem::GetExecutableFolder(), user_data_root);
  std::unordered_set<std::string> seen_packages;
  size_t installed_count = 0;
  size_t existing_count = 0;
  size_t rejected_count = 0;

  for (const auto &source_directory : source_directories) {
    if (!std::filesystem::is_directory(source_directory)) {
      continue;
    }

    std::error_code iter_ec;
    std::filesystem::recursive_directory_iterator iterator(
        source_directory,
        std::filesystem::directory_options::skip_permission_denied, iter_ec);
    const std::filesystem::recursive_directory_iterator end;
    while (!iter_ec && iterator != end) {
      const auto entry = *iterator;
      iterator.increment(iter_ec);
      if (!entry.is_regular_file() ||
          !IsDisc2PackageName(entry.path().filename().string())) {
        continue;
      }

      const auto package_path = entry.path();
      std::error_code canonical_ec;
      auto package_key =
          std::filesystem::weakly_canonical(package_path, canonical_ec)
              .string();
      if (canonical_ec) {
        package_key = std::filesystem::absolute(package_path).string();
      }
      if (!seen_packages.insert(package_key).second) {
        continue;
      }

      const auto header =
          rex::filesystem::StfsContainerDevice::ReadPackageHeader(package_path);
      if (!header) {
        REXLOG_WARN("Rejecting FM4 Disc 2 package with invalid STFS header: {}",
                    package_path.string());
        ++rejected_count;
        continue;
      }

      const auto content_type =
          static_cast<rex::system::XContentType>(header->metadata.content_type);
      const uint32_t package_title_id =
          header->metadata.execution_info.title_id;
      if (content_type != rex::system::XContentType::kMarketplaceContent ||
          package_title_id != kFm4TitleId) {
        REXLOG_WARN("Rejecting Disc 2 package {} (type {:08X}, title {:08X})",
                    package_path.filename().string(),
                    static_cast<uint32_t>(content_type), package_title_id);
        ++rejected_count;
        continue;
      }

      if (IsInstalled(user_data_root, package_path)) {
        REXLOG_INFO("FM4 Disc 2 package already installed: {}",
                    package_path.filename().string());
        ++existing_count;
        continue;
      }

      REXLOG_INFO("Installing FM4 Disc 2 package: {}", package_path.string());
      const auto result =
          runtime()->kernel_state()->content_manager()->InstallContent(
              package_path);
      if (result == 0) {
        ++installed_count;
        REXLOG_INFO("Installed FM4 Disc 2 package: {}",
                    package_path.filename().string());
      } else {
        ++rejected_count;
        REXLOG_WARN("Failed to install FM4 Disc 2 package {}: {:08X}",
                    package_path.filename().string(),
                    static_cast<uint32_t>(result));
      }
    }
    if (iter_ec) {
      REXLOG_WARN("Could not finish scanning FM4 Disc 2 folder {}: {}",
                  source_directory.string(), iter_ec.message());
    }
  }

  REXLOG_INFO(
      "FM4 Disc 2 scan complete: {} installed, {} already present, {} rejected",
      installed_count, existing_count, rejected_count);
}
