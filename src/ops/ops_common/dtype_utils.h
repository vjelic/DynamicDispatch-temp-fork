// Copyright (c) 2025 Advanced Micro Devices, Inc
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef DTYPE_UTILS
#define DTYPE_UTILS

//
// Helper functions to pack/unpack bfloat16 and int4
// data types, which aren't natively supported by the CPU
//

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

#include <algorithm>
#include <array>

namespace ryzenai {

/*
* converts float to bfloat16 by rounding the LSB to nearest even
@param x is a floating point value
@return bfloat16 value in uint16_t variable
*/
static inline uint16_t float_to_bfloat16(float x) {
  uint32_t i;
  uint8_t *src = (uint8_t *)&x;
  uint8_t *tmp = (uint8_t *)&i;
  // copy float to uint32_t
  std::memcpy(tmp, src, sizeof(float));
  // round to nearest even
  uint32_t lsb = (i >> 16) & 0x1;
  uint32_t bias = 0x7fff + lsb;
  i += bias;
  // extract upper half of input
  uint16_t y = uint16_t(i >> 16);
  return y;
}

static inline uint32_t as_uint(const float x) { return *(uint32_t *)&x; }
static inline float as_float(const uint32_t x) { return *(float *)&x; }

static inline float float16_to_float(
    const uint16_t x) { // IEEE-754 16-bit floating-point format (without
                        // infinity): 1-5-10, exp-15, +-131008.0,
                        // +-6.1035156E-5, +-5.9604645E-8, 3.311 digits
  const uint32_t e = (x & 0x7C00) >> 10; // exponent
  const uint32_t m = (x & 0x03FF) << 13; // mantissa
  const uint32_t v =
      as_uint((float)m) >>
      23; // evil log2 bit hack to count leading zeros in denormalized format
  return as_float(
      (x & 0x8000) << 16 | (e != 0) * ((e + 112) << 23 | m) |
      ((e == 0) & (m != 0)) *
          ((v - 37) << 23 | ((m << (150 - v)) &
                             0x007FE000))); // sign : normalized : denormalized
}

static inline uint16_t float_to_float16(
    const float x) { // IEEE-754 16-bit floating-point format (without
                     // infinity): 1-5-10, exp-15, +-131008.0, +-6.1035156E-5,
                     // +-5.9604645E-8, 3.311 digits
  const uint32_t b = as_uint(x) + 0x00001000; // round-to-nearest-even: add last
                                              // bit after truncated mantissa
  const uint32_t e = (b & 0x7F800000) >> 23;  // exponent
  const uint32_t m =
      b &
      0x007FFFFF; // mantissa; in line below: 0x007FF000 = 0x00800000-0x00001000
                  // = decimal indicator flag - initial rounding
  return (b & 0x80000000) >> 16 |
         (e > 112) * ((((e - 112) << 10) & 0x7C00) | m >> 13) |
         ((e < 113) & (e > 101)) *
             ((((0x007FF000 + m) >> (125 - e)) + 1) >> 1) |
         (e > 143) * 0x7FFF; // sign : normalized : denormalized : saturate
}

#ifdef __GNUC__
#include <cpuid.h>
#else
#include <intrin.h>
#endif

static __inline void cpuidex_wrapper(int __cpuid_info[4], int __leaf,
                                     int __subleaf) {
#ifdef __GNUC__
  __cpuid_count(__leaf, __subleaf, __cpuid_info[0], __cpuid_info[1],
                __cpuid_info[2], __cpuid_info[3]);
#else
  __cpuidex(__cpuid_info, __leaf, __subleaf);
#endif
}

static bool check_avx512_and_bf16_support() {

  int cpuid[4];

  cpuidex_wrapper(cpuid, 0, 0);
  if (cpuid[0] >= 7) {
    cpuidex_wrapper(cpuid, 7, 0);
    bool avx512vl_supported =
        (cpuid[1] & ((int)1 << 31)) != 0; // AVX-512-VL: EBX[31]
    cpuidex_wrapper(cpuid, 7, 1);
    bool avx512bf16_supported =
        (cpuid[0] & (1 << 5)) != 0; // AVX-512 BF16: EAX[5]
    return avx512vl_supported && avx512bf16_supported;
  }

  return false;
}

#include <immintrin.h>

static inline void float_buffer_to_bfloat16(const float *in,
                                            std::size_t num_elements,
                                            std::uint16_t *out,
                                            const bool use_avx) {
  if (!use_avx) {
    std::transform(in, in + num_elements, out, float_to_bfloat16);
  } else {
// This code is not compiling on the linux build machine in DD
#ifdef _WIN32
    static_assert(sizeof(float) == 4);

    constexpr std::size_t VECTOR_SIZE_BYTES = (512 / 8);
    constexpr std::size_t FLOAT_SIZE_BYTES = sizeof(float);

    static_assert(VECTOR_SIZE_BYTES % FLOAT_SIZE_BYTES == 0);
    constexpr std::size_t FLOATS_PER_VECTOR =
        VECTOR_SIZE_BYTES / FLOAT_SIZE_BYTES;

    const std::size_t num_iter = num_elements / FLOATS_PER_VECTOR;
    const std::size_t remainder = num_elements - num_iter * FLOATS_PER_VECTOR;

    for (std::size_t i = 0; i < num_iter; ++i) {
      __m512 float_vec = _mm512_loadu_ps(in);
      __m256bh bf16_vec = _mm512_cvtneps2bf16(float_vec);
      _mm256_storeu_epi16(out, bf16_vec);

      in += FLOATS_PER_VECTOR;
      out += FLOATS_PER_VECTOR;
    }

    for (std::size_t i = 0; i < remainder; ++i) {
      *out++ = float_to_bfloat16(*in++);
    }
#else
    throw;
#endif
  }
}

/*
 * converts bfloat16 value to float value by zeropadding the last 16 bits
 * @param x is a bfloat16 value in uint16_t var
 * @return float output
 */
static float bfloat16_to_float(uint16_t x) {
  float y = 0.0;
  uint8_t *src = (uint8_t *)&x;
  uint8_t *dst = (uint8_t *)&y;
  std::memcpy(&dst[2], src, sizeof(uint16_t));
  return y;
}

/*
 * Simulate rounding bfloat16 to nearest even
 * @param x is a bfloat16 input in float variable
 * @return bfloat16 value with nearest even rounding in float type
 */
static float bfloat16_rnd_even(float x) {
  return bfloat16_to_float(float_to_bfloat16(x));
}

/*
 * generate a random number of bfloat16 dtype
 * @param non
 * @return bfloat16 data in uint16_t dtype
 */
static uint16_t rand_bfloat16(float range = 1.0f) {
  float x = range * (2.0f * (rand() / (float)RAND_MAX) - 1.0f);
  return float_to_bfloat16(x);
}

/*
* converts float16 to bfloat16 by rounding the LSB to nearest even
@param x is the float16 value
@return bfloat16 value in uint16_t variable
*/
static inline uint16_t float16_to_bfloat16(uint16_t float16) {
  return float_to_bfloat16(float16_to_float(float16));
}

static inline void bfloat16_buffer_to_float(const uint16_t *in,
                                            std::size_t num_elements,
                                            float *out, const bool use_avx) {
  if (!use_avx) {
    std::transform(in, in + num_elements, out, bfloat16_to_float);
  } else {
#ifdef _WIN32
    static_assert(sizeof(uint16_t) == 2);
    constexpr std::size_t VECTOR_SIZE_BYTES = (256 / 8);
    constexpr std::size_t BFLOAT_16SIZE_BYTES = sizeof(uint16_t);

    static_assert(VECTOR_SIZE_BYTES % BFLOAT_16SIZE_BYTES == 0);
    constexpr std::size_t FLOAT16S_PER_VECTOR =
        VECTOR_SIZE_BYTES / BFLOAT_16SIZE_BYTES;

    const std::size_t num_iter = num_elements / FLOAT16S_PER_VECTOR;
    const std::size_t remainder = num_elements % FLOAT16S_PER_VECTOR;

    for (std::size_t i = 0; i < num_iter; ++i) {
      __m256bh bfloat16_vec =
          _mm256_loadu_si256(reinterpret_cast<const __m256bh *>(in));
      __m512 float32_vec = _mm512_castsi512_ps(
          _mm512_slli_epi32(_mm512_cvtepu16_epi32(bfloat16_vec), 16));
      _mm512_store_ps(out, float32_vec);
      in += FLOAT16S_PER_VECTOR;
      out += FLOAT16S_PER_VECTOR;
    }

    if (remainder > 0) {
      __m256bh bfloat16_vec{};
      memcpy(&bfloat16_vec, in, remainder * sizeof(uint16_t));
      __m512 float32_vec = _mm512_castsi512_ps(
          _mm512_slli_epi32(_mm512_cvtepu16_epi32(bfloat16_vec), 16));
      memcpy(out, &float32_vec, remainder * sizeof(uint32_t));
    }
#else
    throw;
#endif
  }
}

/*
* converts float16 to bfloat16 by rounding the LSB to nearest even
@param x is the float16 value
@return bfloat16 value in uint16_t variable
*/
static inline uint16_t bfloat16_to_float16(uint16_t bfloat16) {
  return float_to_float16(bfloat16_to_float(bfloat16));
}

static inline void float16_buffer_to_bfloat16(const uint16_t *in,
                                              std::size_t num_elements,
                                              std::uint16_t *out,
                                              const bool use_avx) {
  if (!use_avx) {
    std::transform(in, in + num_elements, out, float16_to_bfloat16);
  } else {
#ifdef _WIN32
    static_assert(sizeof(uint16_t) == 2);
    constexpr std::size_t VECTOR_SIZE_BYTES = (256 / 8);
    constexpr std::size_t BFLOAT_16SIZE_BYTES = sizeof(uint16_t);

    static_assert(VECTOR_SIZE_BYTES % BFLOAT_16SIZE_BYTES == 0);
    constexpr std::size_t FLOAT16S_PER_VECTOR =
        VECTOR_SIZE_BYTES / BFLOAT_16SIZE_BYTES;

    const std::size_t num_iter = num_elements / FLOAT16S_PER_VECTOR;
    const std::size_t remainder = num_elements % FLOAT16S_PER_VECTOR;

    for (std::size_t i = 0; i < num_iter; ++i) {
      __m256h float16_vec =
          _mm256_loadu_si256(reinterpret_cast<const __m256h *>(in));
      __m512 float32_vec = _mm512_cvtph_ps(float16_vec);
      __m256bh bf16_vec = _mm512_cvtneps2bf16(float32_vec);
      _mm256_storeu_epi16(out, bf16_vec);
      in += FLOAT16S_PER_VECTOR;
      out += FLOAT16S_PER_VECTOR;
    }

    if (remainder > 0) {
      __m256h float16_vec;
      memcpy(&float16_vec, in, remainder * sizeof(uint16_t));
      __m512 float32_vec = _mm512_cvtph_ps(float16_vec);
      __m256bh bf16_vec = _mm512_cvtneps2bf16(float32_vec);
      memcpy(out, &bf16_vec, remainder * sizeof(uint16_t));
    }
#else
    throw;
#endif
  }
}

static inline void bfloat16_buffer_to_float16(const uint16_t *in,
                                              std::size_t num_elements,
                                              std::uint16_t *out,
                                              const bool use_avx) {
  if (!use_avx) {
    std::transform(in, in + num_elements, out, bfloat16_to_float16);
  } else {
#ifdef _WIN32
    static_assert(sizeof(uint16_t) == 2);
    constexpr std::size_t VECTOR_SIZE_BYTES = (256 / 8);
    constexpr std::size_t BFLOAT_16SIZE_BYTES = sizeof(uint16_t);

    static_assert(VECTOR_SIZE_BYTES % BFLOAT_16SIZE_BYTES == 0);
    constexpr std::size_t FLOAT16S_PER_VECTOR =
        VECTOR_SIZE_BYTES / BFLOAT_16SIZE_BYTES;

    const std::size_t num_iter = num_elements / FLOAT16S_PER_VECTOR;
    const std::size_t remainder = num_elements % FLOAT16S_PER_VECTOR;

    for (std::size_t i = 0; i < num_iter; ++i) {
      __m256bh bfloat16_vec =
          _mm256_loadu_si256(reinterpret_cast<const __m256bh *>(in));
      __m512 float32_vec = _mm512_castsi512_ps(
          _mm512_slli_epi32(_mm512_cvtepu16_epi32(bfloat16_vec), 16));
      __m256h float16_vec =
          _mm512_cvtps_ph(float32_vec, _MM_FROUND_TO_NEAREST_INT);
      _mm256_storeu_epi16(out, float16_vec);
      in += FLOAT16S_PER_VECTOR;
      out += FLOAT16S_PER_VECTOR;
    }

    if (remainder > 0) {
      __m256bh bfloat16_vec{};
      memcpy(&bfloat16_vec, in, remainder * sizeof(uint16_t));
      __m512 float32_vec = _mm512_castsi512_ps(
          _mm512_slli_epi32(_mm512_cvtepu16_epi32(bfloat16_vec), 16));
      __m256h float16_vec =
          _mm512_cvtps_ph(float32_vec, _MM_FROUND_TO_NEAREST_INT);
      memcpy(out, &float16_vec, remainder * sizeof(uint16_t));
    }
#else
    throw;
#endif
  }
}

/*
 * pack two int4 values (with 0s in MSB) into an int8 variable
 * @param x is the first int4 value in int8 dtype
 * @param y is the second int4 value in int8 dtype
 * @return packed int8 value
 */
static uint8_t pack_v2int4(int x, int y) {
  assert(-8 <= x && x <= 7);
  assert(-8 <= y && y <= 7);
  return (x & 0xF) | ((y & 0xF) << 4);
}

/*
 * pack two uint4 values (with 0s in MSB) into an uint8 variable
 * @param x is the first uint4 value in int8 dtype
 * @param y is the second uint4 value in int8 dtype
 * @return packed uint8 value
 */
static uint8_t pack_v2uint4(int x, int y) {
  assert(0 <= x && x <= 15);
  assert(0 <= y && y <= 15);
  return (x & 0xF) | ((y & 0xF) << 4);
}

struct v2int {
  int x;
  int y;
};

/*
 * unpack an int8 variable into 2 int4 variables
 * @param a is uint8_t variable
 * @return v2int object with 2 int4 elements
 */
static v2int unpack_v2int4(uint8_t a) {
  v2int v;
  // Extract nibbles
  v.x = (a & 0x0F);
  v.y = (a & 0xF0) >> 4;
  // Convert to signed two's complement
  v.x = (v.x % 8) - ((v.x / 8) * 8);
  v.y = (v.y % 8) - ((v.y / 8) * 8);
  return v;
}

/*
 * unpack an int8 variable into 2 uint4 variables
 * @param a is uint8_t variable
 * @return v2int object with 2 uint4 elements
 */
static v2int unpack_v2uint4(uint8_t a) {
  v2int v;
  // Extract nibbles
  v.x = (a & 0x0F);
  v.y = (a & 0xF0) >> 4;
  return v;
}

/*
 * random number generator for int4 dtype
 */
static int rand_int4(int data_range = 8) {
  return (rand() % (2 * data_range)) - data_range;
}

/*
 * random number generator for uint4 dtype
 */
static int rand_uint4(int data_range = 16) { return (rand() % data_range); }

} // namespace ryzenai
#endif // DTYPE_UTILS
