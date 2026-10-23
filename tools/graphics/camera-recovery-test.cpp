// Compare C++ camera recovery with independently decoded real captures.
// GPL-3.0-only, 2026-10-16. All inputs stay private.
#include "../sdk/scene_camera.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
static std::vector<float> Read(const std::filesystem::path& p,size_t count) {
  std::ifstream f(p,std::ios::binary|std::ios::ate);
  if(!f||f.tellg()!=std::streamoff(count*4)) throw std::runtime_error("Invalid captured matrix size");
  std::vector<float> v(count); f.seekg(0); f.read(reinterpret_cast<char*>(v.data()),count*4);
  if(!f) throw std::runtime_error("Truncated matrix"); return v;
}
int wmain(int argc,wchar_t** argv) try {
  if(argc!=4) { std::puts("camera-recovery-test <draw.f32> <independent-view.f32> <independent-vp.f32>"); return 2; }
  auto c=Read(argv[1],512*4),view=Read(argv[2],16),vp=Read(argv[3],16);
  fm4::camera::Camera camera{};
  if(!fm4::camera::Candidate(c.data(),camera)) throw std::runtime_error("Valid captured camera rejected");
  double max_view=0,max_vp=0;
  for(unsigned i=0;i<16;++i) { max_view=std::max(max_view,std::abs(camera.view[i]-view[i])); max_vp=std::max(max_vp,std::abs(camera.vp[i]-vp[i])); }
  if(max_view>1e-3||max_vp>1e-3) throw std::runtime_error("Camera disagrees with independent capture decoder");
  std::array<float,48> invalid{};
  if(fm4::camera::Candidate(invalid.data(),camera)) throw std::runtime_error("Singular camera accepted");
  std::printf("PASS: independent captured view/VP max errors %.9g / %.9g; invalid camera rejected\n",max_view,max_vp);
  return 0;
} catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
