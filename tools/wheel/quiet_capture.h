// Requested guest-output captures. Modified 2026-10-17. GPL-3.0-only.
// Uses the runtime's screenshot API; no desktop capture or window activation.
#pragma once
#include <rex/ui/presenter.h>
#include <rex/logging.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
namespace fm4::quiet {
inline std::unique_ptr<std::jthread> Capture(rex::ui::Presenter* presenter) {
  const char* directory=std::getenv("FM4_CAPTURE_DIR");
  if(!presenter||!directory||!std::getenv("FM4_BACKGROUND")||
     !std::getenv("FM4_QUIET_CAPTURE")) return {};
  const std::filesystem::path folder(directory);
  return std::make_unique<std::jthread>([presenter,folder](std::stop_token stop) {
    unsigned count=0;
    while(!stop.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if(stop.stop_requested()) break;
      try {
        std::error_code error;
        auto request=folder/"capture.request";
        if(count>=256||!std::filesystem::exists(request,error)) continue;
        rex::ui::RawImage image;
        if(!presenter->CaptureGuestOutput(image)) continue;
        const auto stats=presenter->GetGuestFrameStats();
        const auto stats_time=std::chrono::steady_clock::now().time_since_epoch();
        if(stop.stop_requested()) break;
        if(!image.width||!image.height||image.width>7680||image.height>4320||
           image.stride<image.width*4||image.data.size()<size_t(image.stride)*image.height)
          throw std::runtime_error("Invalid guest capture dimensions");
        std::filesystem::create_directories(folder);
        if(!std::filesystem::remove(request,error)) continue;
        auto path=folder/("frame-quiet-"+std::to_string(++count)+".ppm");
        std::ofstream output(path,std::ios::binary);
        output<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
        std::vector<uint8_t> row(size_t(image.width)*3);
        for(uint32_t y=0;y<image.height;++y) {
          auto* pixels=image.data.data()+size_t(y)*image.stride;
          for(uint32_t x=0;x<image.width;++x) std::memcpy(row.data()+size_t(x)*3,pixels+size_t(x)*4,3);
          output.write(reinterpret_cast<const char*>(row.data()),row.size());
        }
        if(!output) throw std::runtime_error("Guest capture write failed");
        auto metrics_path=path; metrics_path.replace_extension(".json");
        std::ofstream metrics(metrics_path);
        metrics<<"{\"guest_frame_count\":"<<stats.frame_count
            <<",\"monotonic_ns\":"<<std::chrono::duration_cast<std::chrono::nanoseconds>(stats_time).count()
            <<",\"smoothed_fps\":"<<stats.fps
            <<",\"smoothed_frame_ms\":"<<stats.frame_time_ms<<"}\n";
        if(!metrics) throw std::runtime_error("Guest frame-stat capture write failed");
        REXLOG_INFO("FM4 quiet guest capture: {} ({}x{})",path.string(),image.width,image.height);
      } catch(const std::exception& e) { REXLOG_WARN("FM4 quiet capture: {}",e.what()); }
    }
  });
}
}
