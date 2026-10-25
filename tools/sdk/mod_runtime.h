// Optional mod session integration. GPL-3.0-only.
#pragma once
#include <filesystem>
#include <string_view>
namespace rex { struct PathConfig; class Runtime; }
namespace rex::ui { class ImGuiDrawer; }
namespace rex::filesystem { class Entry; }
namespace fm4::mods {
void Configure(rex::PathConfig& paths);
bool CheckPaths(const rex::PathConfig& paths);
void Start(rex::Runtime* runtime);
void Attach(rex::ui::ImGuiDrawer* drawer);
void Stop();
rex::filesystem::Entry* Overlay(std::string_view path, rex::filesystem::Entry* root = nullptr);
bool IsOverlay(rex::filesystem::Entry* entry);
bool Active();
bool Capturing();
void DrawActions();
}
