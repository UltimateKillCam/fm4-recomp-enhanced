// Local in-game tools storage. GPL-3.0-only.
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace fm4::tools {
namespace fs = std::filesystem;
using Json = nlohmann::json;
struct Store {
  fs::path saves, state, photos, archive;
  unsigned keep = 8;
  std::string profile;
  Store(fs::path saves, fs::path state, fs::path photos, fs::path archive,
        std::string profile, unsigned keep = 8);
  std::vector<std::string> Backups() const;
  std::string Backup();
  void RequestRestore(const std::string& name);
  bool RestoreAtLaunch();
  unsigned ExportPhotos(const fs::path& package);
  bool Unlock(uint32_t id, uint64_t filetime);
  Json Unlocks() const;
  void RememberPhotoPackage(const fs::path& package);
  std::vector<fs::path> PhotoPackages() const;
};
Json ReadJson(const fs::path& path);
void WriteJson(const fs::path& path, const Json& doc);
bool Within(const fs::path& child, const fs::path& parent);
double ScaledDelta(double measured, double speed, bool armed);
}
