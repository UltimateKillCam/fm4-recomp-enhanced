#pragma once
// Current Streamline SR dispatch for captured FM4 frames. GPL-3.0-only.
// Diagnostic integration only: no frame generation or Neural Rendering claim.
#include <sl.h>
#include <sl_dlss.h>
#include <sl_helpers.h>
#include <array>
#include <source_location>

class DlssReplay {
  using Matrix = std::array<double, 16>;
  HMODULE module_ = nullptr;
  PFun_slShutdown* shutdown_ = nullptr;
  PFun_slSetD3DDevice* set_device_ = nullptr;
  PFun_slSetTagForFrame* tag_ = nullptr;
  PFun_slSetConstants* constants_ = nullptr;
  PFun_slEvaluateFeature* evaluate_ = nullptr;
  PFun_slGetNewFrameToken* token_ = nullptr;
  PFun_slGetFeatureFunction* function_ = nullptr;
  PFun_slDLSSSetOptions* options_ = nullptr;
  bool initialized_ = false;
  template<class T> T* Symbol(const char* name) {
    auto* symbol = reinterpret_cast<T*>(GetProcAddress(module_, name));
    if (!symbol) throw std::runtime_error(std::string("Missing Streamline entry point: ") + name);
    return symbol;
  }
  static void CheckSl(sl::Result result, std::source_location location = std::source_location::current()) {
    if (result != sl::Result::eOk) throw std::runtime_error(std::string("Streamline at line ") +
        std::to_string(location.line()) + ": " + sl::getResultAsStr(result));
  }
  static Matrix LoadMatrix(const fs::path& file) {
    auto bytes = Read(file, 64); Matrix result{};
    for (unsigned i = 0; i < 16; ++i) {
      float value; std::memcpy(&value, bytes.data() + 4 * i, 4);
      if (!std::isfinite(value)) throw std::runtime_error("Invalid captured matrix"); result[i] = value;
    }
    return result;
  }
  static Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
      for (unsigned k = 0; k < 4; ++k) result[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
    return result;
  }
  static Matrix Inverse(const Matrix& source) {
    double rows[4][8]{};
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) {
      rows[r][c] = source[r * 4 + c]; rows[r][c + 4] = r == c;
    }
    for (unsigned c = 0; c < 4; ++c) {
      unsigned pivot = c;
      for (unsigned r = c + 1; r < 4; ++r) if (std::abs(rows[r][c]) > std::abs(rows[pivot][c])) pivot = r;
      if (std::abs(rows[pivot][c]) < 1e-12) throw std::runtime_error("Singular captured matrix");
      for (unsigned k = 0; k < 8; ++k) std::swap(rows[c][k], rows[pivot][k]);
      double scale = rows[c][c]; for (double& value : rows[c]) value /= scale;
      for (unsigned r = 0; r < 4; ++r) if (r != c) {
        scale = rows[r][c]; for (unsigned k = 0; k < 8; ++k) rows[r][k] -= scale * rows[c][k];
      }
    }
    Matrix result{};
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) result[r * 4 + c] = rows[r][c + 4];
    return result;
  }
  static Matrix Presented(Matrix value) {
    // Actual scene composition rotates XY; the viewport reverses Z.
    for (unsigned r = 0; r < 4; ++r) {
      value[r * 4] *= -1; value[r * 4 + 1] *= -1;
      value[r * 4 + 2] = value[r * 4 + 3] - value[r * 4 + 2];
    }
    return value;
  }
  static sl::float4x4 SlMatrix(const Matrix& value) {
    sl::float4x4 result;
    for (unsigned r = 0; r < 4; ++r) result[r] = sl::float4(float(value[r * 4]), float(value[r * 4 + 1]), float(value[r * 4 + 2]), float(value[r * 4 + 3]));
    return result;
  }
public:
  DlssReplay(const DlssReplay&) = delete;
  DlssReplay& operator=(const DlssReplay&) = delete;
  ~DlssReplay() {
    if (initialized_ && shutdown_) shutdown_();
    if (module_) FreeLibrary(module_);
  }
  explicit DlssReplay(const fs::path& plugins) {
    auto path = fs::absolute(plugins); auto dll = path / L"sl.interposer.dll";
    module_ = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) throw std::runtime_error("Signed Streamline loader unavailable");
    auto init = Symbol<PFun_slInit>("slInit");
    shutdown_ = Symbol<PFun_slShutdown>("slShutdown"); set_device_ = Symbol<PFun_slSetD3DDevice>("slSetD3DDevice");
    tag_ = Symbol<PFun_slSetTagForFrame>("slSetTagForFrame"); constants_ = Symbol<PFun_slSetConstants>("slSetConstants");
    evaluate_ = Symbol<PFun_slEvaluateFeature>("slEvaluateFeature"); token_ = Symbol<PFun_slGetNewFrameToken>("slGetNewFrameToken");
    function_ = Symbol<PFun_slGetFeatureFunction>("slGetFeatureFunction");
    const sl::Feature features[]{sl::kFeatureDLSS}; auto folder = path.wstring(); const wchar_t* paths[]{folder.c_str()};
    sl::Preferences preferences{};
    preferences.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eDisableCLStateTracking |
        sl::PreferenceFlags::eDisableDebugText | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features; preferences.numFeaturesToLoad = 1;
    preferences.pathsToPlugins = paths; preferences.numPathsToPlugins = 1;
    preferences.logLevel = sl::LogLevel::eDefault; preferences.engine = sl::EngineType::eCustom;
    preferences.logMessageCallback = [](sl::LogType, const char* message) { std::fprintf(stderr, "%s\n", message); };
    preferences.engineVersion = "FM4-Scene-Replay-1"; preferences.projectId = "2249d991-61bc-4a73-a6c0-746df2973b51";
    CheckSl(init(preferences, sl::kSDKVersion)); initialized_ = true;
  }
  void Device(ID3D12Device* device) {
    CheckSl(set_device_(device)); void* function = nullptr;
    CheckSl(function_(sl::kFeatureDLSS, "slDLSSSetOptions", function));
    options_ = reinterpret_cast<PFun_slDLSSSetOptions*>(function);
    if (!options_) throw std::runtime_error("DLSS options function unavailable");
  }
  void Dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* colour, ID3D12Resource* depth,
                ID3D12Resource* motion, ID3D12Resource* output, const fs::path& folder,
                uint32_t width, uint32_t height, uint32_t out_width, uint32_t out_height,
                float near_plane, float far_plane, float fov, uint32_t frame) {
    sl::ViewportHandle viewport(0); sl::DLSSOptions options{};
    double scale = double(out_width) / width;
    options.mode = scale >= 2.8 ? sl::DLSSMode::eUltraPerformance : scale >= 1.9 ? sl::DLSSMode::eMaxPerformance : scale > 1.1 ? sl::DLSSMode::eMaxQuality : sl::DLSSMode::eDLAA;
    options.outputWidth = out_width; options.outputHeight = out_height;
    options.colorBuffersHDR = sl::Boolean::eFalse; options.useAutoExposure = sl::Boolean::eTrue;
    CheckSl(options_(viewport, options));
    sl::FrameToken* token = nullptr; CheckSl(token_(token, &frame));
    auto projection = Presented(LoadMatrix(folder / "view_to_clip.f32"));
    auto current = Presented(LoadMatrix(folder / "world_to_clip.f32"));
    auto previous = Presented(LoadMatrix(folder / "previous_world_to_clip.f32"));
    auto camera_world = Inverse(LoadMatrix(folder / "world_to_view.f32"));
    auto reprojection = Multiply(Inverse(current), previous);
    sl::Constants constants{};
    constants.cameraViewToClip = SlMatrix(projection); constants.clipToCameraView = SlMatrix(Inverse(projection));
    constants.clipToPrevClip = SlMatrix(reprojection); constants.prevClipToClip = SlMatrix(Inverse(reprojection));
    constants.jitterOffset = {0, 0}; constants.mvecScale = {1.0f / width, 1.0f / height};
    constants.cameraPinholeOffset = {0, 0};
    constants.cameraPos = {float(camera_world[12]), float(camera_world[13]), float(camera_world[14])};
    constants.cameraRight = {float(camera_world[0]), float(camera_world[1]), float(camera_world[2])};
    constants.cameraUp = {float(camera_world[4]), float(camera_world[5]), float(camera_world[6])};
    constants.cameraFwd = {float(camera_world[8]), float(camera_world[9]), float(camera_world[10])};
    constants.cameraNear = near_plane; constants.cameraFar = far_plane;
    constants.cameraFOV = fov; constants.cameraAspectRatio = float(width) / height;
    constants.depthInverted = sl::Boolean::eTrue; constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse; constants.reset = frame == 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    CheckSl(constants_(constants, *token, viewport));
    sl::Extent in_extent{0, 0, width, height}, out_extent{0, 0, out_width, out_height};
    sl::Resource in(sl::ResourceType::eTex2d, colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource z(sl::ResourceType::eTex2d, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource mv(sl::ResourceType::eTex2d, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource out(sl::ResourceType::eTex2d, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    sl::ResourceTag tags[]{
        {&in, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &in_extent},
        {&z, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &in_extent},
        {&mv, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &in_extent},
        {&out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &out_extent}};
    CheckSl(tag_(*token, viewport, tags, 4, list));
    const sl::BaseStructure* inputs[]{&viewport}; CheckSl(evaluate_(sl::kFeatureDLSS, *token, inputs, 1, list));
  }
  void Shutdown() {
    if (initialized_) { CheckSl(shutdown_()); initialized_ = false; }
    if (module_) { FreeLibrary(module_); module_ = nullptr; }
  }
};
