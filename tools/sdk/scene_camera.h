#pragma once
// FM4 camera recovery and reprojection math. GPL-3.0-only, 2026-10-16.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace fm4::camera {
using Matrix = std::array<double, 16>;
inline Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix out{};
  for (unsigned r=0;r<4;++r) for (unsigned c=0;c<4;++c)
    for (unsigned k=0;k<4;++k) out[r*4+c]+=a[r*4+k]*b[k*4+c];
  return out;
}
inline bool Inverse(const Matrix& in, Matrix& out) {
  double a[4][8]{};
  for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) { a[r][c]=in[r*4+c]; a[r][c+4]=r==c; }
  for(unsigned c=0;c<4;++c) {
    unsigned p=c;
    for(unsigned r=c+1;r<4;++r) if(std::abs(a[r][c])>std::abs(a[p][c])) p=r;
    if(!std::isfinite(a[p][c]) || std::abs(a[p][c])<1e-12) return false;
    for(unsigned k=0;k<8;++k) std::swap(a[c][k],a[p][k]);
    double v=a[c][c]; for(double& x:a[c]) x/=v;
    for(unsigned r=0;r<4;++r) if(r!=c) { v=a[r][c]; for(unsigned k=0;k<8;++k) a[r][k]-=v*a[c][k]; }
  }
  for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) out[r*4+c]=a[r][c+4];
  return true;
}
inline bool InverseAffine(const Matrix& m, Matrix& out) {
  // These transforms are row-vector affine matrices. Keep the general solver
  // for even slightly projective inputs and ill-conditioned determinants.
  if(m[3]!=0 || m[7]!=0 || m[11]!=0 || m[15]!=1) return Inverse(m,out);
  const double a=m[0],b=m[1],c=m[2],d=m[4],e=m[5],f=m[6],g=m[8],h=m[9],i=m[10];
  const double A=e*i-f*h,B=c*h-b*i,C=b*f-c*e;
  const double D=f*g-d*i,E=a*i-c*g,F=c*d-a*f;
  const double G=d*h-e*g,H=b*g-a*h,I=a*e-b*d;
  const double determinant=a*A+b*D+c*G;
  if(!std::isfinite(determinant) || std::abs(determinant)<1e-12) return Inverse(m,out);
  const double reciprocal=1/determinant;
  out={A*reciprocal,B*reciprocal,C*reciprocal,0,
       D*reciprocal,E*reciprocal,F*reciprocal,0,
       G*reciprocal,H*reciprocal,I*reciprocal,0,0,0,0,1};
  for(unsigned col=0;col<3;++col)
    out[12+col]=-(m[12]*out[col]+m[13]*out[4+col]+m[14]*out[8+col]);
  for(double value:out) if(!std::isfinite(value)) return Inverse(m,out);
  return true;
}
inline Matrix Presented(Matrix m) {
  for(unsigned r=0;r<4;++r) { m[r*4]*=-1; m[r*4+1]*=-1; m[r*4+2]=m[r*4+3]-m[r*4+2]; }
  return m;
}
struct Camera { Matrix view{}, projection{}, vp{}; float near_plane=0, far_plane=0, fov=0; };
inline bool Candidate(const float* constants, Camera& out) {
  Matrix mvp{},mv{},world{},inverse{};
  for(unsigned i=0;i<48;++i) if(!std::isfinite(constants[i])) return false;
  for(unsigned i=0;i<16;++i) { mvp[i]=constants[i]; mv[i]=constants[i+16]; world[i]=constants[i+32]; }
  for(const auto* m:{&mv,&world}) for(unsigned i=0;i<4;++i)
    if(std::abs((*m)[i*4+3]-(i==3?1.0:0.0))>1e-5) return false;
  if(!InverseAffine(mv,inverse)) return false;
  auto p=Multiply(inverse,mvp);
  for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c)
    if(!((r==c && r<3)||(r==2&&c==3)||(r==3&&c==2)) && std::abs(p[r*4+c])>2e-4) return false;
  const double x=p[0],y=p[5],a=p[10],b=p[14];
  if(std::abs(p[11]-1)>1e-4 || !(std::abs(x)>.1&&std::abs(x)<100&&std::abs(y)>.1&&std::abs(y)<100&&x*y>0&&a>1&&a<2&&b<0)) return false;
  double n=-b/a,f=-b/(a-1);
  if(!(n>.001&&f>n&&f<1e8) || !InverseAffine(world,inverse)) return false;
  auto v=Multiply(inverse,mv);
  for(unsigned r=0;r<3;++r) for(unsigned c=0;c<3;++c) {
    double dot=0; for(unsigned k=0;k<3;++k) dot+=v[r*4+k]*v[c*4+k];
    if(std::abs(dot-(r==c?1.0:0.0))>2e-3) return false;
  }
  out.view=v; out.projection=p; out.vp=Multiply(v,p);
  out.near_plane=float(n); out.far_plane=float(f); out.fov=float(2*std::atan(1/std::abs(y)));
  return true;
}
inline bool SameView(const Camera& a,const Camera& b,double tolerance=.025) {
  for(unsigned i=0;i<16;++i) if(std::abs(a.view[i]-b.view[i])>tolerance) return false;
  return true;
}
// Static-track shaders retain a different far plane from the car shaders.
// Recover their actual world-to-clip matrix from c128..135, then verify its
// projection against the independently recovered current camera view.
inline bool TrackVp(const float* c, Matrix& vp) {
  Matrix mvp{},world{},inverse{};
  for(unsigned i=0;i<32;++i) if(!std::isfinite(c[i])) return false;
  for(unsigned r=0;r<4;++r) for(unsigned col=0;col<4;++col) {
    mvp[r*4+col]=c[col*4+r]; world[r*4+col]=c[16+r*4+col];
  }
  for(unsigned r=0;r<4;++r) if(std::abs(world[r*4+3]-(r==3?1.0:0.0))>1e-5) return false;
  if(!InverseAffine(world,inverse)) return false;
  vp=Multiply(inverse,mvp); return true;
}
inline bool TrackProjection(const Matrix& vp,Camera& camera) {
  Matrix inverse{}; if(!InverseAffine(camera.view,inverse)) return false;
  auto p=Multiply(inverse,vp);
  for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c)
    if(!((r==c&&r<3)||(r==2&&c==3)||(r==3&&c==2))&&std::abs(p[r*4+c])>5e-4) return false;
  if(std::abs(p[11]-1)>1e-4 || std::abs(p[0]-camera.projection[0])>.002 ||
      std::abs(p[5]-camera.projection[5])>.002 || !(p[10]>1&&p[10]<2&&p[14]<0)) return false;
  double n=-p[14]/p[10],f=-p[14]/(p[10]-1);
  if(!(n>.001&&f>n&&f<1e8) || std::abs(n-camera.near_plane)>.002) return false;
  // Remove floating point reconstruction noise from mathematically zero terms.
  for(unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c)
    if(!((r==c&&r<3)||(r==2&&c==3)||(r==3&&c==2))) p[r*4+c]=0;
  p[11]=1; camera.projection=p; camera.vp=Multiply(camera.view,p);
  camera.near_plane=float(n); camera.far_plane=float(f); return true;
}
inline bool SceneDraw(uint32_t surface,uint32_t depth,const float* viewport) {
  return (surface&0x3fff)==1280 && ((surface>>16)&3)!=0 && (depth&2) &&
    ((depth>>4)&7)==6 && std::abs(std::abs(viewport[0])-640)<.01 &&
    std::abs(std::abs(viewport[2])-360)<.01 && std::abs(viewport[4]+1)<.001 && std::abs(viewport[5]-1)<.001;
}
} // namespace fm4::camera
