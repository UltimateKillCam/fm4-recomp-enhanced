// Optional local trainer guest bridge. GPL-3.0-only.
// The runtime exports are absent in normal builds, leaving guest state intact.
#ifdef _WIN32
#include <windows.h>
namespace {
template<typename T> T Fm4ToolExport(const char* name) {
  auto module=GetModuleHandleW(L"rexruntime.dll");
  return module?reinterpret_cast<T>(GetProcAddress(module,name)):nullptr;
}
bool Fm4TrainerArmed() {
  static auto active=Fm4ToolExport<bool(*)()>("Fm4ToolsTrainerActive");
  return active && active();
}
double Fm4TrainerDelta(double measured) {
  static auto scale=Fm4ToolExport<double(*)(double)>("Fm4ToolsScaleDelta");
  return scale?scale(measured):measured;
}
}
extern "C" REX_FUNC(__imp__sub_823E6300);
extern "C" REX_FUNC(__imp__sub_82405FC0);
REX_HOOK_RAW(sub_823E6300) {
  static auto take=Fm4ToolExport<int64_t(*)()>("Fm4ToolsTakeCredits");
  static auto pending=Fm4ToolExport<bool(*)()>("Fm4ToolsHasCredits");
  const uint32_t wrapper=ctx.r3.u32;
  uint32_t profile=0;
  if(take && pending && pending() && !(wrapper&3)) {
    if(auto p=Fm4TimingBytes(wrapper,0,4)) profile=Fm4TimingU32(p);
  }
  __imp__sub_823E6300(ctx,base);
  if(!profile || (profile&3) || !Fm4TimingBytes(profile,0,280))return;
  // Getter reads Main/Credits; setter updates that property, clamps to
  // 999999999, marks it dirty and dispatches FM4's own credits listeners.
  constexpr uint32_t getter[]={0x3D608201,0x80830000,0x38610050,0x38ABF314};
  constexpr uint32_t setter[]={0x3D603B9A,0x7C7D1B78,0x616BC9FF,0x7C9E2378};
  static const bool valid=Fm4TimingCode(0x823E6310,getter)&&Fm4TimingCode(0x82405FCC,setter);
  if(!valid)return;
  const int64_t credits=take();if(credits<0 || credits>999999999)return;
  auto saved=ctx;
  ctx.r3.u64=profile;ctx.r4.u64=uint32_t(credits);ctx.dispatch_address=0;
  try { __imp__sub_82405FC0(ctx,base); }
  catch(...) { ctx=saved;throw; }
  ctx=saved;ctx.r3.u64=uint32_t(credits);
  REXLOG_INFO("FM4 trainer: credits applied through guest setter: {}",credits);
}
#else
namespace {
bool Fm4TrainerArmed() {return false;}
double Fm4TrainerDelta(double measured) {return measured;}
}
#endif
