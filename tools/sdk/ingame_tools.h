// Optional in-game tools. GPL-3.0-only.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string_view>
#include "ingame_io.h"
namespace rex::system { class KernelState; }
namespace rex::filesystem { class Entry; }
namespace fm4::tools {
class GuestIo : public io::Reader {public: GuestIo();};
void Start(rex::system::KernelState* kernel);
void Stop();
void DrawTabs();
void DrawToasts();
void TrainerClosed();
void PollDiagnostic(bool& visible);
void Package(std::string_view name, const std::filesystem::path& path, bool closed);
void Written(rex::filesystem::Entry* entry);
void Achievement(rex::system::KernelState* kernel, uint32_t user, uint32_t id);
uint64_t AchievementTime(uint32_t user, uint32_t id);
}
