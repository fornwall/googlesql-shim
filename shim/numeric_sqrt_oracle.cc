// A differential oracle for `numeric::sqrt` (Rust) against the real
// `googlesql::NumericValue::Sqrt`. It generates a deterministic spread of
// NUMERIC values, computes each square root with GoogleSQL, and prints
// `input_packed_hi input_packed_lo status output_packed_hi output_packed_lo`
// per line (packed values are the __int128 the Rust side stores as an i128;
// non-negative here, so the high word is the top 64 bits). The Rust harness in
// smallquery's `crates/smallquery/tests/numeric_sqrt_differential.rs` reads
// this and checks its port reproduces every output bit for bit.
//
// Build and run (here; the cargo test runs in fornwall/smallquery):
//   bazel run //shim:numeric_sqrt_oracle > /tmp/sqrt.txt
//   NUMERIC_SQRT_ORACLE=/tmp/sqrt.txt cargo test -p smallquery --test \
//       numeric_sqrt_differential

#include <cstdint>
#include <cstdio>
#include <random>

#include "googlesql/public/numeric_value.h"

using googlesql::NumericValue;

namespace {

// kNumericMax = 10^38 - 1.
constexpr unsigned __int128 kNumericMax =
    (static_cast<unsigned __int128>(5421010862427522170ULL) << 64) |
    687399551400673279ULL;

void Emit(unsigned __int128 mag) {
  __int128 packed = static_cast<__int128>(mag);
  auto value = NumericValue::FromPackedInt(packed);
  uint64_t in_hi = static_cast<uint64_t>(mag >> 64);
  uint64_t in_lo = static_cast<uint64_t>(mag);
  if (!value.ok()) {
    // Should not happen: mag is always in range below.
    std::printf("%llu %llu bad 0 0\n", (unsigned long long)in_hi,
                (unsigned long long)in_lo);
    return;
  }
  auto result = value->Sqrt();
  if (!result.ok()) {
    std::printf("%llu %llu err 0 0\n", (unsigned long long)in_hi,
                (unsigned long long)in_lo);
    return;
  }
  unsigned __int128 out =
      static_cast<unsigned __int128>(result->as_packed_int());
  uint64_t out_hi = static_cast<uint64_t>(out >> 64);
  uint64_t out_lo = static_cast<uint64_t>(out);
  std::printf("%llu %llu ok %llu %llu\n", (unsigned long long)in_hi,
              (unsigned long long)in_lo, (unsigned long long)out_hi,
              (unsigned long long)out_lo);
}

unsigned __int128 Rand128(std::mt19937_64* rng) {
  return (static_cast<unsigned __int128>((*rng)()) << 64) |
         static_cast<unsigned __int128>((*rng)());
}

}  // namespace

int main() {
  std::mt19937_64 rng(0xC0FFEEULL);

  // Structured edges: zero, one, the maximum, powers of ten and their
  // neighbours (the decompose-into-[0.5,2) path pivots on the exponent).
  Emit(0);
  Emit(kNumericMax);
  unsigned __int128 p = 1;
  for (int i = 0; i < 38; ++i) {
    Emit(p);
    if (p > 0) Emit(p - 1);
    if (p < kNumericMax) Emit(p + 1);
    p *= 10;
  }
  // Perfect squares of scale-9 integers, so termination-on-exact is exercised.
  for (unsigned __int128 root = 1; root <= 316227766; root += (root / 4) + 1) {
    unsigned __int128 sq = root * 1000000000ULL;  // root as NUMERIC
    // (root*10^9)^2 / 10^9 stays in range only for modest roots; guard it.
    unsigned __int128 square = (sq / 1000000000ULL) * sq;  // == root^2 * 10^9
    if (square <= kNumericMax) Emit(square);
  }

  // Uniform over the whole packed range.
  for (int i = 0; i < 1000000; ++i) {
    Emit(Rand128(&rng) % (kNumericMax + 1));
  }
  // Values below 1.0 (< 10^9), where the fractional iteration dominates.
  for (int i = 0; i < 1000000; ++i) {
    Emit(Rand128(&rng) % 1000000000ULL);
  }
  // Medium magnitudes (< 10^19), spanning the low integer range.
  for (int i = 0; i < 1000000; ++i) {
    Emit(Rand128(&rng) % 10000000000000000000ULL);
  }
  return 0;
}
