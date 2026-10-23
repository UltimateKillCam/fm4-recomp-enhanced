// Compare the affine fast path with a general pivoted solver and real draws.
// Modified 2026-10-18. GPL-3.0-only. Captures remain private.
#include "../sdk/scene_camera.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <vector>
using fm4::camera::Matrix;
static void Check(const Matrix& input) {
  Matrix general{},affine{};
  const bool g=fm4::camera::Inverse(input,general),a=fm4::camera::InverseAffine(input,affine);
  if(a!=g) throw std::runtime_error("Inverse acceptance differs from general solver");
  if(!a) return;
  for(unsigned i=0;i<16;++i)
    if(std::abs(general[i]-affine[i])>1e-9*std::max(1.0,std::abs(general[i])))
      throw std::runtime_error("Affine inverse differs from pivoted solver");
  const auto identity=fm4::camera::Multiply(input,affine);
  for(unsigned i=0;i<16;++i)
    if(std::abs(identity[i]-(i%5==0?1.0:0.0))>1e-7)
      throw std::runtime_error("Affine inverse failed independent identity product");
}
int main(int argc,char** argv) try {
  std::vector<Matrix> inputs;
  std::mt19937 random(0x464D34);
  std::uniform_real_distribution<double> angle(-3.14,3.14),scale(.2,5),position(-5000,5000);
  for(unsigned n=0;n<10000;++n) {
    const double a=angle(random),sx=scale(random),sy=scale(random),sz=scale(random);
    Matrix m={std::cos(a)*sx,std::sin(a)*sx,0,0,-std::sin(a)*sy,std::cos(a)*sy,0,0,
              0,0,sz,0,position(random),position(random),position(random),1};
    if(n%5==0) m[3]=.000001; // Projective inputs must retain the full solver.
    Check(m); inputs.push_back(m);
  }
  Matrix singular{}; Check(singular);
  Matrix tiny={1e-5,0,0,0,0,1e-5,0,0,0,0,1e-5,0,1,2,3,1};Check(tiny);
  unsigned captured=0;
  if(argc==2) for(const auto& entry:std::filesystem::directory_iterator(argv[1])) {
    if(entry.path().extension()!=".f32"||!entry.path().filename().string().starts_with("draw-")) continue;
    std::ifstream file(entry.path(),std::ios::binary|std::ios::ate);
    if(file.tellg()!=8192) throw std::runtime_error("Unexpected captured constant length");
    std::array<float,2048> constants{};file.seekg(0);file.read(reinterpret_cast<char*>(constants.data()),8192);
    if(!file) throw std::runtime_error("Truncated captured constants");
    for(unsigned offset:{16u,32u,128u*4u+16u}) {
      Matrix m{};for(unsigned i=0;i<16;++i)m[i]=constants[offset+i];
      bool finite=true;for(double value:m)finite&=std::isfinite(value);
      if(!finite)continue;
      // Very ill-conditioned non-camera constants are outside this numerical
      // comparison; actual camera transforms meet these bounds.
      Matrix inverse{};if(!fm4::camera::Inverse(m,inverse))continue;
      double maximum=0;for(double value:inverse)maximum=std::max(maximum,std::abs(value));
      if(maximum>1e8)continue;
      Check(m);inputs.push_back(m);++captured;
    }
  }
  for(bool improved:{false,true}) {
    const auto start=std::chrono::steady_clock::now();double sum=0;
    for(unsigned repeat=0;repeat<48;++repeat)for(const auto& m:inputs) {
      Matrix inverse{};
      if(improved?fm4::camera::InverseAffine(m,inverse):fm4::camera::Inverse(m,inverse))sum+=inverse[12];
    }
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::printf("%s: %.3f ns/matrix checksum %.9g\n",improved?"Affine":"General",ms*1e6/(48*inputs.size()),sum);
  }
  std::printf("PASS: 10002 synthetic cases and %u real captured transforms.\n",captured);
  return 0;
}catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
