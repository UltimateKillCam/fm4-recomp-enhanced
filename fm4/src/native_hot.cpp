// Optional native implementations of the title's memory and vector leaves.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/system/checkpoint.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_BOOL(n4_native_hot, false, "CPU",
                   "Use native byte fill and aligned byte copy leaves")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(n4_native_transform, false, "CPU",
                   "Use the exact scalar native three-component transform leaf")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

uint8_t* NativeBytes(uint8_t* base, uint32_t address) {
  // Match generated Win32 guest-physical alias addressing.
#if REX_PLATFORM_WIN32
  return base + address + (address >= 0xE0000000u ? 0x1000ull : 0ull);
#else
  return base + address;
#endif
}

uint64_t NativeBackingAddress(uint32_t address) {
  // The XEX aliases and physical/writeback views can overlap despite having
  // disjoint guest virtual addresses. Compare their backing offsets too.
  if (address >= 0xA0000000u) {
    return 0x100000000ull + (address & 0x1FFFFFFFu) +
           (address >= 0xE0000000u ? 0x1000ull : 0ull);
  }
  if (address >= 0x90000000u) return address - 0x10000000u;
  if (address >= 0x7F000000u && address < 0x80000000u)
    return 0x100000000ull + (address - 0x7F000000u);
  return address;
}

bool NativeSpan(uint32_t address, uint32_t size) {
  const uint64_t end = uint64_t(address) + size;
  return address >= 0x1000 && end <= 0x100000000ull &&
         (!size || NativeBackingAddress(uint32_t(end - 1)) ==
                       NativeBackingAddress(address) + size - 1);
}

bool Disjoint(uint32_t a, uint32_t na, uint32_t b, uint32_t nb) {
  const uint64_t backing_a = NativeBackingAddress(a);
  const uint64_t backing_b = NativeBackingAddress(b);
  return backing_a + na <= backing_b || backing_b + nb <= backing_a;
}

uint64_t NativeRead64(uint8_t* base, uint32_t address) {
  uint64_t value;
  std::memcpy(&value, NativeBytes(base, address), sizeof(value));
  return std::byteswap(value);
}

uint32_t NativeRead32(uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, NativeBytes(base, address), sizeof(value));
  return std::byteswap(value);
}

// Preserve the leaf's volatile registers too: generated callers may observe
// CR0/CR1/CR6/CR7 and XER even though the source-level API only returns r3.
bool NativeCopy(PPCContext& ctx, uint8_t* base) {
  const uint32_t dest = ctx.r3.u32, source = ctx.r4.u32, size = ctx.r5.u32;
  const uint32_t saved_return = ctx.r1.u32 - 8;
  if (ctx.memory_trace_length || ctx.r3.u64 != dest || ctx.r4.u64 != source ||
      ctx.r5.u64 != size || ((dest | source) & 7) ||
      !NativeSpan(dest, size) || !NativeSpan(source, size) ||
      !NativeSpan(saved_return, 8) || !Disjoint(dest, size, source, size) ||
      !Disjoint(saved_return, 8, dest, size) ||
      !Disjoint(saved_return, 8, source, size)) return false;

  const uint32_t prefix = size >= 128 ? (128 - (dest & 127)) & 127 : 0;
  const uint32_t remaining = size - prefix;
  const uint32_t blocks = remaining >> 7, tail = remaining & 127;
  const uint32_t tail_words = tail >> 3, tail_bytes = tail & 7;
  const uint32_t copied = size - tail;
  uint64_t saved = std::byteswap(ctx.r3.u64);
  std::memcpy(NativeBytes(base, saved_return), &saved, sizeof(saved));

  // Compute the observable state of the final bulk iteration, then its tail.
  ctx.xer.ca = 1;
  ctx.cr7.compare<uint32_t>(size, 128, ctx.xer);
  ctx.r5.u64 = remaining;
  ctx.r4.u64 = source + copied - 8;
  ctx.cr0.compare<uint32_t>(blocks, 0, ctx.xer);
  if (blocks) {
    const uint32_t prefetches = (((remaining + 127) >> 7) - 1) % 8 + 1;
    ctx.r9.u64 = 8 + 128 * prefetches;
    ctx.r10.u64 = 8;
    ctx.r11.u64 = uint64_t(source + prefix - 8) + remaining - ctx.r9.u64;
    ctx.r12.u64 = uint64_t(dest + prefix - 8) + remaining;
    ctx.r8.u64 = NativeRead64(base, source + copied - 16);
    ctx.cr0.compare<uint32_t>(ctx.r4.u32, ctx.r11.u32, ctx.xer);
    if (tail && !ctx.cr0.lt) ctx.r8.s64 = -1;
    ctx.ctr.u64 = 0;
  } else if (prefix) {
    ctx.ctr.u64 = 0;
  }
  ctx.r7.u64 = tail_words;
  ctx.r6.u64 = tail_bytes;
  ctx.cr6.compare<uint32_t>(tail_bytes, 0, ctx.xer);
  if (tail_words) {
    ctx.r7.u64 = NativeRead64(base, source + copied + tail_words * 8 - 8);
    ctx.r4.u64 += tail_words * 8;
    ctx.ctr.u64 = 0;
  }
  ctx.cr1.compare<uint32_t>(tail_bytes, 4, ctx.xer);
  if (tail_bytes == 4) {
    ctx.r6.u64 = 0;
    ctx.r5.u64 = NativeRead32(base, source + size - 4);
    ctx.cr0.compare<uint32_t>(0, 0, ctx.xer);
  } else if (tail_bytes) {
    ctx.r4.u64 = source + size - 1;
    ctx.r7.u64 = *NativeBytes(base, source + size - 1);
    ctx.ctr.u64 = 0;
  }
  std::memcpy(NativeBytes(base, dest), NativeBytes(base, source), size);
  ctx.current_function = 0x827659B0;
  ctx.current_instruction = tail_bytes == 4 ? 0x82765AA0 : 0x82765A84;
  return true;
}

bool NativeFill(PPCContext& ctx, uint8_t* base) {
  const uint32_t dest = ctx.r3.u32, size = ctx.r5.u32;
  if (ctx.memory_trace_length || ctx.r5.u64 != size || !NativeSpan(dest, size))
    return false;
  const uint32_t prefix = std::min(size, (4 - (dest & 3)) & 3);
  const uint32_t remaining = size - prefix;
  const uint32_t tail = remaining & 3;
  const uint64_t value = ctx.r4.u64;
  std::memset(NativeBytes(base, dest), int(value & 255), size);
  ctx.r4.u64 = (value & 0xFFFFFFFF00000000ull) |
              uint32_t(value & 255) * 0x01010101u;
  ctx.r5.u64 = remaining;
  ctx.r6.u64 = ctx.r3.u64 + size - tail;
  ctx.r0.u64 = tail;
  ctx.cr0.compare<int32_t>(int32_t(tail), 0, ctx.xer);
  // The three-byte tail has only two decrement-and-branch instructions.
  ctx.ctr.u64 = tail == 3 ? 1 : 0;
  ctx.current_function = 0x827660C0;
  uint32_t last_store = prefix ? 0x827660D4 : 0x827660C0;
  if (remaining >= 16) last_store = 0x82766104;
  if (remaining & 12) last_store = 0x8276611C + (((remaining & 12) >> 2) - 1) * 12;
  if (tail) last_store = 0x82766148 + (tail - 1) * 8;
  ctx.current_instruction = last_store;
  return true;
}

#if defined(__clang__) && (defined(_M_X64) || defined(__x86_64__))
// The runtime already targets x86-64-v3. Keep FMA lowering local to this leaf
// so the six exact double operations need no separate CRT calls.
__attribute__((target("avx,fma")))
#endif
bool NativeTransform(PPCContext& ctx, uint8_t* base) {
  const uint32_t stack = ctx.r1.u32, matrix = ctx.r4.u32;
  const uint32_t dest = ctx.r3.u32 & ~15u;
  if (ctx.memory_trace_length || (stack & 15) ||
      !NativeSpan(stack - 16, 16) || !NativeSpan(stack + 32, 16) ||
      !NativeSpan(matrix, 44) || !NativeSpan(dest, 16) ||
      !Disjoint(matrix, 44, stack - 16, 16) ||
      !Disjoint(matrix, 44, stack + 32, 16) ||
      !Disjoint(dest, 16, stack - 16, 16) ||
      !Disjoint(dest, 16, stack + 32, 16)) return false;

  uint32_t matrix_bits[3][3];
  for (uint32_t row = 0; row < 3; ++row) {
    for (uint32_t col = 0; col < 3; ++col) {
      const uint32_t bits = NativeRead32(base, matrix + row * 16 + col * 4);
      if ((bits & 0x7F800000u) == 0x7F800000u) return false;
      matrix_bits[row][col] = bits;
    }
  }
  // VMX memory order reverses all sixteen bytes. Keep the fourth result lane
  // from the original stack scratch, including its arbitrary bit pattern.
  for (uint32_t lane = 1; lane < 4; ++lane)
    if ((ctx.v1.u32[lane] & 0x7F800000u) == 0x7F800000u) return false;

  ctx.fpscr.disableFlushMode();
  double m[3][3];
  for (uint32_t row = 0; row < 3; ++row)
    for (uint32_t col = 0; col < 3; ++col)
      m[row][col] = double(std::bit_cast<float>(matrix_bits[row][col]));
  const double x = double(ctx.v1.f32[3]);
  const double y = double(ctx.v1.f32[2]);
  const double z = double(ctx.v1.f32[1]);
  for (uint32_t i = 0; i < 16; ++i)
    NativeBytes(base, stack + 32)[i] = ctx.v1.u8[15 - i];
  // Retain the generated double FMA followed by single-precision rounding at
  // each guest fmuls/fmadds. A float FMA or reassociated dot product differs.
  ctx.f10.f64 = double(float(m[1][1] * y));
  ctx.f11.f64 = double(float(m[1][0] * y));
  double result_z = double(float(m[1][2] * y));
  ctx.f9.f64 = m[2][0]; ctx.f8.f64 = m[2][1]; ctx.f7.f64 = m[2][2];
  ctx.f10.f64 = double(float(std::fma(m[2][1], z, ctx.f10.f64)));
  ctx.f11.f64 = double(float(std::fma(m[2][0], z, ctx.f11.f64)));
  result_z = double(float(std::fma(m[2][2], z, result_z)));
  ctx.f6.f64 = m[0][0]; ctx.f5.f64 = m[0][1]; ctx.f4.f64 = m[0][2];
  const float result_x = float(std::fma(m[0][0], x, ctx.f11.f64));
  // The first result temporarily occupied f13 before being stored. Its
  // promotion can raise the host denormal flag even though f13 is overwritten.
  volatile double stored_x = double(result_x);
  const float result_y = float(std::fma(m[0][1], x, ctx.f10.f64));
  ctx.f0.f64 = double(float(std::fma(m[0][2], x, result_z)));
  ctx.f12.f64 = x;
  ctx.f13.f64 = double(result_y);
  const uint32_t bits[3] = {std::byteswap(std::bit_cast<uint32_t>(float(stored_x))),
                            std::byteswap(std::bit_cast<uint32_t>(result_y)),
                            std::byteswap(std::bit_cast<uint32_t>(float(ctx.f0.f64)))};
  std::memcpy(NativeBytes(base, stack - 16), bits, sizeof(bits));
  for (uint32_t i = 0; i < 16; ++i)
    ctx.v63.u8[i] = NativeBytes(base, stack - 16)[15 - i];
  std::memcpy(NativeBytes(base, dest), NativeBytes(base, stack - 16), 16);
  ctx.r11.u64 = ctx.r1.u64 - 16;
  ctx.r12.u64 = 32;
  ctx.current_function = 0x823099C8;
  ctx.current_instruction = 0x82309A30;
  return true;
}

}  // namespace

extern "C" REX_FUNC(__imp__sub_827659B0);
REX_HOOK_RAW(sub_827659B0) {
  if (REXCVAR_GET(n4_native_hot) && ctx.dispatch_address == 0) {
    rex::system::PollCheckpointBoundary(ctx, 0x827659B0);
    if (NativeCopy(ctx, base)) return;
  }
  __imp__sub_827659B0(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_827660C0);
REX_HOOK_RAW(sub_827660C0) {
  if (REXCVAR_GET(n4_native_hot) && ctx.dispatch_address == 0) {
    rex::system::PollCheckpointBoundary(ctx, 0x827660C0);
    if (NativeFill(ctx, base)) return;
  }
  __imp__sub_827660C0(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_823099C8);
REX_HOOK_RAW(sub_823099C8) {
  if (REXCVAR_GET(n4_native_transform) && ctx.dispatch_address == 0) {
    rex::system::PollCheckpointBoundary(ctx, 0x823099C8);
    if (NativeTransform(ctx, base)) return;
  }
  __imp__sub_823099C8(ctx, base);
}
