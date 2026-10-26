// Optional native implementations of the title's memory and vector leaves.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/intrinsics.h>
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

REXCVAR_DEFINE_BOOL(m1_native_more, false, "CPU",
                   "Use exact native exponent, packed decode, lookup and matrix leaves")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(r1_native_tier3, false, "CPU",
                   "Use exact native numeric, vector upload and data conversion leaves")
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

uint16_t NativeRead16(uint8_t* base, uint32_t address) {
  uint16_t value;
  std::memcpy(&value, NativeBytes(base, address), sizeof(value));
  return std::byteswap(value);
}

void NativeWrite16(uint8_t* base, uint32_t address, uint16_t value) {
  value = std::byteswap(value);
  std::memcpy(NativeBytes(base, address), &value, sizeof(value));
}

void NativeWrite32(uint8_t* base, uint32_t address, uint32_t value) {
  value = std::byteswap(value);
  std::memcpy(NativeBytes(base, address), &value, sizeof(value));
}

void NativeWrite64(uint8_t* base, uint32_t address, uint64_t value) {
  value = std::byteswap(value);
  std::memcpy(NativeBytes(base, address), &value, sizeof(value));
}

// This is the title's exponent-field operation, including its stack scratch;
// substituting ldexp would change its treatment of exceptional bit patterns.
bool NativeSetExponent(PPCContext& ctx, uint8_t* base) {
  const uint32_t stack = ctx.r1.u32;
  if (ctx.memory_trace_length || !NativeSpan(stack - 16, 8) ||
      !NativeSpan(stack + 16, 8)) return false;
  const uint64_t bits = ctx.f1.u64;
  const uint32_t sign_fraction = uint32_t(bits >> 48) & 0x800F;
  ctx.fpscr.disableFlushMode();
  NativeWrite64(base, stack + 16, bits);
  ctx.r10.u64 = uint32_t(ctx.r4.u64 + 1022) << 4;
  ctx.cr0.compare<int32_t>(int32_t(sign_fraction), 0, ctx.xer);
  ctx.r11.u64 = ctx.r10.u64 | sign_fraction;
  ctx.f1.u64 = (bits & 0x0000FFFFFFFFFFFFull) | (ctx.r11.u64 << 48);
  NativeWrite64(base, stack - 16, ctx.f1.u64);
  ctx.current_function = 0x82300850;
  ctx.current_instruction = 0x82300870;
  return true;
}

bool NativeGetExponent(PPCContext& ctx, uint8_t* base) {
  const uint32_t scratch = ctx.r1.u32 + 16;
  if (ctx.memory_trace_length || !NativeSpan(scratch, 8)) return false;
  ctx.fpscr.disableFlushMode();
  NativeWrite64(base, scratch, ctx.f1.u64);
  ctx.r11.s64 = int64_t((ctx.f1.u64 >> 52) & 0x7FF) - 1022;
  ctx.r3.s64 = ctx.r11.s16;
  ctx.current_function = 0x82342020;
  ctx.current_instruction = 0x82342024;
  return true;
}

// Normal finite values take the common decomposition path. Zero, subnormals
// and nonfinite inputs retain the generated normalization/exception path.
bool NativeSplitExponent(PPCContext& ctx, uint8_t* base) {
  const uint64_t bits = ctx.f1.u64;
  const uint32_t exponent = uint32_t(bits >> 52) & 0x7FF;
  const uint32_t stack = ctx.r1.u32;
  if (ctx.memory_trace_length || !exponent || exponent == 0x7FF ||
      !NativeSpan(stack - 16, 8) || !NativeSpan(stack + 16, 8) ||
      !NativeSpan(ctx.r4.u32, 4) || NativeRead64(base, 0x82001580) != 0)
    return false;
  ctx.fpscr.disableFlushMode();
  NativeWrite64(base, stack + 16, bits);
  ctx.f0.u64 = 0;
  ctx.cr6.compare(ctx.f1.f64, ctx.f0.f64);
  const uint32_t sign_fraction = uint32_t(bits >> 48) & 0x800F;
  ctx.cr0.compare<int32_t>(int32_t(sign_fraction), 0, ctx.xer);
  ctx.f1.u64 = (bits & 0x800FFFFFFFFFFFFFull) | 0x3FE0000000000000ull;
  NativeWrite64(base, stack - 16, ctx.f1.u64);
  ctx.r10.u64 = exponent;
  ctx.r11.u64 = exponent;
  ctx.r9.s64 = int64_t(exponent) - 1022;
  NativeWrite32(base, ctx.r4.u32, ctx.r9.u32);
  ctx.current_function = 0x82300758;
  ctx.current_instruction = 0x82300844;
  return true;
}

bool NativePackedDecode(PPCContext& ctx, uint8_t* base) {
  const uint32_t dest = ctx.r3.u32, source = ctx.r4.u32, table = ctx.r5.u32;
  if (ctx.memory_trace_length || !NativeSpan(source, 4) ||
      !NativeSpan(dest, 4) || !NativeSpan(table, 2048)) return false;
  const uint32_t first = NativeRead16(base, source) >> 6;
  const uint32_t sum = first + (NativeRead16(base, source + 2) >> 6);
  const uint32_t end = std::min(sum, 1023u) * 2;
  const uint32_t begin = first * 2;
  ctx.cr6.compare<uint32_t>(sum, 1023, ctx.xer);
  ctx.r9.u64 = (uint32_t(NativeRead16(base, table + begin)) << 6) & 0xFFC0;
  NativeWrite16(base, dest, ctx.r9.u16);
  // The first store may alias the lookup table. Preserve both subsequent reads.
  const uint32_t upper = NativeRead16(base, table + end);
  ctx.r10.u64 = NativeRead16(base, table + begin);
  ctx.r11.u64 = ((upper - ctx.r10.u32) << 6) & 0xFFC0;
  NativeWrite16(base, dest + 2, ctx.r11.u16);
  ctx.current_function = 0x826F1260;
  ctx.current_instruction = 0x826F12A8;
  return true;
}

bool NativeKeyLookup(PPCContext& ctx, uint8_t* base) {
  const uint32_t object = ctx.r3.u32;
  if (ctx.memory_trace_length || !NativeSpan(object, 32)) return false;
  const uint32_t offset = (NativeRead16(base, object + 28) & 1) ? 68 : 32;
  const uint32_t count = *NativeBytes(base, object + 31);
  const uint32_t list = object + offset;
  if (count && !NativeSpan(list, (count - 1) * 8 + 4)) return false;
  const uint64_t address = ctx.r3.u64 + offset;
  uint32_t index = 0, key = 0;
  for (; index < count; ++index) {
    key = NativeRead32(base, list + index * 8) >> 5;
    if (key == ctx.r4.u32) break;
  }
  ctx.r9.u64 = count;
  ctx.r10.u64 = index;
  ctx.r11.u64 = address + index * 8;
  ctx.cr0.compare<uint32_t>(count, 0, ctx.xer);
  if (count) {
    ctx.r8.u64 = key;
    if (index < count) ctx.cr6.compare<uint32_t>(key, ctx.r4.u32, ctx.xer);
    else ctx.cr6.compare<uint32_t>(count, count, ctx.xer);
  }
  ctx.r3.u64 = index < count ? ctx.r11.u64 : 0;
  ctx.current_function = 0x8233C638;
  ctx.current_instruction = count ? 0x8233C670 : 0x8233C664;
  return true;
}

#if defined(__clang__) && (defined(_M_X64) || defined(__x86_64__))
__attribute__((target("avx,fma")))
#endif
bool NativeMatrixVector(PPCContext& ctx, uint8_t* base) {
  const uint32_t n = ctx.r3.u32, matrix = ctx.r4.u32;
  const uint32_t vector = ctx.r5.u32, dest = ctx.r6.u32;
  const uint32_t scratch = ctx.r1.u32 - 16;
  if (ctx.memory_trace_length || ctx.r3.u64 != n || n > 16 ||
      !NativeSpan(scratch, 16)) return false;
  uint32_t matrix_bits[256], vector_bits[16];
  if (n) {
    if (ctx.r4.u64 != matrix || ctx.r5.u64 != vector || ctx.r6.u64 != dest ||
        !NativeSpan(matrix, n * n * 4) || !NativeSpan(vector, n * 4) ||
        !NativeSpan(dest, n * 4) ||
        !Disjoint(dest, n * 4, matrix, n * n * 4) ||
        !Disjoint(dest, n * 4, vector, n * 4) ||
        !Disjoint(scratch, 16, matrix, n * n * 4) ||
        !Disjoint(scratch, 16, vector, n * 4) ||
        !Disjoint(scratch, 16, dest, n * 4)) return false;
    for (uint32_t i = 0; i < n * n; ++i) {
      matrix_bits[i] = NativeRead32(base, matrix + i * 4);
      if ((matrix_bits[i] & 0x7F800000) == 0x7F800000) return false;
    }
    for (uint32_t i = 0; i < n; ++i) {
      vector_bits[i] = NativeRead32(base, vector + i * 4);
      if ((vector_bits[i] & 0x7F800000) == 0x7F800000) return false;
    }
  }
  NativeWrite64(base, scratch, ctx.r30.u64);
  NativeWrite64(base, scratch + 8, ctx.r31.u64);
  ctx.r11.u64 = ctx.r6.u64;
  ctx.cr6.compare<uint32_t>(n, 0, ctx.xer);
  if (n) {
    ctx.fpscr.disableFlushMode();
    for (uint32_t row = 0; row < n; ++row) {
      const double first = double(std::bit_cast<float>(matrix_bits[row * n]));
      ctx.f13.f64 = double(std::bit_cast<float>(vector_bits[0]));
      // Retain every single round and promotion, even on the final column:
      // promotion of a subnormal can set the host denormal exception flag.
      volatile double rounded = double(float(first * ctx.f13.f64));
      double value = rounded;
      for (uint32_t col = 1; col < n; ++col) {
        ctx.f13.f64 = value;
        ctx.f12.f64 = double(std::bit_cast<float>(matrix_bits[row * n + col]));
        const double element = double(std::bit_cast<float>(vector_bits[col]));
        rounded = double(float(std::fma(ctx.f12.f64, element, value)));
        value = rounded;
      }
      ctx.f0.f64 = value;
      NativeWrite32(base, dest + row * 4, std::bit_cast<uint32_t>(float(value)));
    }
    ctx.cr6.compare<uint32_t>(n, 1, ctx.xer);
    ctx.r6.u64 = 0;
    ctx.r8.u64 = n;
    ctx.r9.u64 = uint64_t(matrix) + n * n * 4;
    ctx.r10.u64 = n * n;
    ctx.r11.u64 = uint64_t(dest) + n * 4;
    if (n > 1) {
      ctx.r7.u64 = vector + (n - 1) * 4;
      ctx.ctr.u64 = 0;
    }
    ctx.xer.ca = 1;
    ctx.cr0.compare<int32_t>(0, 0, ctx.xer);
  }
  ctx.current_function = 0x82332930;
  ctx.current_instruction = 0x823329B8;
  return true;
}

bool NativeIntegerClass(PPCContext& ctx, uint8_t* base) {
  const uint32_t scratch = ctx.r1.u32 + 16;
  const uint64_t input = ctx.f1.u64;
  if (ctx.memory_trace_length || !NativeSpan(scratch, 8) ||
      (input & 0x7FF0000000000000ull) == 0x7FF0000000000000ull ||
      !Disjoint(scratch, 8, 0x82001520, 8)) return false;
  const uint64_t scale = NativeRead64(base, 0x82001520);
  if ((scale & 0x7FF0000000000000ull) == 0x7FF0000000000000ull)
    return false;
  ctx.fpscr.disableFlushMode();
  NativeWrite64(base, scratch, input);
  ctx.r11.u64 = (input >> 48) & 0x7FF0;
  ctx.cr0.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
  ctx.current_function = 0x8230F480;
  ctx.current_instruction = 0x8230F484;
  if (!ctx.r11.u32) {
    ctx.r11.u64 = (input >> 32) & 0xFFFFF;
    ctx.cr0.compare<int32_t>(ctx.r11.s32, 0, ctx.xer);
    ctx.current_instruction = 0x8230F490;
    if (ctx.r11.u32) { ctx.r3.u64 = 0; return true; }
    ctx.r11.u64 = uint32_t(input);
    ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
    ctx.current_instruction = 0x8230F49C;
    if (ctx.r11.u32) { ctx.r3.u64 = 0; return true; }
  }
  ctx.f0.s64 = ctx.f1.f64 > double(INT64_MAX) ? INT64_MAX :
      simde_mm_cvtsd_si64(simde_mm_load_sd(&ctx.f1.f64));
  ctx.f0.f64 = double(ctx.f0.s64);
  ctx.cr6.compare(ctx.f0.f64, ctx.f1.f64);
  ctx.r3.u64 = 0;
  if (!ctx.cr6.eq) return true;
  ctx.r11.s64 = -2113929216;
  ctx.f0.f64 = ctx.f1.f64 * std::bit_cast<double>(scale);
  // The second conversion also obeys the inherited host rounding mode.
  ctx.f13.s64 = std::isnan(ctx.f0.f64) ? INT64_MIN :
      ctx.f0.f64 > double(INT64_MAX) ? INT64_MAX :
      simde_mm_cvtsd_si64(simde_mm_load_sd(&ctx.f0.f64));
  ctx.f13.f64 = double(ctx.f13.s64);
  ctx.cr6.compare(ctx.f13.f64, ctx.f0.f64);
  ctx.r3.u64 = ctx.cr6.eq ? 2 : 1;
  ctx.current_instruction = 0x8230F4BC;
  return true;
}

#if defined(__clang__) && (defined(_M_X64) || defined(__x86_64__))
__attribute__((target("avx,fma")))
#endif
bool NativeQuantize16(PPCContext& ctx, uint8_t* base) {
  const uint32_t scratch = ctx.r1.u32 - 16;
  if (ctx.memory_trace_length || !NativeSpan(scratch, 8) ||
      !Disjoint(scratch, 8, 0x82000000, 5508) ||
      (ctx.f1.u64 & 0x7FF0000000000000ull) == 0x7FF0000000000000ull)
    return false;
  const uint32_t addresses[] = {0x82000F48, 0x82000F34, 0x82000DB4, 0x820014B8};
  uint32_t bits[4];
  for (unsigned i = 0; i < 4; ++i) {
    bits[i] = NativeRead32(base, addresses[i]);
    if ((bits[i] & 0x7F800000) == 0x7F800000) return false;
  }
  ctx.fpscr.disableFlushMode();
  ctx.r11.s64 = ctx.r10.s64 = -2113929216;
  ctx.f13.f64 = double(std::bit_cast<float>(bits[0]));
  ctx.cr6.compare(ctx.f1.f64, ctx.f13.f64);
  if (!ctx.cr6.lt) {
    ctx.f13.f64 = ctx.f1.f64;
    ctx.f0.f64 = double(std::bit_cast<float>(bits[1]));
    ctx.cr6.compare(ctx.f1.f64, ctx.f0.f64);
    if (!ctx.cr6.gt) ctx.f0.f64 = ctx.f13.f64;
  } else {
    ctx.f0.f64 = ctx.f13.f64;
  }
  ctx.f13.f64 = double(std::bit_cast<float>(bits[2]));
  ctx.f12.f64 = double(std::bit_cast<float>(bits[3]));
  ctx.f0.f64 = double(float(std::fma(ctx.f0.f64, ctx.f13.f64, ctx.f12.f64)));
  ctx.f0.s64 = std::isnan(ctx.f0.f64) ? INT64_MIN :
      ctx.f0.f64 > double(INT64_MAX) ? INT64_MAX :
      simde_mm_cvttsd_si64(simde_mm_load_sd(&ctx.f0.f64));
  NativeWrite64(base, scratch, ctx.f0.u64);
  ctx.r3.u64 = ctx.f0.u64 & 0xFFFF;
  ctx.current_function = 0x82879328;
  ctx.current_instruction = 0x8287936C;
  return true;
}

bool NativeIndirectCompare(PPCContext& ctx, uint8_t* base) {
  if (ctx.memory_trace_length || !NativeSpan(ctx.r3.u32, 4) ||
      !NativeSpan(ctx.r4.u32, 4)) return false;
  const uint32_t a = NativeRead32(base, ctx.r3.u32);
  const uint32_t b = NativeRead32(base, ctx.r4.u32);
  if (!NativeSpan(a, 4) || !NativeSpan(b, 4)) return false;
  const uint32_t aa = NativeRead32(base, a), bb = NativeRead32(base, b);
  if (aa > 0xFFFFFFAF || bb > 0xFFFFFFAF ||
      !NativeSpan(aa + 80, 4) || !NativeSpan(bb + 80, 4)) return false;
  ctx.r11.u64 = NativeRead32(base, aa + 80);
  ctx.r10.u64 = NativeRead32(base, bb + 80);
  ctx.r3.u64 = ctx.r10.u64 - ctx.r11.u64;
  ctx.current_function = 0x822C4918;
  ctx.current_instruction = 0x822C492C;
  return true;
}

void NativeLoadVector(PPCVRegister& dest, uint8_t* base, uint32_t address) {
  for (unsigned i = 0; i < 16; ++i)
    dest.u8[i] = NativeBytes(base, address)[15 - i];
}

bool NativeVectorUpload(PPCContext& ctx, uint8_t* base) {
  const uint32_t object = ctx.r3.u32, source = ctx.r5.u32, count = ctx.r6.u32;
  const uint32_t offset = uint32_t(ctx.r4.u32 + 120) << 4;
  const uint64_t dest64 = uint64_t(object) + offset;
  if (ctx.memory_trace_length || ctx.r3.u64 != object ||
      ctx.r5.u64 != source || ctx.r6.u64 != count || count > 4096 ||
      ((object | source) & 15) || dest64 > 0xFFFFFFFFull ||
      !NativeSpan(object, 8) || !NativeSpan(uint32_t(dest64), count * 16) ||
      !NativeSpan(source, count * 16) ||
      !Disjoint(uint32_t(dest64), count * 16, source, count * 16) ||
      !Disjoint(object, 8, uint32_t(dest64), count * 16)) return false;
  const uint32_t dest = uint32_t(dest64), groups = count / 4, tail = count % 4;
  ctx.r10.u64 = dest64 + count * 16;
  ctx.r9.u64 = tail;
  ctx.r8.u64 = groups ? 48 : 128;
  ctx.r5.u64 = 16;
  if (groups) {
    const uint32_t last = source + (groups - 1) * 64;
    NativeLoadVector(ctx.v63, base, last);
    NativeLoadVector(ctx.v62, base, last + 16);
    NativeLoadVector(ctx.v61, base, last + 32);
    NativeLoadVector(ctx.v60, base, last + 48);
    ctx.v59 = ctx.v60;
    ctx.r4.u64 = 64;
    ctx.r6.u64 = 32;
  }
  if (tail) {
    NativeLoadVector(ctx.v63, base, source + count * 16 - 16);
    std::memset(&ctx.v62, 0, sizeof(ctx.v62));
  }
  if (count) {
    ctx.ctr.u64 = 0;
    std::memcpy(NativeBytes(base, dest), NativeBytes(base, source), count * 16);
  }
  ctx.cr6.compare<uint32_t>(tail, 0, ctx.xer);
  ctx.r11.u64 = NativeRead64(base, object) | ctx.r7.u64;
  NativeWrite64(base, object, ctx.r11.u64);
  ctx.current_function = 0x823164E0;
  ctx.current_instruction = 0x823165B0;
  return true;
}

bool NativeGatherVector(PPCContext& ctx, uint8_t* base) {
  const uint32_t scratch = ctx.r1.u32 - 16, dest = ctx.r3.u32 & ~15u;
  if (ctx.memory_trace_length || (ctx.r1.u32 & 15) ||
      !NativeSpan(ctx.r4.u32 + 4, 4) || ctx.r4.u32 > 0xFFFFFFFB ||
      !NativeSpan(scratch, 16) || !NativeSpan(dest, 16)) return false;
  const uint32_t object = NativeRead32(base, ctx.r4.u32 + 4);
  if (!NativeSpan(object, 108)) return false;
  const uint32_t offset = ctx.r5.u32 << 2;
  const uint32_t pointers[] = {NativeRead32(base, object + 56),
      NativeRead32(base, object + 104), NativeRead32(base, object + 80)};
  uint32_t bits[3];
  for (unsigned i = 0; i < 3; ++i) {
    const uint64_t address = uint64_t(pointers[i]) + offset;
    if (address > 0xFFFFFFFFull || !NativeSpan(uint32_t(address), 4)) return false;
    bits[i] = NativeRead32(base, uint32_t(address));
    if ((bits[i] & 0x7F800000) == 0x7F800000) return false;
  }
  ctx.fpscr.disableFlushMode();
  ctx.r11.u64 = pointers[2]; ctx.r8.u64 = pointers[0]; ctx.r7.u64 = pointers[1];
  ctx.r10.u64 = offset; ctx.r9.u64 = ctx.r1.u64 - 16;
  ctx.f0.f64 = double(std::bit_cast<float>(bits[0]));
  ctx.f13.f64 = double(std::bit_cast<float>(bits[1]));
  ctx.f12.f64 = double(std::bit_cast<float>(bits[2]));
  NativeWrite32(base, scratch, std::bit_cast<uint32_t>(float(ctx.f0.f64)));
  NativeWrite32(base, scratch + 4, std::bit_cast<uint32_t>(float(ctx.f13.f64)));
  NativeWrite32(base, scratch + 8, std::bit_cast<uint32_t>(float(ctx.f12.f64)));
  NativeLoadVector(ctx.v63, base, scratch);
  std::memmove(NativeBytes(base, dest), NativeBytes(base, scratch), 16);
  ctx.current_function = 0x822FF5C0;
  ctx.current_instruction = 0x822FF5EC;
  return true;
}

// Keep the single rounding after each scalar operation in the matrix-to-
// quaternion conversion. A conventional quaternion formula reassociates it.
#pragma STDC FENV_ACCESS ON
bool NativeMatrixQuaternion(PPCContext& ctx, uint8_t* base) {
  const uint32_t stack = ctx.r1.u32, scratch = stack - 16;
  if (ctx.memory_trace_length || (stack & 15) || !NativeSpan(scratch, 16))
    return false;
  uint32_t m[3][3];
  const uint32_t rows[] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32};
  for (unsigned row = 0; row < 3; ++row) {
    if (!NativeSpan(rows[row], 12)) return false;
    for (unsigned col = 0; col < 3; ++col) {
      m[row][col] = NativeRead32(base, rows[row] + col * 4);
      if ((m[row][col] & 0x7F800000) == 0x7F800000) return false;
    }
  }
  const uint32_t addresses[] = {0x82000F48, 0x82000F34, 0x82000EBC,
                                0x82000F70, 0x82000F30};
  uint32_t constants[5];
  for (unsigned i = 0; i < 5; ++i) {
    constants[i] = NativeRead32(base, addresses[i]);
    if ((constants[i] & 0x7F800000) == 0x7F800000) return false;
  }
  const uint64_t half_bits = NativeRead64(base, 0x820014E0);
  // These fixed title constants keep unused exceptional intermediates from
  // acquiring different host flags after scalar register allocation.
  if (constants[0] != 0 || constants[1] != 0x3F800000 ||
      constants[2] != 0x40000000 || constants[4] != 0xBF800000 ||
      (constants[3] != 0x3E800000 && constants[3] != 0x3F000000) ||
      (half_bits != 0x3FD0000000000000ull &&
       half_bits != 0x3FE0000000000000ull))
    return false;
  ctx.fpscr.disableFlushMode();
  auto round = [](double value) { return double(float(value)); };
  auto value = [&](unsigned row, unsigned col) {
    return double(std::bit_cast<float>(m[row][col]));
  };
  double f13 = value(1, 1), f12 = value(2, 2);
  double f5 = round(f12 + f13);
  double f11 = value(0, 0), f0 = value(1, 0), f10 = value(2, 0);
  const double f4 = double(std::bit_cast<float>(constants[0]));
  double f9 = value(0, 1), f8 = value(2, 1), f7 = value(0, 2), f6 = value(1, 2);
  f5 = round(f5 + f11);
  ctx.cr6.compare(f5, f4);
  if (ctx.cr6.gt) {
    f9 = round(f9 - f0); f11 = round(f6 - f8); f10 = round(f10 - f7);
    f13 = double(std::bit_cast<float>(constants[2]));
    f0 = double(std::bit_cast<float>(constants[1]));
    f8 = round(f5 + f0);
    f12 = std::bit_cast<double>(half_bits);
    f8 = round(std::sqrt(f8));
    f13 = round(f8 * f13); f8 = round(f0 / f13); f12 = f13 * f12;
    f11 = round(f11 * f8); f10 = round(f10 * f8); f13 = round(f9 * f8);
    f12 = round(f12);
  } else {
    ctx.cr6.compare(f11, f13);
    bool first = ctx.cr6.gt;
    if (first) { ctx.cr6.compare(f11, f12); first = ctx.cr6.gt; }
    if (first) {
      f9 = round(f9 + f0); f8 = round(f6 - f8); f7 = round(f7 + f10);
      f10 = double(std::bit_cast<float>(constants[2]));
      f0 = double(std::bit_cast<float>(constants[1]));
      f6 = round(f11 + f0);
      f11 = double(std::bit_cast<float>(constants[3]));
      f13 = round(f6 - f13); f13 = round(f13 - f12);
      f13 = round(std::sqrt(f13)); f13 = round(f13 * f10);
      f6 = round(f0 / f13); f11 = round(f13 * f11);
      f12 = round(f8 * f6); f10 = round(f9 * f6); f13 = round(f7 * f6);
    } else {
      ctx.cr6.compare(f13, f12);
      f8 = round(f6 + f8);
      if (ctx.cr6.gt) {
        f9 = round(f9 + f0); f0 = double(std::bit_cast<float>(constants[1]));
        f6 = round(f13 + f0); f13 = double(std::bit_cast<float>(constants[3]));
        f7 = round(f10 - f7); f10 = double(std::bit_cast<float>(constants[2]));
        f11 = round(f6 - f11); f12 = round(f11 - f12);
        f12 = round(std::sqrt(f12)); f12 = round(f12 * f10);
        f6 = round(f0 / f12); f10 = round(f12 * f13);
        f12 = round(f7 * f6); f11 = round(f9 * f6); f13 = round(f8 * f6);
      } else {
        f9 = round(f9 - f0); f0 = double(std::bit_cast<float>(constants[1]));
        f6 = round(f12 + f0); f12 = double(std::bit_cast<float>(constants[3]));
        f7 = round(f7 + f10); f10 = double(std::bit_cast<float>(constants[2]));
        f11 = round(f6 - f11); f13 = round(f11 - f13);
        f13 = round(std::sqrt(f13)); f13 = round(f13 * f10);
        f10 = round(f0 / f13); f13 = round(f13 * f12);
        f12 = round(f9 * f10); f11 = round(f7 * f10); f10 = round(f8 * f10);
      }
    }
  }
  f8 = round(f11 - f0); f6 = round(f13 - f0);
  f7 = round(f10 - f0); f5 = round(f12 - f0);
  f9 = double(std::bit_cast<float>(constants[4]));
  f11 = f8 >= 0.0 ? f0 : f11; f13 = f6 >= 0.0 ? f0 : f13;
  f10 = f7 >= 0.0 ? f0 : f10; f0 = f5 >= 0.0 ? f0 : f12;
  f12 = round(f11 - f9); f7 = round(f13 - f9);
  f8 = round(f10 - f9); f6 = round(f0 - f9);
  f12 = f12 >= 0.0 ? f11 : f9;
  NativeWrite32(base, scratch, std::bit_cast<uint32_t>(float(f12)));
  f13 = f7 >= 0.0 ? f13 : f9;
  NativeWrite32(base, scratch + 8, std::bit_cast<uint32_t>(float(f13)));
  f12 = f8 >= 0.0 ? f10 : f9;
  NativeWrite32(base, scratch + 4, std::bit_cast<uint32_t>(float(f12)));
  f0 = f6 >= 0.0 ? f0 : f9;
  NativeWrite32(base, scratch + 12, std::bit_cast<uint32_t>(float(f0)));
  ctx.f0.f64 = f0; ctx.f4.f64 = f4; ctx.f5.f64 = f5; ctx.f6.f64 = f6;
  ctx.f7.f64 = f7; ctx.f8.f64 = f8; ctx.f9.f64 = f9; ctx.f10.f64 = f10;
  ctx.f11.f64 = f11; ctx.f12.f64 = f12; ctx.f13.f64 = f13;
  ctx.r9.s64 = ctx.r11.s64 = -2113929216;
  ctx.r10.u64 = ctx.r1.u64 - 16;
  NativeLoadVector(ctx.v1, base, scratch);
  ctx.current_function = 0x823348C0;
  ctx.current_instruction = 0x82334AA0;
  return true;
}
#pragma STDC FENV_ACCESS OFF

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

#define NATIVE_MORE_HOOK(address, implementation)                           \
  extern "C" REX_FUNC(__imp__sub_##address);                                \
  REX_HOOK_RAW(sub_##address) {                                            \
    if (REXCVAR_GET(m1_native_more) && ctx.dispatch_address == 0) {          \
      rex::system::PollCheckpointBoundary(ctx, 0x##address);                \
      if (implementation(ctx, base)) return;                              \
    }                                                                     \
    __imp__sub_##address(ctx, base);                                       \
  }

NATIVE_MORE_HOOK(82300758, NativeSplitExponent)
NATIVE_MORE_HOOK(82300850, NativeSetExponent)
NATIVE_MORE_HOOK(82342020, NativeGetExponent)
NATIVE_MORE_HOOK(826F1260, NativePackedDecode)
NATIVE_MORE_HOOK(8233C638, NativeKeyLookup)
NATIVE_MORE_HOOK(82332930, NativeMatrixVector)

#undef NATIVE_MORE_HOOK

#define NATIVE_TIER3_HOOK(address, implementation)                          \
  extern "C" REX_FUNC(__imp__sub_##address);                               \
  REX_HOOK_RAW(sub_##address) {                                           \
    if (REXCVAR_GET(r1_native_tier3) && ctx.dispatch_address == 0) {        \
      rex::system::PollCheckpointBoundary(ctx, 0x##address);               \
      if (implementation(ctx, base)) return;                             \
    }                                                                    \
    __imp__sub_##address(ctx, base);                                      \
  }

NATIVE_TIER3_HOOK(8230F480, NativeIntegerClass)
NATIVE_TIER3_HOOK(82879328, NativeQuantize16)
NATIVE_TIER3_HOOK(823164E0, NativeVectorUpload)
NATIVE_TIER3_HOOK(822C4918, NativeIndirectCompare)
NATIVE_TIER3_HOOK(823348C0, NativeMatrixQuaternion)
NATIVE_TIER3_HOOK(822FF5C0, NativeGatherVector)

#undef NATIVE_TIER3_HOOK
