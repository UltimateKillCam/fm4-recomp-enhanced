// Optional native batching of the title's cascaded audio filter.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/checkpoint.h>

#include <bit>
#include <cstdint>
#include <cstring>

#if REX_ARCH_AMD64 && REX_PLATFORM_WIN32
#include <immintrin.h>
#include <intrin.h>
#endif

REXCVAR_DEFINE_BOOL(r3_host_lean, false, "CPU",
                   "Batch the six-channel audio filter with exact native arithmetic")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

extern "C" REX_FUNC(__imp__sub_82B68810);

namespace {

#if defined(__clang__) && REX_ARCH_AMD64 && REX_PLATFORM_WIN32

uint8_t* AudioBytes(uint8_t* base, uint32_t address) {
  return base + address + (address >= 0xE0000000u ? 0x1000ull : 0ull);
}

uint64_t AudioBacking(uint32_t address) {
  if (address >= 0xA0000000u)
    return 0x100000000ull + (address & 0x1FFFFFFFu) +
           (address >= 0xE0000000u ? 0x1000ull : 0ull);
  if (address >= 0x90000000u) return address - 0x10000000u;
  if (address >= 0x7F000000u && address < 0x80000000u)
    return 0x100000000ull + address - 0x7F000000u;
  return address;
}

bool AudioSpan(uint32_t address, uint32_t size) {
  const uint64_t end = uint64_t(address) + size;
  return address >= 0x1000 && end <= 0x100000000ull &&
         AudioBacking(uint32_t(end - 1)) == AudioBacking(address) + size - 1;
}

bool AudioDisjoint(uint32_t a, uint32_t na, uint32_t b, uint32_t nb) {
  return AudioBacking(a) + na <= AudioBacking(b) ||
         AudioBacking(b) + nb <= AudioBacking(a);
}

uint32_t AudioRead(uint8_t* base, uint32_t address) {
  uint32_t bits;
  std::memcpy(&bits, AudioBytes(base, address), sizeof(bits));
  return std::byteswap(bits);
}

void AudioWrite(uint8_t* base, uint32_t address, uint32_t bits) {
  bits = std::byteswap(bits);
  std::memcpy(AudioBytes(base, address), &bits, sizeof(bits));
}

bool AudioFmaAvailable() {
  static const bool available = [] {
    int info[4];
    __cpuid(info, 1);
    constexpr int required = (1 << 12) | (1 << 27) | (1 << 28);
    return (info[2] & required) == required && (_xgetbv(0) & 6) == 6;
  }();
  return available;
}

// Every guest single operation computes in double and rounds to single.
// A float FMA would lose the guest's intermediate double rounding.
__attribute__((target("avx,fma")))
__m128d AudioRound(__m128d value) {
  return _mm_cvtps_pd(_mm_cvtpd_ps(value));
}

__attribute__((target("avx,fma")))
void AudioFilterPrefix(uint8_t* base, uint32_t filter, uint32_t input,
                       uint32_t output, uint32_t count) {
  constexpr uint32_t noise_address = 0x82DA5760;
  __m128d history[3], result[3];
  for (uint32_t pair = 0; pair < 3; ++pair) {
    const uint32_t at = filter + 320 + pair * 16;
    history[pair] = _mm_set_pd(std::bit_cast<float>(AudioRead(base, at + 8)),
                              std::bit_cast<float>(AudioRead(base, at)));
    result[pair] = _mm_set_pd(std::bit_cast<float>(AudioRead(base, at + 12)),
                             std::bit_cast<float>(AudioRead(base, at + 4)));
  }
  const __m128d gain = _mm_set1_pd(std::bit_cast<float>(AudioRead(base, filter + 448)));
  const __m128d feedback = _mm_set1_pd(std::bit_cast<float>(AudioRead(base, filter + 452)));
  uint32_t noise_bits = AudioRead(base, noise_address);
  for (uint32_t sample = 0; sample < count; ++sample) {
    __m128d signal[3];
    const __m128d noise = _mm_set1_pd(std::bit_cast<float>(noise_bits));
    // Read all channels before writing any: the guest permits in-place audio.
    for (uint32_t pair = 0; pair < 3; ++pair) {
      const uint32_t at = input + sample * 24 + pair * 8;
      signal[pair] = _mm_set_pd(std::bit_cast<float>(AudioRead(base, at + 4)),
                               std::bit_cast<float>(AudioRead(base, at)));
      signal[pair] = AudioRound(_mm_add_pd(signal[pair], noise));
    }
    for (uint32_t pair = 0; pair < 3; ++pair) {
      const __m128d previous = AudioRound(_mm_mul_pd(feedback, history[pair]));
      history[pair] = AudioRound(_mm_fmadd_pd(signal[pair], gain, previous));
      const __m128d scaled = AudioRound(_mm_mul_pd(gain, history[pair]));
      result[pair] = AudioRound(_mm_fmadd_pd(feedback, result[pair], scaled));
      const __m128 packed = _mm_cvtpd_ps(result[pair]);
      alignas(16) uint32_t bits[4];
      _mm_store_si128(reinterpret_cast<__m128i*>(bits), _mm_castps_si128(packed));
      const uint32_t at = output + sample * 24 + pair * 8;
      AudioWrite(base, at, bits[0]);
      AudioWrite(base, at + 4, bits[1]);
    }
    noise_bits ^= 0x80000000u;
  }
  for (uint32_t pair = 0; pair < 3; ++pair) {
    alignas(16) uint32_t h[4], y[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(h), _mm_castps_si128(_mm_cvtpd_ps(history[pair])));
    _mm_store_si128(reinterpret_cast<__m128i*>(y), _mm_castps_si128(_mm_cvtpd_ps(result[pair])));
    const uint32_t at = filter + 320 + pair * 16;
    AudioWrite(base, at, h[0]);
    AudioWrite(base, at + 4, y[0]);
    AudioWrite(base, at + 8, h[1]);
    AudioWrite(base, at + 12, y[1]);
  }
  AudioWrite(base, noise_address, noise_bits);
}

bool NativeAudioFilter(PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t noise = 0x82DA5760;
  const uint32_t filter = ctx.r3.u32, input = ctx.r4.u32;
  const uint32_t output = ctx.r5.u32, count = ctx.r6.u32;
  if (ctx.memory_trace_length || ctx.dispatch_address || !AudioFmaAvailable() ||
      (ctx.fpscr.getcsr() & 0x1F80u) != 0x1F80u ||
      ctx.r3.u64 != filter || ctx.r4.u64 != input || ctx.r5.u64 != output ||
      ctx.r6.u64 != count || ctx.r7.u64 != 6 || count < 2 || count > 2048 ||
      !AudioSpan(filter, 456) || !AudioSpan(input, count * 24) ||
      !AudioSpan(output, count * 24) || !AudioSpan(ctx.r1.u32 - 256, 256)) return false;
  const uint32_t size = count * 24;
  if ((AudioBacking(input) != AudioBacking(output) &&
       !AudioDisjoint(input, size, output, size)) ||
      !AudioDisjoint(filter, 456, input, size) ||
      !AudioDisjoint(filter, 456, output, size) ||
      !AudioDisjoint(ctx.r1.u32 - 256, 256, input, size) ||
      !AudioDisjoint(ctx.r1.u32 - 256, 256, output, size) ||
      !AudioDisjoint(ctx.r1.u32 - 256, 256, filter, 456) ||
      !AudioDisjoint(noise, 4, input, size) ||
      !AudioDisjoint(noise, 4, output, size) ||
      !AudioDisjoint(noise, 4, filter, 456) ||
      !AudioDisjoint(noise, 4, ctx.r1.u32 - 256, 256)) return false;
  const uint32_t gain = AudioRead(base, filter + 448) & 0x7FFFFFFFu;
  if (AudioRead(base, 0x82000F34) != 0x3F800000u ||
      AudioRead(base, 0x82000F48) != 0 ||
      AudioRead(base, filter + 312) != AudioRead(base, filter + 316) ||
      (AudioRead(base, filter + 312) & 0x7FFFFFFFu) > 0x7F7FFFFFu ||
      (AudioRead(base, filter + 36) >> 16 & 63) != 63 ||
      gain == 0 || gain >= 0x3F800000u ||
      (AudioRead(base, filter + 452) & 0x7FFFFFFFu) > 0x3F800000u ||
      (AudioRead(base, noise) & 0x7FFFFFFFu) > 0x3F800000u) return false;
  // Keep nonfinite/extreme samples on the fully instrumented guest path.
  // Integer validation leaves the floating environment untouched on rejection.
  for (uint32_t at = 320; at < 368; at += 4)
    if ((AudioRead(base, filter + at) & 0x7FFFFFFFu) > 0x41800000u) return false;
  for (uint32_t at = 0; at < size; at += 4)
    if ((AudioRead(base, input + at) & 0x7FFFFFFFu) > 0x41800000u) return false;

  rex::system::PollCheckpointBoundary(ctx, 0x82B68810);
  ctx.fpscr.disableFlushMode();
  AudioFilterPrefix(base, filter, input, output, count - 1);
  // The original final sample reconstructs every volatile FP/GPR value, CR,
  // stack save, instruction marker and return sequence without an ABI shortcut.
  ctx.r4.u64 = input + (count - 1) * 24;
  ctx.r5.u64 = output + (count - 1) * 24;
  ctx.r6.u64 = 1;
  __imp__sub_82B68810(ctx, base);
  ctx.r4.u64 = input;
  ctx.r5.u64 = output;
  ctx.r6.u64 = count;
  static thread_local bool announced = false;
  if (!announced) {
    REXLOG_INFO("Native six-channel audio filter active");
    announced = true;
  }
  return true;
}

#endif

}  // namespace

REX_HOOK_RAW(sub_82B68810) {
#if defined(__clang__) && REX_ARCH_AMD64 && REX_PLATFORM_WIN32
  if (REXCVAR_GET(r3_host_lean) && NativeAudioFilter(ctx, base)) return;
#endif
  __imp__sub_82B68810(ctx, base);
}
