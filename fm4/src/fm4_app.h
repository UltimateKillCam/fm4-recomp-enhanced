// Wheel/controller integration. Modified 2026-10-17. GPL-3.0-only.
// fm4 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <thread>

class Fm4App : public rex::ReXApp {
public:
  using rex::ReXApp::ReXApp;
  ~Fm4App() override { StopQuietCapture(); }

  static std::unique_ptr<rex::ui::WindowedApp>
  Create(rex::ui::WindowedAppContext &ctx) {
    return std::unique_ptr<Fm4App>(new Fm4App(ctx, "fm4", PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  void OnPreSetup(rex::RuntimeConfig& config) override;
  // void OnLoadXexImage(std::string& xex_image) override {}
#ifdef FM4_ENABLE_FORZA_PLUS_NATIVE
  bool SetupEnvironment() override;
  void OnConfigurePaths(rex::PathConfig& paths) override;
  bool ConstructRuntime(const rex::PathConfig& paths) override;
  void OnPostLoadXexImage() override;
#endif
  void OnPostSetup() override;
  void OnPostLaunchModule(rex::system::XThread* thread) override;
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  void OnShutdown() override;
  bool OnWindowCloseRequested() override;
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
private:
  void StopQuietCapture();
  std::unique_ptr<std::jthread> quiet_capture_;
};
