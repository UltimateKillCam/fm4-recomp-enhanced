// Optional local membership and content inspection. GPL-3.0-only.
#include <rex/ppc.h>
#include <rex/hook.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <atomic>
#include <cstring>
#include <mutex>

REXCVAR_DEFINE_BOOL(fm4_vip_owned, false, "FM4",
                   "Owner confirms ownership of the VIP membership")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fm4_content_availability_log, false, "FM4",
                   "Inspect DLC cars through the game's content predicates")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

extern "C" REX_FUNC(__imp__sub_82385178);
extern "C" REX_FUNC(__imp__sub_82466980);
extern "C" REX_FUNC(__imp__sub_82435E20);
extern "C" REX_FUNC(__imp__sub_82420B40);
extern "C" REX_FUNC(__imp__sub_824187E0);
extern "C" REX_FUNC(__imp__sub_8240EAB0);
extern "C" REX_FUNC(__imp__sub_82418878);
extern "C" REX_FUNC(__imp__sub_82436328);

namespace {
uint32_t U32(const uint8_t* p) {
  return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
}
const uint8_t* Bytes(uint32_t address, uint32_t size) {
  if (!address || !size || uint64_t(address)+size>0x100000000ull) return nullptr;
  auto* kernel=REX_KERNEL_STATE();
  auto* memory=kernel?kernel->memory():nullptr;
  if (!memory) return nullptr;
  auto* heap=memory->LookupHeap(address);
  rex::memory::HeapAllocationInfo info{};
  if (!heap || memory->LookupHeap(address+size-1)!=heap ||
      !heap->QueryRegionInfo(address & ~(heap->page_size()-1), &info) ||
      !(info.state & rex::memory::kMemoryAllocationCommit) ||
      !(info.protect & rex::memory::kMemoryProtectRead) ||
      uint64_t(address)+size>uint64_t(info.base_address)+info.region_size) return nullptr;
  return memory->TranslateVirtual<const uint8_t*>(address);
}
bool OriginalCode() {
  static const bool valid=[] {
    const auto* key=Bytes(0x82011588,11);
    const auto* getter=Bytes(0x82943AB8,8);
    const auto* lookup=Bytes(0x823F9720,8);
    const bool ok=key && std::memcmp(key,"Main/IsVip",11)==0 &&
        getter && U32(getter)==0x88630008 && U32(getter+4)==0x4E800020 &&
        lookup && U32(lookup)==0x54AB1838 && U32(lookup+4)==0x812300B4;
    if (!ok) REXLOG_WARN("FM4 content: executable layout differs; optional hooks disabled");
    return ok;
  }();
  return valid;
}
thread_local unsigned vip_write_depth=0;
std::atomic<bool> profile_seen=false;
struct Fm4ContentCar {uint32_t publisher, id;const char* name;const char* groups;bool autovista;};
#include "content_catalog.inc"

void Inspect(PPCContext& parent, uint8_t* base, uint32_t manager) {
  // Called only after the original cache rebuild has released its lock.
  // A changed cache gets a new snapshot, so early startup is never the final result.
  static std::mutex mutex;
  std::lock_guard<std::mutex> guard(mutex);
  if (!profile_seen.load()) return;
  const auto* object=Bytes(manager,0x108);
  if (!object) return;
  const uint32_t begin=U32(object+0xB4), end=U32(object+0xB8);
  if (!begin || end<begin || end-begin>1024*1024 || (end-begin)%8) return;
  const auto* entries=Bytes(begin,end-begin);
  if (!entries) return;
  uint64_t hash=1469598103934665603ull;
  for (uint32_t i=0;i<end-begin;++i) hash=(hash^entries[i])*1099511628211ull;
  static uint64_t last_hash=0;
  static bool last_profile=false;
  const bool profile=profile_seen.load();
  if (hash==last_hash && profile==last_profile) return;
  last_hash=hash;last_profile=profile;
  static uint32_t scratch=0;
  if (!scratch) scratch=REX_KERNEL_MEMORY()->SystemHeapAlloc(16);
  if (!scratch) return;
  auto* output=REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(scratch);
  static uint32_t snapshot=0;
  ++snapshot;
  REXLOG_INFO("FM4 content snapshot={} entries={} profile_seen={} vip_override={}",
              snapshot,(end-begin)/8,profile,REXCVAR_GET(fm4_vip_owned));
  // Each side call gets a full private register context. The original guest
  // stack is at a return boundary; reserve an extra frame below it.
  PPCContext probe=parent;
  auto prepare=[&](const Fm4ContentCar& car,uint32_t type) {
    probe=parent;probe.r1.u32-=0x100;probe.dispatch_address=0;
    probe.r3.u64=manager;probe.r4.u64=car.id;probe.r5.u64=type;
    probe.r6.u64=scratch;probe.r7.u64=scratch+1;output[0]=output[1]=0;
  };
  uint32_t available=0;
  for (const auto& car:kContentCars) {
    prepare(car,1);__imp__sub_824187E0(probe,base);
    const bool known=probe.r3.u32!=0,installed=output[0]!=0,purchased=output[1]!=0;
    prepare(car,1);__imp__sub_8240EAB0(probe,base);
    const bool visible=probe.r3.u32!=0;
    prepare(car,1);__imp__sub_82418878(probe,base);
    const bool usable=probe.r3.u32!=0;
    available+=usable;
    int av=-1;
    if (car.autovista) {
      prepare(car,2);__imp__sub_82418878(probe,base);av=probe.r3.u32!=0;
    }
    REXLOG_INFO("FM4 content car snapshot={} publisher={} id={} name={} groups={} known={} installed={} entitled={} visible={} available={} autovista={}",
                snapshot,car.publisher,car.id,car.name,car.groups,known,installed,purchased,visible,usable,av);
  }
  REXLOG_INFO("FM4 content complete snapshot={} available={}/{}; entitlement is separate from garage ownership and credits",
              snapshot,available,std::size(kContentCars));
}
}

REX_HOOK_RAW(sub_82385178) {
  const bool wanted=REXCVAR_GET(fm4_vip_owned) || REXCVAR_GET(fm4_content_availability_log);
  const bool vip=wanted && ctx.r5.u32==0x82011588 && OriginalCode();
  const uint32_t profile=ctx.r4.u32;
  __imp__sub_82385178(ctx,base);
  if (!vip) return;
  if (const auto* p=Bytes(profile,0x19);p && p[0x18]) profile_seen=true;
  // The original returns a temporary bool variant (type 0, value byte +8).
  // Never alter the profile property, its setter comparison or another key.
  const auto* value=Bytes(ctx.r3.u32,20);
  if (REXCVAR_GET(fm4_content_availability_log) && !vip_write_depth && value) {
    static std::atomic<bool> logged=false;
    if (!logged.exchange(true))
      REXLOG_INFO("FM4 VIP read profile_loaded={} type={} original={} effective={}",
                  profile_seen.load(),U32(value+16),value[8]!=0,
                  REXCVAR_GET(fm4_vip_owned) || value[8]!=0);
  }
  if (REXCVAR_GET(fm4_vip_owned) && !vip_write_depth && value && U32(value+16)==0) {
    base[ctx.r3.u32+8]=1;
    static std::once_flag once;
    std::call_once(once,[]{REXLOG_INFO("FM4 VIP: owner setting supplies Main/IsVip reads; saved profile is unchanged");});
  }
  // The content cache can finish before profile loading. A later property read
  // is also a safe return boundary after the profile-map getter released its lock.
  if (REXCVAR_GET(fm4_content_availability_log) && !vip_write_depth && profile_seen.load()) {
    if (const auto* manager=Bytes(0x82DC6584,4)) Inspect(ctx,base,U32(manager));
  }
}

REX_HOOK_RAW(sub_82466980) {
  __imp__sub_82466980(ctx,base);
  // This second accessor reads the same key through a profile-map snapshot.
  if (REXCVAR_GET(fm4_vip_owned) && !vip_write_depth && OriginalCode()) ctx.r3.u64=1;
}

REX_HOOK_RAW(sub_82435E20) {
  // A server/profile refresh must still compare and save its original state.
  struct Scope {Scope(){++vip_write_depth;}~Scope(){--vip_write_depth;}} scope;
  __imp__sub_82435E20(ctx,base);
}

REX_HOOK_RAW(sub_82420B40) {
  const uint32_t manager=ctx.r3.u32;
  __imp__sub_82420B40(ctx,base);
  if (REXCVAR_GET(fm4_content_availability_log) && OriginalCode()) Inspect(ctx,base,manager);
}

REX_HOOK_RAW(sub_82436328) {
  const uint32_t wrapper=ctx.r3.u32;
  __imp__sub_82436328(ctx,base);
  if (!REXCVAR_GET(fm4_content_availability_log) || ctx.r3.u32 || !OriginalCode()) return;
  const auto* object=Bytes(wrapper,0xA0);
  const auto* profile=object?Bytes(U32(object+0x9C),0x19):nullptr;
  const auto* manager=Bytes(0x82DC6584,4);
  if (profile && profile[0x18] && manager) {
    profile_seen=true;
    Inspect(ctx,base,U32(manager));
  }
}
