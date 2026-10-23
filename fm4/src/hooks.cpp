// Scoped FM4 startup-media hooks.

#include <rex/ppc.h>

void EnableTurn10MovieSkip(PPCRegister& r29) {
  // The following store writes descriptor + 0x28.
  r29.u64 = 1;
}

void RestoreTurn10MovieMode(PPCRegister& r29) {
  // Do not change the mode used by later movie descriptors.
  r29.u64 = 2;
}

void EnableClarksonVoiceoverSkip(PPCRegister& r3) {
  // The previous instruction loaded the allowskipvoiceover byte into r3.
  r3.u64 = 1;
}

// Track streaming memory: FM4 sizes its main asset pool from a settings value (124 MB x 1.08).
// With some random AI fields the race-start streaming exceeds it, the loaders get no memory
// and silently drop track assets (missing Bernese Alps road sections for the whole race).
// The PC build has spare guest physical memory, so give that pool 8 MB more budget.
// Applies only to the stock value. Added 2026-10-19.
#include <rex/hook.h>
#include <rex/logging.h>

extern "C" REX_FUNC(__imp__sub_8237CFD8);
REX_HOOK_RAW(sub_8237CFD8) {
  __imp__sub_8237CFD8(ctx, base);
  static bool applied = false;
  if (applied || !ctx.r3.u32) return;
  uint8_t* budget = base + ctx.r3.u32 + 1640;
  const uint32_t value = uint32_t(budget[0]) << 24 | uint32_t(budget[1]) << 16 |
                         uint32_t(budget[2]) << 8 | budget[3];
  if (value != 0x07C00000u) return;  // settings not populated yet, or not the stock budget
  constexpr uint32_t kBudget = 0x07C00000u + 0x00800000u;
  budget[0] = uint8_t(kBudget >> 24); budget[1] = uint8_t(kBudget >> 16);
  budget[2] = uint8_t(kBudget >> 8); budget[3] = uint8_t(kBudget);
  applied = true;
  REXLOG_INFO("FM4 asset pool budget raised from 124 MB to 132 MB");
}
