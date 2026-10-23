// Modified 2026-10-18. GPL-3.0-only. No devices, window or physical output.
#include <rex/ppc/context.h>
#include <rex/logging.h>
#include <spdlog/sinks/base_sink.h>
#include <atomic>
#include <array>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
extern "C" __declspec(dllimport) void __imp__XamXStudioRequest(PPCContext&,uint8_t*);
struct Capture final : spdlog::sinks::base_sink<std::mutex> {
  unsigned warnings=0,flushes=0;
  void sink_it_(const spdlog::details::log_msg& message) override {
    if(std::string_view(message.payload.data(),message.payload.size()).find("XamXStudioRequest")!=std::string_view::npos)++warnings;
  }
  void flush_() override {++flushes;}
};
int main(){
  auto capture=std::make_shared<Capture>();rex::LogConfig config;
  config.extra_sinks.push_back(capture);rex::InitLogging(config);
  std::atomic<bool> changed=false;
  std::vector<std::jthread> workers;
  for(unsigned n=0;n<8;++n)workers.emplace_back([&,n]{
    PPCContext context;std::memset(&context,0x41+n,sizeof(context));
    std::array<uint8_t,sizeof(PPCContext)> before;std::memcpy(before.data(),&context,sizeof(context));
    std::array<uint8_t,128> memory;memory.fill(uint8_t(n+1));const auto original=memory;
    for(unsigned i=0;i<128;++i)__imp__XamXStudioRequest(context,memory.data());
    if(std::memcmp(before.data(),&context,sizeof(context))||memory!=original)changed=true;
  });
  workers.clear();
  const auto count=capture->warnings;rex::ShutdownLogging();
  if(changed||count!=1){std::fprintf(stderr,"FAIL: guest state changed=%d, warnings=%u\n",int(changed.load()),count);return 1;}
  std::puts("PASS: 1024 concurrent stub calls preserve all guest registers/memory and emit one warning");return 0;
}
