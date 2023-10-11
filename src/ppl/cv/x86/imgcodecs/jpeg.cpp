// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "jpeg.h"
#include "codecs.h"

#include <limits.h>
#include <assert.h>
#include <memory.h>
#include <thread>
#include <vector>

#include "ppl/cv/x86/intrinutils.hpp"
#include <immintrin.h>

#include "ppl/cv/types.h"
#include "ppl/common/log.h"

#include <time.h>  // debug
#include <sys/time.h>  // debug
#include <iostream>  // debug
#include <gperftools/profiler.h>  // profiling

using namespace ppl::common;

namespace ppl {
namespace cv {
namespace x86 {

#define STBI_MAX_DIMENSIONS (1 << 24)
#define NULL_MARKER 0xff
#define DIVIDE4(x) ((uint8_t) ((x) >> 2))
#define DIVIDE16(x) ((uint8_t) ((x) >> 4))
#define SIMD_ALIGN16(type, name) type name __attribute__((aligned(16)))

// in each scan, we'll have scan_n components, and the order
// of the components is specified by order[], rstn marker
#define DRI_RESTART(x) ((x) >= 0xd0 && (x) <= 0xd7)
// #define ROTATE_BITS(x, y) (((x) << (y)) | ((x) >> (-(y) & 31)))
// #define ROTATE_BITS(x, y) (((x) << (y)) | ((x) >> (-(y) & (BUFFER_BITS - 1))))
#define ROTATE_BITS(x, y) (((x) << (y)) | ((x) >> ((BUFFER_BITS - (y)))))

#define FLOAT2FLOAT(x) ((int32_t) (((x) * 4096 + 0.5)))
#define FSH(x) ((x) * 4096)

// derived from jidctint -- DCT_ISLOW
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                                \
   int32_t t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;                 \
   p2 = s2;                                                                    \
   p3 = s6;                                                                    \
   p1 = (p2 + p3) * FLOAT2FLOAT(0.5411961f);                                   \
   t2 = p1 + p3*FLOAT2FLOAT(-1.847759065f);                                    \
   t3 = p1 + p2*FLOAT2FLOAT( 0.765366865f);                                    \
   p2 = s0;                                                                    \
   p3 = s4;                                                                    \
   t0 = FSH(p2 + p3);                                                          \
   t1 = FSH(p2 - p3);                                                          \
   x0 = t0 + t3;                                                               \
   x3 = t0 - t3;                                                               \
   x1 = t1 + t2;                                                               \
   x2 = t1 - t2;                                                               \
   t0 = s7;                                                                    \
   t1 = s5;                                                                    \
   t2 = s3;                                                                    \
   t3 = s1;                                                                    \
   p3 = t0 + t2;                                                               \
   p4 = t1 + t3;                                                               \
   p1 = t0 + t3;                                                               \
   p2 = t1 + t2;                                                               \
   p5 = (p3 + p4) * FLOAT2FLOAT( 1.175875602f);                                \
   t0 = t0 * FLOAT2FLOAT( 0.298631336f);                                       \
   t1 = t1 * FLOAT2FLOAT( 2.053119869f);                                       \
   t2 = t2 * FLOAT2FLOAT( 3.072711026f);                                       \
   t3 = t3 * FLOAT2FLOAT( 1.501321110f);                                       \
   p1 = p5 + p1*FLOAT2FLOAT(-0.899976223f);                                    \
   p2 = p5 + p2*FLOAT2FLOAT(-2.562915447f);                                    \
   p3 = p3 * FLOAT2FLOAT(-1.961570560f);                                       \
   p4 = p4 * FLOAT2FLOAT(-0.390180644f);                                       \
   t3 += p1 + p4;                                                              \
   t2 += p2 + p3;                                                              \
   t1 += p2 + p4;                                                              \
   t0 += p1 + p3;

// given a value that's at position X in the zigzag stream,
// where does it appear in the 8x8 matrix coded as row-major?
static const uint8_t dezigzag_indices[64 + 15] = {
    0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
    // let corrupt input sample past end
    63, 63, 63, 63, 63, 63, 63, 63,
    63, 63, 63, 63, 63, 63, 63
};

// (1 << n) - 1
static const uint32_t bit_mask[17] = {0, 1, 3, 7, 15, 31, 63, 127, 255, 511,
    1023, 2047, 4095, 8191, 16383, 32767, 65535};
// bias[n] = (-1<<n) + 1
static const int32_t jbias[16] = {0, -1, -3, -7, -15, -31, -63, -127, -255,
    -511, -1023, -2047, -4095, -8191, -16383, -32767};

// take a -128..127 value and stbi__clamp it and convert to 0..255
inline static uint8_t clampInt8(int32_t value) {
   value = value > 255 ? 255 : (value < 0 ? 0 : value);

   return value;
}

// returns 1 if the product is valid, 0 on overflow.
// negative factors are considered invalid.
static int32_t validMul2Sizes(int32_t a, int32_t b) {
   if (a < 0 || b < 0) return 0;
   if (b == 0) return 1; // mul-by-0 is always safe
   // portable way to check for no overflows in a*b
   return a <= INT_MAX / b;
}

// return 1 if the sum is valid, 0 on overflow.
// negative terms are considered invalid.
static int32_t validAddSizes(int32_t a, int32_t b) {
   if (b < 0) return 0;
   // now 0 <= b <= INT_MAX, hence also
   // 0 <= INT_MAX - b <= INTMAX.
   // And "a + b <= INT_MAX" (which might overflow) is the
   // same as a <= INT_MAX - b (no overflow)
   return a <= INT_MAX - b;
}

// returns 1 if "a*b + add" has no negative terms/factors and doesn't overflow
static int32_t validMad2Sizes(int32_t a, int32_t b, int32_t add) {
   return validMul2Sizes(a, b) && validAddSizes(a * b, add);
}

// returns 1 if "a*b*c + add" has no negative terms/factors and doesn't overflow
static int32_t validMad3Sizes(int32_t a, int32_t b, int32_t c, int32_t add) {
    return validMul2Sizes(a, b) && validMul2Sizes(a * b, c) &&
           validAddSizes(a * b * c, add);
}

// mallocs with size overflow checking
static void* mallocMad2(int32_t a, int32_t b, int32_t add) {
   if (!validMad2Sizes(a, b, add)) return NULL;
   return malloc(a * b + add);
}

static void* mallocMad3(int32_t a, int32_t b, int32_t c, int32_t add) {
   if (!validMad3Sizes(a, b, c, add)) return NULL;
   return malloc(a * b * c + add);
}

void idctDecodeBlock(uint8_t *output, int32_t out_stride, int16_t data[64]) {
    int32_t i, val[64], *v = val;
    uint8_t *o;
    int16_t *d = data;
    int32_t scaled = 65536 + (128 << 17);

    // columns
    for (i = 0; i < 8; ++i, ++d, ++v) {
        // if all zeroes, shortcut -- this avoids dequantizing 0s and IDCTing
        if (d[8] == 0 && d[16] == 0 && d[24] == 0 && d[32] == 0 &&
            d[40] == 0 && d[48] == 0 && d[56] == 0) {
            //    no shortcut                 0     seconds
            //    (1|2|3|4|5|6|7)==0          0     seconds
            //    all separate               -0.047 seconds
            //    1 && 2|3 && 4|5 && 6|7:    -0.047 seconds
            int32_t dcterm = d[0] * 4;
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] =
                   dcterm;
        } else {
            IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
            // constants scaled things up by 1<<12; let's bring them back
            // down, but keep 2 extra bits of precision
            x0 += 512; x1 += 512; x2 += 512; x3 += 512;
            v[ 0] = (x0 + t3) >> 10;
            v[56] = (x0 - t3) >> 10;
            v[ 8] = (x1 + t2) >> 10;
            v[48] = (x1 - t2) >> 10;
            v[16] = (x2 + t1) >> 10;
            v[40] = (x2 - t1) >> 10;
            v[24] = (x3 + t0) >> 10;
            v[32] = (x3 - t0) >> 10;
        }
    }

    for (i = 0, v = val, o = output; i < 8; ++i, v += 8, o += out_stride) {
        // no fast case since the first 1D IDCT spread components out
        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        // constants scaled things up by 1<<12, plus we had 1<<2 from first
        // loop, plus horizontal and vertical each scale by sqrt(8) so together
        // we've got an extra 1<<3, so 1<<17 total we need to remove.
        // so we want to round that, which means adding 0.5 * 1<<17,
        // aka 65536. Also, we'll end up with -128 to 127 that we want
        // to encode as 0..255 by adding 128, so we'll add that before the shift
        x0 += scaled;
        x1 += scaled;
        x2 += scaled;
        x3 += scaled;
        // tried computing the shifts into temps, or'ing the temps to see
        // if any were out of range, but that was slower
        o[0] = clampInt8((x0 + t3) >> 17);
        o[7] = clampInt8((x0 - t3) >> 17);
        o[1] = clampInt8((x1 + t2) >> 17);
        o[6] = clampInt8((x1 - t2) >> 17);
        o[2] = clampInt8((x2 + t1) >> 17);
        o[5] = clampInt8((x2 - t1) >> 17);
        o[3] = clampInt8((x3 + t0) >> 17);
        o[4] = clampInt8((x3 - t0) >> 17);
    }
}

// this is a reduced-precision calculation of YCbCr-to-BGR introduced
// to make sure the code produces the same results in both SIMD and scalar
#define FLOAT2FIXED(x) (((int32_t) ((x) * 4096.0f + 0.5f)) << 8)
static void YCbCr2BGRRow(uint8_t *out, const uint8_t *y, const uint8_t *pcb,
                         const uint8_t *pcr, int32_t width, int32_t channels) {
    for (int32_t i = 0; i < width; ++i) {
        int32_t y_fixed = (y[i] << 20) + (1 << 19); // rounding
        int32_t b, g, r;
        int32_t cb = pcb[i] - 128;
        int32_t cr = pcr[i] - 128;
        b = y_fixed + cb * FLOAT2FIXED(1.77200f);
        g = y_fixed + (cr * -FLOAT2FIXED(0.71414f)) +
            ((cb * -FLOAT2FIXED(0.34414f)) & 0xffff0000);
        r = y_fixed + cr * FLOAT2FIXED(1.40200f);
        b >>= 20;
        g >>= 20;
        r >>= 20;
        b = clampInt8(b);
        g = clampInt8(g);
        r = clampInt8(r);
        out[0] = (uint8_t)b;
        out[1] = (uint8_t)g;
        out[2] = (uint8_t)r;
        out += channels;
    }
}
/* int32_t stbi__cpuid3(void)
{
   int32_t res;
   __asm {
      mov  eax,1
      cpuid
      mov  res,edx
   }
   return res;
}

int32_t stbi__sse2_available(void)
{
   int32_t info3 = stbi__cpuid3();
   return ((info3 >> 26) & 1) != 0;
} */

YCrCb2BGR_i::YCrCb2BGR_i(int32_t _width, int32_t _channels) : width(_width),
                         channels(_channels) {
    signflip  = _mm_set1_epi8(-0x80);
    cr_const0 = _mm_set1_epi16(   (int16_t)(1.40200f * 4096.0f + 0.5f));
    cr_const1 = _mm_set1_epi16( - (int16_t)(0.71414f * 4096.0f + 0.5f));
    cb_const0 = _mm_set1_epi16( - (int16_t)(0.34414f * 4096.0f + 0.5f));
    cb_const1 = _mm_set1_epi16(   (int16_t)(1.77200f * 4096.0f + 0.5f));
    y_bias = _mm_set1_epi8((char) (uint8_t) 128);
}

YCrCb2BGR_i::~YCrCb2BGR_i() {
}

void YCrCb2BGR_i::process8Elements(uint8_t const *y, uint8_t const *pcb,
                                   uint8_t const *pcr, uint32_t index,
                                   __m128i &b16s, __m128i &g16s,
                                   __m128i &r16s) const {
    // load
    __m128i y_bytes = _mm_loadl_epi64((__m128i *)(y + index));
    __m128i cb_bytes = _mm_loadl_epi64((__m128i *)(pcb + index));
    __m128i cr_bytes = _mm_loadl_epi64((__m128i *)(pcr + index));
    __m128i cb_biased = _mm_xor_si128(cb_bytes, signflip); // -128
    __m128i cr_biased = _mm_xor_si128(cr_bytes, signflip); // -128

    // unpack to int16_t (and left-shift cr, cb by 8)
    __m128i yw  = _mm_unpacklo_epi8(y_bias, y_bytes);
    __m128i cbw = _mm_unpacklo_epi8(_mm_setzero_si128(), cb_biased);
    __m128i crw = _mm_unpacklo_epi8(_mm_setzero_si128(), cr_biased);

    // color transform
    __m128i yws = _mm_srli_epi16(yw, 4);
    __m128i cb0 = _mm_mulhi_epi16(cb_const0, cbw);
    __m128i cr0 = _mm_mulhi_epi16(cr_const0, crw);
    __m128i cb1 = _mm_mulhi_epi16(cbw, cb_const1);
    __m128i cr1 = _mm_mulhi_epi16(crw, cr_const1);
    __m128i bws = _mm_add_epi16(yws, cb1);
    __m128i gwt = _mm_add_epi16(cb0, yws);
    __m128i rws = _mm_add_epi16(cr0, yws);
    __m128i gws = _mm_add_epi16(gwt, cr1);

    // descale
    b16s = _mm_srai_epi16(bws, 4);
    g16s = _mm_srai_epi16(gws, 4);
    r16s = _mm_srai_epi16(rws, 4);
}

// void YCrCb2BGR_i::convertBGR(uint8_t *dst, uint8_t const *y, uint8_t const *pcb,
//                     uint8_t const *pcr, int32_t count, int32_t step) {
void YCrCb2BGR_i::convertBGR(uint8_t const *y, uint8_t const *pcb,
                             uint8_t const *pcr, uint8_t *dst) {
    int32_t i = 0;
    for (; i <= width - 32; i += 32, dst += channels * 32) {
        process8Elements(y, pcb, pcr, i, b16s0, g16s0, r16s0);
        process8Elements(y, pcb, pcr, i + 8, b16s1, g16s1, r16s1);
        __m128i b8s0 = _mm_packus_epi16(b16s0, b16s1);
        __m128i g8s0 = _mm_packus_epi16(g16s0, g16s1);
        __m128i r8s0 = _mm_packus_epi16(r16s0, r16s1);

        process8Elements(y, pcb, pcr, i + 16, b16s0, g16s0, r16s0);
        process8Elements(y, pcb, pcr, i + 24, b16s1, g16s1, r16s1);
        __m128i b8s1 = _mm_packus_epi16(b16s0, b16s1);
        __m128i g8s1 = _mm_packus_epi16(g16s0, g16s1);
        __m128i r8s1 = _mm_packus_epi16(r16s0, r16s1);

        _mm_interleave_epi8(b8s0, b8s1, g8s0, g8s1, r8s0, r8s1);
        _mm_storeu_si128((__m128i *)(dst), b8s0);
        _mm_storeu_si128((__m128i *)(dst + 16), b8s1);
        _mm_storeu_si128((__m128i *)(dst + 32), g8s0);
        _mm_storeu_si128((__m128i *)(dst + 48), g8s1);
        _mm_storeu_si128((__m128i *)(dst + 64), r8s0);
        _mm_storeu_si128((__m128i *)(dst + 80), r8s1);
    }

    for (; i < width; ++i) {
        int32_t y_fixed = (y[i] << 20) + (1 << 19); // rounding
        int32_t b, g, r;
        int32_t cb = pcb[i] - 128;
        int32_t cr = pcr[i] - 128;
        b = y_fixed + cb * FLOAT2FIXED(1.77200f);
        g = y_fixed + (cr * -FLOAT2FIXED(0.71414f)) +
            ((cb * -FLOAT2FIXED(0.34414f)) & 0xffff0000);
        r = y_fixed + cr * FLOAT2FIXED(1.40200f);
        b >>= 20;
        g >>= 20;
        r >>= 20;
        b = clampInt8(b);
        g = clampInt8(g);
        r = clampInt8(r);
        dst[0] = (uint8_t)b;
        dst[1] = (uint8_t)g;
        dst[2] = (uint8_t)r;
        dst += channels;
    }

}

static void YCbCr2BGRSse(uint8_t *out, uint8_t const *y, uint8_t const *pcb,
                         uint8_t const *pcr, int32_t width, int32_t channels) {
    int32_t i = 0;

    // channels == 3 is pretty ugly on the final interleave, and i'm not convinced
    // it's useful in practice (you wouldn't use it for textures, for example).
    // so just accelerate channels == 4 case.
    // this is a fairly straightforward implementation and not super-optimized.
    __m128i signflip  = _mm_set1_epi8(-0x80);
    __m128i cr_const0 = _mm_set1_epi16(   (int16_t) ( 1.40200f*4096.0f+0.5f));
    __m128i cr_const1 = _mm_set1_epi16( - (int16_t) ( 0.71414f*4096.0f+0.5f));
    __m128i cb_const0 = _mm_set1_epi16( - (int16_t) ( 0.34414f*4096.0f+0.5f));
    __m128i cb_const1 = _mm_set1_epi16(   (int16_t) ( 1.77200f*4096.0f+0.5f));
    __m128i y_bias = _mm_set1_epi8((char) (uint8_t) 128);
    __m128i xw = _mm_set1_epi16(255); // alpha channel

    for (; i + 7 < width; i += 8) {
        // load
        __m128i y_bytes = _mm_loadl_epi64((__m128i *) (y+i));
        __m128i cb_bytes = _mm_loadl_epi64((__m128i *) (pcb+i));
        __m128i cr_bytes = _mm_loadl_epi64((__m128i *) (pcr+i));
        __m128i cb_biased = _mm_xor_si128(cb_bytes, signflip); // -128
        __m128i cr_biased = _mm_xor_si128(cr_bytes, signflip); // -128

        // unpack to int16_t (and left-shift cr, cb by 8)
        __m128i yw  = _mm_unpacklo_epi8(y_bias, y_bytes);
        __m128i cbw = _mm_unpacklo_epi8(_mm_setzero_si128(), cb_biased);
        __m128i crw = _mm_unpacklo_epi8(_mm_setzero_si128(), cr_biased);

        // color transform
        __m128i yws = _mm_srli_epi16(yw, 4);
        __m128i cb0 = _mm_mulhi_epi16(cb_const0, cbw);
        __m128i cr0 = _mm_mulhi_epi16(cr_const0, crw);
        __m128i cb1 = _mm_mulhi_epi16(cbw, cb_const1);
        __m128i cr1 = _mm_mulhi_epi16(crw, cr_const1);
        __m128i bws = _mm_add_epi16(yws, cb1);
        __m128i gwt = _mm_add_epi16(cb0, yws);
        __m128i rws = _mm_add_epi16(cr0, yws);
        __m128i gws = _mm_add_epi16(gwt, cr1);

        // descale
        __m128i bw = _mm_srai_epi16(bws, 4);
        __m128i gw = _mm_srai_epi16(gws, 4);
        __m128i rw = _mm_srai_epi16(rws, 4);

        // back to byte, set up for transpose
        __m128i brb = _mm_packus_epi16(bw, rw);
        __m128i gxb = _mm_packus_epi16(gw, xw);

        uint8_t* bs = (uint8_t*)&brb;
        uint8_t* gs = (uint8_t*)&gxb;
        uint8_t* rs = (uint8_t*)&brb + 8;
        for (int j = 0; j < 8; j++) {
            out[0] = (uint8_t)bs[j];
            out[1] = (uint8_t)gs[j];
            out[2] = (uint8_t)rs[j];
            out += channels;
        }
    }

    for (; i < width; ++i) {
        int32_t y_fixed = (y[i] << 20) + (1 << 19); // rounding
        int32_t b, g, r;
        int32_t cb = pcb[i] - 128;
        int32_t cr = pcr[i] - 128;
        b = y_fixed + cb * FLOAT2FIXED(1.77200f);
        g = y_fixed + (cr * -FLOAT2FIXED(0.71414f)) +
            ((cb * -FLOAT2FIXED(0.34414f)) & 0xffff0000);
        r = y_fixed + cr * FLOAT2FIXED(1.40200f);
        b >>= 20;
        g >>= 20;
        r >>= 20;
        b = clampInt8(b);
        g = clampInt8(g);
        r = clampInt8(r);
        out[0] = (uint8_t)b;
        out[1] = (uint8_t)g;
        out[2] = (uint8_t)r;
        out += channels;
    }
}

static uint8_t *resampleRowHV2(uint8_t *out, uint8_t *in_near, uint8_t *in_far,
                               int32_t width, int32_t hs) {
    // need to generate 2x2 samples for every one in input
    int32_t i, t0, t1;
    if (width == 1) {
        out[0] = out[1] = DIVIDE4(3 * in_near[0] + in_far[0] + 2);
        return out;
    }

    t1 = 3 * in_near[0] + in_far[0];
    out[0] = DIVIDE4(t1 + 2);
    for (i = 1; i < width; ++i) {
        t0 = t1;
        t1 = 3 * in_near[i] + in_far[i];
        out[i * 2 - 1] = DIVIDE16(3 * t0 + t1 + 8);
        out[i * 2]     = DIVIDE16(3 * t1 + t0 + 8);
    }
    out[width * 2 - 1] = DIVIDE4(t1 + 2);

    return out;
}

static uint8_t *resampleRow1(uint8_t *out, uint8_t *in_near, uint8_t *in_far,
                             int32_t width, int32_t hs) {
   return in_near;
}

static uint8_t* resampleRowV2(uint8_t *out, uint8_t *in_near, uint8_t *in_far,
                              int32_t width, int32_t hs) {
    // need to generate two samples vertically for every one in input
    for (int32_t i = 0; i < width; ++i) {
        out[i] = DIVIDE4(3 * in_near[i] + in_far[i] + 2);
    }

    return out;
}

static uint8_t* resampleRowH2(uint8_t *out, uint8_t *in_near, uint8_t *in_far,
                              int32_t width, int32_t hs) {
   // need to generate two samples horizontally for every one in input
    int32_t i;
    uint8_t *input = in_near;

    if (width == 1) {
        // if only one sample, can't do any interpolation
        out[0] = out[1] = input[0];
        return out;
    }

    out[0] = input[0];
    out[1] = DIVIDE4(input[0] * 3 + input[1] + 2);
    for (i = 1; i < width - 1; ++i) {
        int32_t n = 3 * input[i] + 2;
        out[i * 2 + 0] = DIVIDE4(n + input[i - 1]);
        out[i * 2 + 1] = DIVIDE4(n + input[i + 1]);
    }
    out[i * 2 + 0] = DIVIDE4(input[width - 2] * 3 + input[width - 1] + 2);
    out[i * 2 + 1] = input[width - 1];

    return out;
}

static uint8_t *resampleRowGeneric(uint8_t *out, uint8_t *in_near,
                                   uint8_t *in_far, int32_t width, int32_t hs) {
    // resample with nearest-neighbor
    for (int32_t i = 0; i < width; ++i) {
        for (int32_t j = 0; j < hs; ++j) {
            out[i * hs + j] = in_near[i];
        }
    }

    return out;
}

// fast 0..255 * 0..255 => 0..255 rounded multiplication
static uint8_t blinn8x8(uint8_t x, uint8_t y) {
   uint32_t t = x * y + 128;
   return (uint8_t) ((t + (t >> 8)) >> 8);
}

static uint8_t computeY(int32_t r, int32_t g, int32_t b) {
    return (uint8_t)((r * 77 + g * 150 + 29 * b) >> 8);
}

JpegDecoder::JpegDecoder(BytesReader& file_data) {
    file_data_ = &file_data;

    jpeg_ = (JpegDecodeData*) malloc(sizeof(JpegDecodeData));
    if (jpeg_ == nullptr) {
       LOG(ERROR) << "No enough memory to initialize JpegDecoder.";
    }
}

JpegDecoder::~JpegDecoder() {
    // for (uint32_t i = 0; i < 4; i++) {
    //     if (jpeg_->dequant[i] != nullptr) {
    //         delete [] jpeg_->dequant[i];
    //     }
    // }

    // delete [] jpeg_->huff_dc;
    // delete [] jpeg_->huff_ac;

   free(jpeg_);
}

/* bool JpegDecoder::buildHuffmanTable(HuffmanLookupTable *huffman_table,
                                    int32_t *counts) {
    int32_t bit_number, i, j, index = 0;
    uint32_t code;
    // build code length list for each symbol (from JPEG spec)
    for (bit_number = 1; bit_number <= 16; ++bit_number) {
        for (j = 0; j < counts[bit_number - 1]; ++j) {
            huffman_table->bit_lengths[index++] = (uint8_t)bit_number;
        }
    }
    huffman_table->bit_lengths[index] = 0;

    // compute actual binary codes (from jpeg spec)
    code = 0;
    index = 0;
    for (j = 1; j <= 16; ++j) {
        // compute delta to add to code to compute symbol id
        huffman_table->delta[j] = index - code;
        if (huffman_table->bit_lengths[index] == j) {
            while (huffman_table->bit_lengths[index] == j) {
                huffman_table->codes[index++] = (uint16_t)(code++);
            }
            if (code - 1 >= (1u << j)) {
                LOG(ERROR) << "bad code lengths, corrupt JPEG";
                return false;
            }
        }
        // compute largest code + 1 for this size, preshifted as needed later
        huffman_table->max_codes[j] = code << (16 - j);
        code <<= 1;
    }
    huffman_table->max_codes[j] = 0xffffffff;

    // build non-spec acceleration table; 255 is flag for not-accelerated
    // store indices of tuples of symbol--code--bit length
    memset(huffman_table->fast_indices, 255, 1 << FAST_BITS);
    for (i = 0; i < index; ++i) {
        int32_t bit_length = huffman_table->bit_lengths[i];
        if (bit_length <= FAST_BITS) {
            code = huffman_table->codes[i] << (FAST_BITS - bit_length);
            int32_t count = 1 << (FAST_BITS - bit_length);
            for (j = 0; j < count; ++j) {
                huffman_table->fast_indices[code + j] = (uint8_t) i;
            }
            // memset(huffman_table->fast_indices + code, i, count);
        }
    }

    return true;
} */

bool JpegDecoder::buildHuffmanTable(HuffmanLookupTable *huffman_table,
                                    int32_t *symbol_counts) {
    uint8_t bit_lengths[257];
    uint16_t codes[256];
    int32_t bit_number, i, index = 0;
    // build code length list for each symbol (from JPEG spec)
    for (bit_number = 1; bit_number <= MAX_BITS; ++bit_number) {
        for (i = 0; i < symbol_counts[bit_number - 1]; ++i) {
            bit_lengths[index++] = (uint8_t)bit_number;
        }
    }
    bit_lengths[index] = 0;

    // compute actual binary codes (from jpeg spec)
    uint32_t code = 0;
    index = 0;
    for (bit_number = 1; bit_number <= MAX_BITS; ++bit_number) {
        // compute delta to add to code to compute symbol id
        huffman_table->delta[bit_number] = index - code;
        if (bit_lengths[index] == bit_number) {
            while (bit_lengths[index] == bit_number) {
                codes[index++] = (uint16_t)(code++);
            }
            if (code >= (1 << bit_number)) {
                LOG(ERROR) << "Wrong code during huffman tabale generation.";
                return false;
            }
        }
        // compute largest code + 1 for this size, preshifted as needed later
        huffman_table->max_codes[bit_number] = code << (MAX_BITS - bit_number);
        code <<= 1;
    }
    huffman_table->max_codes[bit_number] = 0xFFFF;

    // build non-spec acceleration table; oxFFFF is flag for not-accelerated
    // store indices of tuples of bit length - symbol.
    memset(huffman_table->lookups, 255,
           sizeof(uint16_t) * (1 << LOOKAHEAD_BITS));
    int32_t size = index;
    for (index = 0; index < size; ++index) {
        int32_t bit_length = bit_lengths[index];
        if (bit_length <= LOOKAHEAD_BITS) {
            code = codes[index] << (LOOKAHEAD_BITS - bit_length);
            int32_t count = 1 << (LOOKAHEAD_BITS - bit_length);
            for (i = 0; i < count; ++i) {
                huffman_table->lookups[code++] = (bit_length << 8) |
                                                    huffman_table->symbols[index];
            }
            // memset(huffman_table->fast_indices + code, index, count);
        }
    }

    return true;
}

bool JpegDecoder::parseAPP0(JpegDecodeData *jpeg) {
    int32_t length = file_data_->getWordBigEndian();
    if (length < 16) {
        LOG(ERROR) << "bad JFIF APP0 segment length, corrupt JPEG.";
        return false;
    }

    uint8_t tag[5] = {'J','F','I','F','\0'};
    int32_t i = 0;
    uint8_t value;
    for (; i < 5; ++i) {
        value = file_data_->getByte();
        if (value != tag[i]) break;
    }

    if (i == 5) {
        jpeg->jfif = 1;
    }
    else {
        jpeg->jfif = 0;
    }
    file_data_->skip(length - 7);

    return true;
}

bool JpegDecoder::parseAPP14(JpegDecodeData *jpeg) {
    int32_t length = file_data_->getWordBigEndian();
    if (length < 8) {
        LOG(ERROR) << "bad Adobe APP14 segment length, corrupt JPEG.";
        return false;
    }

    uint8_t tag[6] = {'A','d','o','b','e','\0'};
    int32_t i = 0;
    uint8_t value;
    for (; i < 6; ++i) {
        value = file_data_->getByte();
        if (value != tag[i]) break;
    }

    if (i == 6) {
        file_data_->skip(5);
        jpeg->app14_color_transform = file_data_->getByte();
        length -= 6;
    }
    file_data_->skip(length - 8);

    return true;
}

bool JpegDecoder::parseSOF(JpegDecodeData *jpeg) {
    int32_t length = file_data_->getWordBigEndian();
    if (length < 11) {
        LOG(ERROR) << "Invalid SOF length: " << length
                   << ", correct value: not less than 11.";
        return false;
    }
    int32_t value = file_data_->getByte();  // precision bit
    if (value != 8) {
        LOG(ERROR) << "Invalid pixel component precision: " << value
                   << ", correct value: 8 (bit).";
        return false;
    }

    height_ = file_data_->getWordBigEndian();
    width_  = file_data_->getWordBigEndian();
    if (height_ < 1 || width_ < 1) {
        LOG(ERROR) << "Invalid image height/width: " << height_ << ", "
                   << width_;
        return false;
    }

    jpeg->components = file_data_->getByte();  // Gray(1), YCbCr/YIQ(3), CMYK(4)
    channels_ = jpeg->components >= 3 ? 3 : 1;
    if (jpeg->components != 1 && jpeg->components != 3 &&
        jpeg->components != 4) {
        LOG(ERROR) << "Invalid component count: " << jpeg->components
                   << ", correct value: 0(Gray), 3(YCbCr), 4(CMYK).";
        return false;
    }
    if (height_ * width_ * jpeg->components > MAX_IMAGE_SIZE) {
        LOG(ERROR) << "the JPEG image is too large.";
        return false;
    }
    if (length != 8 + 3 * jpeg->components) {
        LOG(ERROR) << "Invalid SOF length: " << length << ", correct value: "
                   << 8 + 3 * jpeg->components;
        return false;
    }

    int32_t i = 0;
    for (; i < jpeg->components; i++) {
        jpeg->img_comp[i].data = nullptr;
        jpeg->img_comp[i].line_buffer = nullptr;
    }

    // int32_t count = jpeg->components >= 3 ? 2 : 1;
    // for (i = 0; i < count; i++) {
    //     jpeg->huff_dc = new HuffmanLookupTable[count];
    //     jpeg->huff_ac = new HuffmanLookupTable[count];
    // }

    jpeg->rgb = 0;
    int32_t h_max = 1, v_max = 1;
    for (i = 0; i < jpeg->components; ++i) {
        const uint8_t rgb[3] = { 'R', 'G', 'B' };
        // component id: Y(1), Cb(2), Cr(3), I(4), Q(5)
        jpeg->img_comp[i].id = file_data_->getByte();
        if (jpeg->components == 3 && jpeg->img_comp[i].id == rgb[i]) {
            ++jpeg->rgb;
        }
        value = file_data_->getByte();
        jpeg->img_comp[i].hsampling = (value >> 4);  // horizonal sampling rate
        if (!jpeg->img_comp[i].hsampling || jpeg->img_comp[i].hsampling > 4) {
            LOG(ERROR) << "Invalid horizontal sampling rate: " << (value >> 4)
                       << " of component " << jpeg->img_comp[i].id
                       << ", valid value: 1-3.";
            return false;
        }
        jpeg->img_comp[i].vsampling = value & 15;   // vertical sampling rate
        if (!jpeg->img_comp[i].vsampling || jpeg->img_comp[i].vsampling > 4) {
            LOG(ERROR) << "Invalid vertical sampling rate: " << (value & 15)
                       << " of component " << jpeg->img_comp[i].id
                       << ", valid value: 1-3.";
            return false;
        }
        value = file_data_->getByte();
        jpeg->img_comp[i].quant_id = value;        // quantification table ID
        if (jpeg->img_comp[i].quant_id > 3) {
            LOG(ERROR) << "Invalid ID of quantification table: " << value
                       << " of component " << jpeg->img_comp[i].id
                       << ", valid value: 0-3.";
            return false;
        }

        if (jpeg->img_comp[i].hsampling > h_max) {
            h_max = jpeg->img_comp[i].hsampling;
        }
        if (jpeg->img_comp[i].vsampling > v_max) {
            v_max = jpeg->img_comp[i].vsampling;
        }
    }

    for (i = 0; i < jpeg->components; ++i) {
        if (h_max % jpeg->img_comp[i].hsampling != 0) {
            LOG(ERROR) << "Invalid horizontal component samples.";
            return false;
        }
        if (v_max % jpeg->img_comp[i].vsampling != 0) {
            LOG(ERROR) << "Invalid vertical component samples.";
            return false;
        }
    }

    // compute interleaved mcu info
    jpeg->img_h_max = h_max;
    jpeg->img_v_max = v_max;
    jpeg->mcu_width = h_max * 8;
    jpeg->mcu_height = v_max * 8;
    // these sizes can't be more than 17 bits
    jpeg->mcus_x = (width_ + jpeg->mcu_width - 1) / jpeg->mcu_width;
    jpeg->mcus_y = (height_ + jpeg->mcu_height - 1) / jpeg->mcu_height;

    for (i = 0; i < jpeg->components; ++i) {
        // number of effective pixels (e.g. for non-interleaved MCU)
        jpeg->img_comp[i].x =
            (width_ * jpeg->img_comp[i].hsampling + h_max - 1) / h_max;
        jpeg->img_comp[i].y =
            (height_ * jpeg->img_comp[i].vsampling + v_max - 1) / v_max;
        // to simplify generation, we'll allocate enough memory to decode
        // the bogus oversized data from using interleaved MCUs and their
        // big blocks (e.g. a 16x16 iMCU on an image of width 33); we won't
        // discard the extra data until colorspace conversion
        //
        // mcus_x, mcus_y: <=17 bits; comp[i].hsampling and .v are <=4 (checked earlier)
        // so these muls can't overflow with 32-bit ints (which we require)
        jpeg->img_comp[i].w2 = jpeg->mcus_x *
            jpeg->img_comp[i].hsampling * 8;
        jpeg->img_comp[i].h2 = jpeg->mcus_y *
            jpeg->img_comp[i].vsampling * 8;
        jpeg->img_comp[i].coeff = nullptr;
        jpeg->img_comp[i].raw_coeff = nullptr;
        jpeg->img_comp[i].line_buffer = nullptr;
        jpeg->img_comp[i].raw_data =
            mallocMad2(jpeg->img_comp[i].w2, jpeg->img_comp[i].h2, 15);
        if (jpeg->img_comp[i].raw_data == nullptr) {
            freeComponents(jpeg, i+1);
            LOG(ERROR) << "failed to allocate memory";
            return false;
        }
        // align blocks for idct using mmx/sse
        jpeg->img_comp[i].data =
            (uint8_t*)(((size_t) jpeg->img_comp[i].raw_data + 15) & ~15);
        if (jpeg->progressive) {
            // w2, h2 are multiples of 8 (see above)
            jpeg->img_comp[i].coeff_w = jpeg->img_comp[i].w2 / 8;
            jpeg->img_comp[i].coeff_h = jpeg->img_comp[i].h2 / 8;
            jpeg->img_comp[i].raw_coeff = mallocMad3(jpeg->img_comp[i].w2,
                jpeg->img_comp[i].h2, sizeof(int16_t), 15);
            if (jpeg->img_comp[i].raw_coeff == nullptr) {
                freeComponents(jpeg, i+1);
                LOG(ERROR) << "failed to allocate memory";
                return false;
            }
            jpeg->img_comp[i].coeff =
                (int16_t*)(((size_t) jpeg->img_comp[i].raw_coeff + 15) & ~15);
        }
    }

    return true;
}

bool JpegDecoder::parseSOS(JpegDecodeData *jpeg) {
    int32_t length = file_data_->getWordBigEndian();
    int32_t components = file_data_->getByte();
    if (!(components == 1 || components == 3 || components == 4)) {
        LOG(ERROR) << "Invalid SOS component count: " << components
                   << ", valid value: Gray(1), YCbCr(3), CMYK(4).";
        return false;
    }
    jpeg->scan_n = components;  // Gray(1), YCbCr/YIQ(3), CMYK(4)
    if (length != 6 + 2 * components) {
        LOG(ERROR) << "Invalid SOS length: " << length << ", valid value: "
                   << 6 + 2 * components;
        return false;
    }

    int32_t component_id, table_ids, index;
    for (int32_t i = 0; i < components; i++) {
        component_id = file_data_->getByte();
        table_ids = file_data_->getByte();
        for (index = 0; index < jpeg->components; index++) {
            if (jpeg->img_comp[index].id == component_id) break;
        }
        if (index == jpeg->components) return false;
        jpeg->img_comp[index].dc_id = table_ids >> 4;
        if (jpeg->img_comp[index].dc_id > 3) {
            LOG(ERROR) << "Invalid table id of DC: "
                       << jpeg->img_comp[index].dc_id << ", valid value: 0-3.";
            return false;
        }
        jpeg->img_comp[index].ac_id = table_ids & 15;
        if (jpeg->img_comp[index].ac_id > 3) {
            LOG(ERROR) << "Invalid table id of AC: "
                       << jpeg->img_comp[index].ac_id << ", valid value: 0-3.";
            return false;
        }
        jpeg->order[i] = index;
    }

    jpeg->spec_start = file_data_->getByte();  // 0x00?
    jpeg->spec_end   = file_data_->getByte();  // 0x3F? should be 63, but might be 0
    int32_t value = file_data_->getByte();     // 0x00?
    jpeg->succ_high = (value >> 4);
    jpeg->succ_low  = (value & 15);
    if (jpeg->progressive) {
        if (jpeg->spec_start > 63 || jpeg->spec_end > 63  ||
            jpeg->spec_start > jpeg->spec_end || jpeg->succ_high > 13 ||
            jpeg->succ_low > 13) {
            LOG(ERROR) << "bad SOS, corrupt JPEG";
            return false;
        }
    } else {
        if (jpeg->spec_start != 0) {
            LOG(ERROR) << "bad SOS, corrupt JPEG";
            return false;
        }
        if (jpeg->succ_high != 0 || jpeg->succ_low != 0) {
            LOG(ERROR) << "bad SOS, corrupt JPEG";
            return false;
        }
        jpeg->spec_end = 63;
    }

    return true;
}

// 两种量化表：亮度量化值和色差量化值
bool JpegDecoder::parseDQT(JpegDecodeData *jpeg) {
    int32_t length = file_data_->getWordBigEndian();

    length -= 2;
    while (length > 0) {
        int32_t value = file_data_->getByte();
        int32_t precision = value >> 4;
        int32_t table_id  = value & 15;
        if (precision != 0 && precision != 1) {
            LOG(ERROR) << "Invalid quantization table precision type: "
                       << precision
                       << ", correct value: 0(8 bits), 1(16 bits).";
            return false;
        }
        bool is_16bits = (precision == 1);
        if (table_id > 3) {
            LOG(ERROR) << "Invalid quantization table id: " << table_id
                       << ", correct value: 0~3.";
            return false;
        }

        // jpeg->dequant[table_id] = new uint16_t[64];
        uint16_t* table = jpeg->dequant[table_id];
        if (is_16bits) {
            for (int32_t i = 0; i < 64; ++i) {
                table[dezigzag_indices[i]] = file_data_->getWordBigEndian();
            }
        }
        else {
            for (int32_t i = 0; i < 64; ++i) {
                table[dezigzag_indices[i]] = file_data_->getByte();
            }
        }

        length -= precision ? 129 : 65;
    }

    return (length == 0);
}

bool JpegDecoder::parseDHT(JpegDecodeData *jpeg) {
    int32_t length = file_data_->getWordBigEndian();
    if (length <= 19) {
        LOG(ERROR) << "bad DHT table length, corrupt JPEG.";
        return false;
    }

    length -= 2;
    while (length > 0) {
        int32_t value = file_data_->getByte();
        int32_t type = value >> 4;  // 0(DC table), 1(AC table)
        int32_t table_id = value & 15;  // 0~3
        if (type > 1) {
            LOG(ERROR) << "bad DHT type, corrupt JPEG.";
            return false;
        }
        if (table_id > 3) {
            LOG(ERROR) << "bad DHT table id, corrupt JPEG.";
            return false;
        }

        int32_t symbol_counts[16], count = 0;
        for (int32_t i = 0; i < 16; ++i) {
            symbol_counts[i] = file_data_->getByte();
            count += symbol_counts[i];
        }

        uint8_t *symbol;
        if (type == 0) {
            symbol = jpeg->huff_dc[table_id].symbols;
            for (int32_t i = 0; i < count; ++i) {
                symbol[i] = file_data_->getByte();
            }
            if (!buildHuffmanTable(jpeg->huff_dc + table_id, symbol_counts)) {
                return false;
            }
        }
        else {
            symbol = jpeg->huff_ac[table_id].symbols;
            for (int32_t i = 0; i < count; ++i) {
                symbol[i] = file_data_->getByte();
            }
            if (!buildHuffmanTable(jpeg->huff_ac + table_id, symbol_counts)) {
                return false;
            }
        }
        // if (type == 1) {
        //     buildFastAC(jpeg->fast_ac[table_id], jpeg->huff_ac + table_id);
        // }

        length -= (17 + count);
    }

    return (length == 0);
}

bool JpegDecoder::parseCOM() {
    int32_t value = file_data_->getWordBigEndian();
    if (value < 2) {
        LOG(ERROR) << "bad comment length, corrupt JPEG.";
        return false;
    }

    // uint8_t comment[32];
    // file_data_->getBytes(comment, 15);
    //  std::cout << "header: comment: " << comment << std::endl;
    file_data_->skip(value - 2);

    return true;
}

bool JpegDecoder::parseDRI(JpegDecodeData *jpeg) {
    int32_t length = file_data_->getWordBigEndian();
    if (length != 4) {
        LOG(ERROR) << "bad DRI length, corrupt JPEG.";
        return false;
    }

    int32_t value = file_data_->getWordBigEndian();
    jpeg->restart_interval = value;

    return true;
}

bool JpegDecoder::parseDNL() {
    int32_t length = file_data_->getWordBigEndian();
    if (length != 4) {
        LOG(ERROR) << "bad DNL length, corrupt JPEG.";
        return false;
    }

    int32_t height = file_data_->getWordBigEndian();
    if (height != height_) {
        LOG(ERROR) << "bad DNL height, corrupt JPEG.";
        return false;
    }

    return true;
}

bool JpegDecoder::processOtherSegments(int32_t marker) {
    int32_t length = file_data_->getWordBigEndian();
    if (length < 2) {
        LOG(ERROR) << "bad unknown segment length, corrupt JPEG.";
        return false;
    }

    file_data_->skip(length - 2);

    return true;
}

bool JpegDecoder::processSegments(JpegDecodeData *jpeg, uint8_t marker) {
    bool succeeded;
    switch (marker) {
        // case 0xE1~0xEF: optional segments, APP1 for exif, APP14 for adobe.
        case 0xE0:  // JFIF
            succeeded = parseAPP0(jpeg);
            break;
        case 0xE1:  // Exif, APP1
            succeeded = false;
            break;
        case 0xEE:  // Adobe APP14
            succeeded = parseAPP14(jpeg);
            break;
        case 0xDB:  // define quantization table
            succeeded = parseDQT(jpeg);
            break;
        // case 0xC0~0xCF: optional segments.
        case 0xC0:  // start of frame0, baseline DCT-based JPEG
        case 0xC2:  // start of frame2, progressive DCT-based JPEG
            if (marker == 0xC2) jpeg_->progressive = 1;
            succeeded = parseSOF(jpeg);
            break;
        case 0xC4:  // define huffman table
            succeeded = parseDHT(jpeg);
            break;
        case 0xDD:  // define restart interval
            succeeded = parseDRI(jpeg);
            break;
        case 0xFE:  // comment
            succeeded = parseCOM();
            break;
        case 0xDC:  // define number of lines
            succeeded = parseDNL();
            break;
        default:
            succeeded = processOtherSegments(marker);
    }

    return succeeded;
}

void JpegDecoder::setJpegFunctions(JpegDecodeData *jpeg) {
    jpeg->idctBlockKernel = idctDecodeBlock;
    jpeg->YCbCr2BGRKernel = YCbCr2BGRSse;
    // jpeg->YCbCr2BGRKernel = YCbCr2BGRRow;
    jpeg->resampleRowHV2Kernel = resampleRowHV2;

// #ifdef STBI_SSE2
/*    if (stbi__sse2_available()) {
      jpeg->idctBlockKernel = stbi__idct_simd;
      jpeg->YCbCr2BGRKernel = YCbCr2RgbSimd;
      jpeg->resampleRowHV2Kernel = stbi__resample_row_hv_2_simd;
   } */
// #endif
}

void JpegDecoder::freeComponents(JpegDecodeData *jpeg, int32_t ncomp) {
    for (int32_t i = 0; i < ncomp; ++i) {
        if (jpeg->img_comp[i].raw_data) {
            free(jpeg->img_comp[i].raw_data);
            jpeg->img_comp[i].raw_data = NULL;
            jpeg->img_comp[i].data = NULL;
        }
        if (jpeg->img_comp[i].raw_coeff) {
            free(jpeg->img_comp[i].raw_coeff);
            jpeg->img_comp[i].raw_coeff = 0;
            jpeg->img_comp[i].coeff = 0;
        }
        if (jpeg->img_comp[i].line_buffer) {
            free(jpeg->img_comp[i].line_buffer);
            jpeg->img_comp[i].line_buffer = NULL;
        }
    }
}

// after a restart interval, resetJpegDecoder the entropy decoder and
// the dc prediction
void JpegDecoder::resetJpegDecoder(JpegDecodeData *jpeg) {
    jpeg->code_bits = 0;
    jpeg->code_buffer = 0;
    jpeg->nomore = 0;
    jpeg->img_comp[0].dc_pred = 0;
    jpeg->img_comp[1].dc_pred = 0;
    jpeg->img_comp[2].dc_pred = 0;
    jpeg->img_comp[3].dc_pred = 0;
    jpeg->marker = NULL_MARKER;
    jpeg->todo = jpeg->restart_interval ? jpeg->restart_interval : 0x7fffffff;
    jpeg->eob_run = 0;
    // no more than 1<<31 MCUs if no restart_interal? that's plenty safe,
    // since we don't even allow 1<<30 pixels
    // std::cout << "come in resetJpegDecoder()" << std::endl;
}

static void initializeBitbuffer(BytesReader* file_data, JpegDecodeData *jpeg) {
    // std::cout << "jpeg->code_bits: " << jpeg->code_bits << std::endl;
    // std::cout << "jpeg->nomore: " << jpeg->nomore << std::endl;
    if (jpeg->code_bits > 0) return;

    uint64_t value;
    file_data->getBytes(&value, BUFFER_BYTES);
    if ((value & 0xFF00000000000000) != 0xFF00000000000000 &&
        (value & 0xFF000000000000) != 0xFF000000000000 &&
        (value & 0xFF0000000000) != 0xFF0000000000 &&
        (value & 0xFF00000000) != 0xFF00000000 &&
        (value & 0xFF000000) != 0xFF000000 &&
        (value & 0xFF0000) != 0xFF0000 &&
        (value & 0xFF00) != 0xFF00 &&
        (value & 0xFF) != 0xFF) {
        jpeg->code_buffer = __bswap_64(value);
        jpeg->code_bits = BUFFER_BITS;
    }
    // std::cout << std::showbase;
    // std::cout << "initializebuffer, jpeg->code_buffer: " << std::hex << jpeg->code_buffer << std::endl;
    // std::cout << "initializebuffer, jpeg->code_bits: " << std::dec << jpeg->code_bits << std::endl;
    // std::cout << std::noshowbase;
}

static bool global_prefix = false;
/* static void growBitBuffer2(BytesReader* file_data, JpegDecodeData *jpeg) {
    uint32_t buffer;
    uint32_t valid_bytes = 0, invalid_bytes = 0;
    bool prefix_ff = global_prefix;
    // std::cout << "before, jpeg->code_bits: " << std::dec << jpeg->code_bits << std::endl;

    memcpy(&buffer, file_data->getCurrentPosition(), BUFFER_BYTES);
    buffer = __bswap_32(buffer);
    // std::cout << std::showbase;
    // std::cout << "growBitBuffer, buffer: " << std::hex << buffer << std::endl;
    // std::cout << std::noshowbase;

    if ((!prefix_ff) && (buffer & 0xFF000000) != 0xFF000000
        && (buffer & 0xFF0000) != 0xFF0000 && (buffer & 0xFF00) != 0xFF00
        && (buffer & 0xFF) != 0xFF) {
        valid_bytes = BUFFER_BYTES;

        jpeg->code_buffer |= buffer >> jpeg->code_bits;
        jpeg->code_bits += (valid_bytes) << 3;
        if (jpeg->code_bits > BUFFER_BITS) {
            invalid_bytes = ((jpeg->code_bits - BUFFER_BITS) + 7) >> 3;
            jpeg->code_bits -= (invalid_bytes << 3);
            valid_bytes -= invalid_bytes;
        }
        file_data->skip(valid_bytes);
        // std::cout << "growBitBuffer 0, after, jpeg->code_bits: " << std::dec << jpeg->code_bits << std::endl;
        // std::cout << "growBitBuffer 0, valid_bytes: " << valid_bytes << std::endl;
    }
    else {
        uint32_t index = 0, processed_bytes = 0, ff00_index = 0;
        uint8_t current_byte;
        do {
            if (jpeg->nomore) {
                break;
            }
            if (index == 0) {
                current_byte = (buffer & 0xFF000000) >> 24;
                if (processed_bytes == BUFFER_BYTES - 1 && current_byte == 0xFF) break;
                if (prefix_ff) {
                    if (current_byte == 0xFF) {
                        buffer = (buffer << 8);
                    }
                    else if (current_byte == 0) {
                        buffer |= 0xFF000000;
                        prefix_ff = false;
                        index++;
                    }
                    else {
                        jpeg->marker = current_byte;
                        jpeg->nomore = 1;
                        prefix_ff = false;
                        // index++;
                        // break;
                    }
                }
                else {
                    if (current_byte == 0xFF) {
                        prefix_ff = true;
                        buffer = (buffer << 8);
                    }
                    else {
                        index++;
                    }
                }
            }
            else if (index == 1) {
                current_byte = (buffer & 0xFF0000) >> 16;
                if (processed_bytes == BUFFER_BYTES - 1 && current_byte == 0xFF) break;
                if (prefix_ff) {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFF000000) | ((buffer & 0xFFFF) << 8);
                    }
                    else if (current_byte == 0) {
                        buffer |= 0xFF0000;
                        prefix_ff = false;
                        index++;
                    }
                    else {
                        jpeg->marker = current_byte;
                        jpeg->nomore = 1;
                        prefix_ff = false;
                        // index++;
                        // break;
                    }
                }
                else {
                    if (current_byte == 0xFF) {
                        prefix_ff = true;
                        buffer = (buffer & 0xFF000000) | ((buffer & 0xFFFF) << 8);
                    }
                    else {
                        index++;
                    }
                }
            }
            else if (index == 2) {
                current_byte = (buffer & 0xFF00) >> 8;
                if (processed_bytes == BUFFER_BYTES - 1 && current_byte == 0xFF) break;
                if (prefix_ff) {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFFFF0000) | ((buffer & 0xFF) << 8);
                    }
                    else if (current_byte == 0) {
                        buffer |= 0xFF00;
                        prefix_ff = false;
                        ff00_index = 2;
                        index++;
                    }
                    else {
                        jpeg->marker = current_byte;
                        jpeg->nomore = 1;
                        prefix_ff = false;
                        // index++;
                        // break;
                    }
                }
                else {
                    if (current_byte == 0xFF) {
                        prefix_ff = true;
                        buffer = (buffer & 0xFFFF0000) | ((buffer & 0xFF) << 8);
                    }
                    else {
                        index++;
                    }
                }
            }
            else {  // index == 3
                current_byte = buffer & 0xFF;
                if (current_byte == 0xFF) break;
                if (prefix_ff) {
                    if (current_byte == 0xFF) {
                    }
                    else if (current_byte == 0) {
                        buffer |= 0xFF;
                        prefix_ff = false;
                        ff00_index = 3;
                        index++;
                    }
                    else {
                        jpeg->marker = current_byte;
                        jpeg->nomore = 1;
                        prefix_ff = false;
                        // index++;
                    }
                }
                else {
                    if (current_byte == 0xFF) {
                        prefix_ff = true;
                    }
                    else {
                        index++;
                    }
                }
            }
            processed_bytes++;

            if (processed_bytes == BUFFER_BYTES && index == 0) {
                file_data->skip(BUFFER_BYTES);
                memcpy(&buffer, file_data->getCurrentPosition(), BUFFER_BYTES);
                buffer = __bswap_32(buffer);
                // buffer |= 0xFF000000;
                processed_bytes == 0;
            }
        } while (processed_bytes < BUFFER_BYTES);
        // std::cout << "index: " << index << std::endl;
        // std::cout << "processed_bytes: " << processed_bytes << std::endl;

        global_prefix = prefix_ff;
        jpeg->code_buffer |= buffer >> jpeg->code_bits;
        jpeg->code_bits += (index) << 3;
        if (jpeg->code_bits > BUFFER_BITS) {
            invalid_bytes = ((jpeg->code_bits - BUFFER_BITS) + 7) >> 3;
            jpeg->code_bits -= (invalid_bytes << 3);
            valid_bytes = index - invalid_bytes;  //remove
            invalid_bytes = ff00_index >= valid_bytes ? invalid_bytes + 1 : invalid_bytes;
        }
        // std::cout << "valid_bytes: " << valid_bytes << std::endl;
        processed_bytes -= invalid_bytes;
        file_data->skip(processed_bytes);
        // std::cout << "growBitBuffer 1, after, jpeg->code_bits: " << std::dec << jpeg->code_bits << std::endl;
        // std::cout << "growBitBuffer 1, processed_bytes: " << processed_bytes << std::endl;
    }
} */

static __m128i swap_index = _mm_set_epi8(15, 14, 13, 12, 11, 10, 9, 8,
                                         0, 1, 2, 3, 4, 5, 6, 7);

// static void growBitBuffer(BytesReader* file_data, JpegDecodeData *jpeg) {
inline void growBitBuffer(BytesReader* file_data, JpegDecodeData *jpeg) {
    uint64_t buffer;
    uint32_t valid_bytes = 0, invalid_bytes = 0;
    bool prefix_ff = global_prefix;

    // memcpy(&buffer, file_data->getCurrentPosition(), BUFFER_BYTES);
    // buffer = __bswap_64(buffer);

    uint8_t* current_data = file_data->getCurrentPosition();
    __m128i value0 = _mm_lddqu_si128((__m128i const*)current_data);
    __m128i value1 = _mm_shuffle_epi8(value0, swap_index);
    buffer = _mm_extract_epi64(value1, 0);

    // std::cout << "before, jpeg->code_bits: " << std::dec << jpeg->code_bits << std::endl;
    // std::cout << std::showbase;
    // std::cout << "growBitBuffer, buffer: " << std::hex << buffer << std::endl;
    // std::cout << std::noshowbase;

    if ((!prefix_ff) && (buffer & 0xFF00000000000000) != 0xFF00000000000000 &&
                        (buffer & 0xFF000000000000) != 0xFF000000000000 &&
                        (buffer & 0xFF0000000000) != 0xFF0000000000 &&
                        (buffer & 0xFF00000000) != 0xFF00000000 &&
                        (buffer & 0xFF000000) != 0xFF000000 &&
                        (buffer & 0xFF0000) != 0xFF0000 &&
                        (buffer & 0xFF00) != 0xFF00 &&
                        (buffer & 0xFF) != 0xFF) {
        valid_bytes = BUFFER_BYTES;

        jpeg->code_buffer |= buffer >> jpeg->code_bits;
        jpeg->code_bits += (valid_bytes) << 3;
        if (jpeg->code_bits > BUFFER_BITS) {
            invalid_bytes = ((jpeg->code_bits - BUFFER_BITS) + 7) >> 3;
            jpeg->code_bits -= (invalid_bytes << 3);
            valid_bytes -= invalid_bytes;
        }
        file_data->skip(valid_bytes);
        // std::cout << "growBitBuffer 0, after, jpeg->code_bits: " << std::dec << jpeg->code_bits << std::endl;
        // std::cout << "growBitBuffer 0, valid_bytes: " << valid_bytes << std::endl;
    }
    else {
        uint32_t index = 0, processed_bytes = 0, ff00_index = 0;
        uint8_t current_byte;
        do {
            if (jpeg->nomore) {
                break;
            }
            if (index == 0) {
                current_byte = (buffer & 0xFF00000000000000) >> 56;
                if (processed_bytes == BUFFER_BYTES - 1 &&
                    current_byte == 0xFF) {
                    break;
                }
                if (prefix_ff) {
                    if (current_byte == 0xFF) {
                        buffer = (buffer << 8);
                    }
                    else if (current_byte == 0) {
                        buffer |= 0xFF00000000000000;
                        prefix_ff = false;
                        index++;
                    }
                    else {
                        jpeg->marker = current_byte;
                        jpeg->nomore = 1;
                        prefix_ff = false;
                    }
                }
                else {
                    if (current_byte == 0xFF) {
                        buffer = (buffer << 8);
                        prefix_ff = true;
                    }
                    else {
                        index++;
                    }
                }
            }
            else if (index == 1) {
                current_byte = (buffer & 0xFF000000000000) >> 48;
                if (processed_bytes == BUFFER_BYTES - 1 &&
                    current_byte == 0xFF) {
                    break;
                }
                if (prefix_ff) {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFF00000000000000) |
                                 ((buffer & 0xFFFFFFFFFFFF) << 8);
                    }
                    else if (current_byte == 0) {
                        buffer |= 0xFF000000000000;
                        prefix_ff = false;
                        index++;
                    }
                    else {
                        jpeg->marker = current_byte;
                        jpeg->nomore = 1;
                        prefix_ff = false;
                    }
                }
                else {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFF00000000000000) |
                                 ((buffer & 0xFFFFFFFFFFFF) << 8);
                        prefix_ff = true;
                    }
                    else {
                        index++;
                    }
                }
            }
            else if (index == 2) {
                current_byte = (buffer & 0xFF0000000000) >> 40;
                if (processed_bytes == BUFFER_BYTES - 1 &&
                    current_byte == 0xFF) {
                    break;
                }
                if (prefix_ff) {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFFFF000000000000) |
                                 ((buffer & 0xFFFFFFFFFF) << 8);
                    }
                    else if (current_byte == 0) {
                        buffer |= 0xFF0000000000;
                        prefix_ff = false;
                        ff00_index = 2;
                        index++;
                    }
                    else {
                        jpeg->marker = current_byte;
                        jpeg->nomore = 1;
                        prefix_ff = false;
                    }
                }
                else {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFFFF000000000000) |
                                 ((buffer & 0xFFFFFFFFFF) << 8);
                        prefix_ff = true;
                    }
                    else {
                        index++;
                    }
                }
            }
            else if (index == 3) {
                current_byte = (buffer & 0xFF00000000) >> 32;
                if (processed_bytes == BUFFER_BYTES - 1 &&
                    current_byte == 0xFF) {
                    break;
                }
                if (prefix_ff) {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFFFFFF0000000000) |
                                 ((buffer & 0xFFFFFFFF) << 8);
                    }
                    else if (current_byte == 0) {
                        buffer |= 0xFF00000000;
                        prefix_ff = false;
                        ff00_index = 3;
                        index++;
                    }
                    else {
                        jpeg->marker = current_byte;
                        jpeg->nomore = 1;
                        prefix_ff = false;
                    }
                }
                else {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFFFFFF0000000000) |
                                 ((buffer & 0xFFFFFFFF) << 8);
                        prefix_ff = true;
                    }
                    else {
                        index++;
                    }
                }
            }
            else if (index == 4) {
                current_byte = (buffer & 0xFF000000) >> 24;
                if (processed_bytes == BUFFER_BYTES - 1 &&
                    current_byte == 0xFF) {
                    break;
                }
                if (prefix_ff) {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFFFFFFFF00000000) |
                                 ((buffer & 0xFFFFFF) << 8);
                    }
                    else if (current_byte == 0) {
                        buffer |= 0xFF000000;
                        prefix_ff = false;
                        ff00_index = 4;
                        index++;
                    }
                    else {
                        jpeg->marker = current_byte;
                        jpeg->nomore = 1;
                        prefix_ff = false;
                    }
                }
                else {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFFFFFFFF00000000) |
                                 ((buffer & 0xFFFFFF) << 8);
                        prefix_ff = true;
                    }
                    else {
                        index++;
                    }
                }
            }
            else if (index == 5) {
                current_byte = (buffer & 0xFF0000) >> 16;
                if (processed_bytes == BUFFER_BYTES - 1 &&
                    current_byte == 0xFF) {
                    break;
                }
                if (prefix_ff) {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFFFFFFFFFF000000) |
                                 ((buffer & 0xFFFF) << 8);
                    }
                    else if (current_byte == 0) {
                        buffer |= 0xFF0000;
                        prefix_ff = false;
                        ff00_index = 5;
                        index++;
                    }
                    else {
                        jpeg->marker = current_byte;
                        jpeg->nomore = 1;
                        prefix_ff = false;
                    }
                }
                else {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFFFFFFFFFF000000) |
                                 ((buffer & 0xFFFF) << 8);
                        prefix_ff = true;
                    }
                    else {
                        index++;
                    }
                }
            }
            else if (index == 6) {
                current_byte = (buffer & 0xFF00) >> 8;
                if (processed_bytes == BUFFER_BYTES - 1 &&
                    current_byte == 0xFF) {
                    break;
                }
                if (prefix_ff) {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFFFFFFFFFFFF0000) |
                                 ((buffer & 0xFF) << 8);
                    }
                    else if (current_byte == 0) {
                        buffer |= 0xFF00;
                        prefix_ff = false;
                        ff00_index = 6;
                        index++;
                    }
                    else {
                        jpeg->marker = current_byte;
                        jpeg->nomore = 1;
                        prefix_ff = false;
                    }
                }
                else {
                    if (current_byte == 0xFF) {
                        buffer = (buffer & 0xFFFFFFFFFFFF0000) |
                                 ((buffer & 0xFF) << 8);
                        prefix_ff = true;
                    }
                    else {
                        index++;
                    }
                }
            }
            else {  // index == 7
                current_byte = buffer & 0xFF;
                if (current_byte == 0xFF) {
                    break;
                }
                else {
                    if (prefix_ff) {
                        if (current_byte == 0) {
                            buffer |= 0xFF;
                            prefix_ff = false;
                            ff00_index = 7;
                            index++;
                        }
                        else {
                            jpeg->marker = current_byte;
                            jpeg->nomore = 1;
                            prefix_ff = false;
                        }
                    }
                    else {
                        index++;
                    }
                }
            }
            processed_bytes++;

            if (processed_bytes == BUFFER_BYTES && index == 0) {
                file_data->skip(BUFFER_BYTES);
                // memcpy(&buffer, file_data->getCurrentPosition(), BUFFER_BYTES);
                // buffer = __bswap_64(buffer);
                uint8_t* current_data = file_data->getCurrentPosition();
                __m128i value0 = _mm_lddqu_si128((__m128i const*)current_data);
                __m128i value1 = _mm_shuffle_epi8(value0, swap_index);
                buffer = _mm_extract_epi64(value1, 0);

                processed_bytes == 0;
            }
        } while (processed_bytes < BUFFER_BYTES);
        // std::cout << "index: " << index << std::endl;
        // std::cout << "processed_bytes: " << processed_bytes << std::endl;

        global_prefix = prefix_ff;
        jpeg->code_buffer |= buffer >> jpeg->code_bits;
        jpeg->code_bits += (index) << 3;
        if (jpeg->code_bits > BUFFER_BITS) {
            invalid_bytes = ((jpeg->code_bits - BUFFER_BITS) + 7) >> 3;
            jpeg->code_bits -= (invalid_bytes << 3);
            valid_bytes = index - invalid_bytes;
            invalid_bytes = ff00_index >= valid_bytes ? invalid_bytes + 1 : invalid_bytes;
        }
        // std::cout << "valid_bytes: " << valid_bytes << std::endl;
        processed_bytes -= invalid_bytes;
        file_data->skip(processed_bytes);
        // std::cout << "growBitBuffer 1, after, jpeg->code_bits: " << std::dec << jpeg->code_bits << std::endl;
        // std::cout << "growBitBuffer 1, processed_bytes: " << processed_bytes << std::endl;
    }
}

// if there's a pending marker from the entropy stream, return that
// otherwise, fetch from the stream and get a marker. if there's no
// marker, return 0xff, which is never a valid marker value
uint8_t JpegDecoder::getMarker(JpegDecodeData *jpeg) {
    uint8_t marker;
    // std::cout << "init, marker: " << std::hex << (int)marker << std::noshowbase << std::dec << std::endl;
    if (jpeg->marker != NULL_MARKER) {
        marker = jpeg->marker;
        jpeg->marker = NULL_MARKER;
        // std::cout << "if (jpeg->marker != NULL_MARKER), marker: " << std::hex << (int)marker << std::noshowbase << std::dec << std::endl;
        return marker;
    }

    marker = file_data_->getByte();
    // std::cout << "readed, marker: " << std::hex << (int)marker << std::noshowbase << std::dec << std::endl;
    if (marker != 0xFF) {
        LOG(ERROR) << "invalid segment identifier.";
        // std::cout << "if (marker != 0xFF), marker: " << std::hex << (int)marker << std::noshowbase << std::dec << std::endl;
        return 0xFF;
    }
    while (marker == 0xFF) {
        marker = file_data_->getByte();
    }
    // std::cout << "end, marker: " << std::hex << (int)marker << std::noshowbase << std::dec << std::endl;

    return marker;
}

uint8_t JpegDecoder::getMarker1(JpegDecodeData *jpeg) {
    uint8_t marker = 0;
    std::cout << "init, marker: " << std::hex << (int)marker << std::noshowbase << std::dec << std::endl;
    if (jpeg->marker != NULL_MARKER) {
        marker = jpeg->marker;
        jpeg->marker = NULL_MARKER;
        std::cout << "if (jpeg->marker != NULL_MARKER), marker: " << std::hex << (int)marker << std::noshowbase << std::dec << std::endl;
        return marker;
    }

    marker = file_data_->getByte();
    std::cout << "readed, marker: " << std::hex << (int)marker << std::noshowbase << std::dec << std::endl;
    // uint32_t previous0 = file_data_->getpreviousByte();
    // std::cout << "in decodeData(), previous0: " << std::hex << previous0 << std::endl;
    if (marker != 0xFF) {
        LOG(ERROR) << "invalid segment identifier.";
        return 0xFF;
    }
    while (marker == 0xFF) {
        marker = file_data_->getByte();
        std::cout << "while (marker == 0xFF), marker: " << std::hex << (int)marker << std::noshowbase << std::dec << std::endl;
    }
    std::cout << "end, marker: " << std::hex << (int)marker << std::noshowbase << std::dec << std::endl;

    return marker;
}

// get some unsigned bits
inline int32_t getBits(JpegDecodeData *jpeg, BytesReader* file_data,
                       int32_t bit_length) {
    uint32_t k;
    if (jpeg->code_bits < bit_length) {
        // std::cout << "before growBitBuffer in getbits" << std::endl;
        growBitBuffer(file_data, jpeg);
    }
    k = ROTATE_BITS(jpeg->code_buffer, bit_length);
    jpeg->code_buffer = k & ~bit_mask[bit_length];
    k &= bit_mask[bit_length];
    jpeg->code_bits -= bit_length;
    return k;
}

inline int32_t getBit(JpegDecodeData *jpeg, BytesReader* file_data) {
    size_t value;
    if (jpeg->code_bits < 1) {
        // std::cout << "before growBitBuffer in getbit" << std::endl;
        growBitBuffer(file_data, jpeg);
    }
    value = jpeg->code_buffer;
    jpeg->code_buffer <<= 1;
    --jpeg->code_bits;

    return value & (1 << (BUFFER_BITS - 1));
    // return value & 0x8000000000000000;
}

// combined JPEG 'receive' and JPEG 'extend', since baseline
// always extends everything it receives.
inline int32_t extendReceive(JpegDecodeData *jpeg, BytesReader* file_data,
                             int32_t bit_length) {
    if (jpeg->code_bits < bit_length) {
        growBitBuffer(file_data, jpeg);
    }

    int32_t sign = jpeg->code_buffer >> (BUFFER_BITS - 1); // sign bit always in MSB; 0 if MSB clear (positive), 1 if MSB set (negative)
    uint64_t value = ROTATE_BITS(jpeg->code_buffer, bit_length);
    // value = ROTATE_BITS(jpeg->code_buffer, (uint64_t)bit_length);
    value &= ((1 << bit_length) - 1);
    value += (((-(1 << bit_length)) + 1) & (sign - 1));

    jpeg->code_buffer <<= bit_length;
    jpeg->code_bits -= bit_length;

    return value;
}


// static int huffman_count = 0;

// decode a jpeg huffman value(bit length) from the bitstream
inline
int32_t JpegDecoder::decodeHuffmanData(JpegDecodeData *jpeg,
                                       HuffmanLookupTable *huffman_table) {
    if (jpeg->code_bits < LOOKAHEAD_BITS) {
        growBitBuffer(file_data_, jpeg);
    }

    // look at the top LOOKAHEAD_BITS and fast indexed table to determine
    // bit length and symbol if the bits is <= LOOKAHEAD_BITS
    uint16_t bits = (jpeg->code_buffer >> (BUFFER_BITS - LOOKAHEAD_BITS)) &
                    ((1 << LOOKAHEAD_BITS) - 1);
    int32_t value = huffman_table->lookups[bits];
    int32_t bit_length;
    if (value != 0xFFFF) {
        bit_length = (value >> 8) & 0xFF;
        jpeg->code_buffer <<= bit_length;
        jpeg->code_bits -= bit_length;
        return (value & 0xFF);
    }

    if (jpeg->code_bits < MAX_BITS) {
        growBitBuffer(file_data_, jpeg);
    }

    bits = jpeg->code_buffer >> (BUFFER_BITS - MAX_BITS);
    for (bit_length = LOOKAHEAD_BITS + 1; ; ++bit_length) {
        if (bits < huffman_table->max_codes[bit_length]) {
            break;
        }
    }
    if (bit_length >= 17) {  // error! code not found
        jpeg->code_bits -= 16;
        return -1;
    }

    // convert the huffman code to the symbol id
    int32_t index = ((jpeg->code_buffer >> (BUFFER_BITS - bit_length)) &
                    bit_mask[bit_length]) + huffman_table->delta[bit_length];
    jpeg->code_buffer <<= bit_length;
    jpeg->code_bits -= bit_length;

    return huffman_table->symbols[index];
}

bool JpegDecoder::decodeProgressiveDCBlock(JpegDecodeData *jpeg,
                                           int16_t decoded_data[64],
                                           HuffmanLookupTable *huffman_dc,
                                           int32_t component_id) {
    if (jpeg->spec_end != 0) {
        LOG(ERROR) << "can't merge DC and AC, Corrupt JPEG";
        return false;
    }

    if (jpeg->succ_high == 0) {  // first scan for DC coefficient.
        memset(decoded_data, 0, 64 * sizeof(int16_t));
        int32_t bit_length = decodeHuffmanData(jpeg, huffman_dc);
        if (bit_length < 0 || bit_length > 15) {
            LOG(ERROR) << "Invalid bit length of DC value from huffman "
                       << "decoding: " << bit_length << ", valid value: 0-15.";
        }
        int32_t value_diff = bit_length ?
                             extendReceive(jpeg, file_data_, bit_length) : 0;

        int32_t dc_value = jpeg->img_comp[component_id].dc_pred + value_diff;
        jpeg->img_comp[component_id].dc_pred = dc_value;
        decoded_data[0] = (int16_t)(dc_value * (1 << jpeg->succ_low));
    } else {  // refinement scan for DC coefficient.
        if (getBit(jpeg, file_data_)) {
            decoded_data[0] += (int16_t)(1 << jpeg->succ_low);
        }
    }

    return true;
}

bool JpegDecoder::decodeProgressiveACBlock(JpegDecodeData *jpeg,
                                           int16_t decoded_data[64],
                                           HuffmanLookupTable *huffman_ac) {
    if (jpeg->spec_start == 0) {
       LOG(ERROR) << "can't merge dc and ac, Corrupt JPEG";
       return false;
    }

    int32_t combined_value, zeroes, bit_length, ac_index;
    if (jpeg->succ_high == 0) {
        int32_t shift = jpeg->succ_low;

        if (jpeg->eob_run) {
            --jpeg->eob_run;
            return true;
        }

        int32_t zig_index, value;
        ac_index = jpeg->spec_start;
        do {
            combined_value = decodeHuffmanData(jpeg, huffman_ac);
            zeroes = combined_value >> 4;
            bit_length = combined_value & 15;
            if (bit_length == 0) {
                if (zeroes < 15) {
                    jpeg->eob_run = (1 << zeroes);
                    if (zeroes) {
                        jpeg->eob_run += getBits(jpeg, file_data_, zeroes);
                    }
                    --jpeg->eob_run;
                    break;
                }
                ac_index += 16;
            } else {
                ac_index += zeroes;
                zig_index = dezigzag_indices[ac_index++];
                value = extendReceive(jpeg, file_data_, bit_length) *
                        (1 << shift);
                decoded_data[zig_index] = (int16_t)value;
            }
        } while (ac_index <= jpeg->spec_end);
    } else {
        // refinement scan for these AC coefficients
        int16_t bit = (int16_t)(1 << jpeg->succ_low);

        if (jpeg->eob_run) {
            --jpeg->eob_run;
            for (ac_index = jpeg->spec_start; ac_index <= jpeg->spec_end;
                 ++ac_index) {
                int16_t *data = &decoded_data[dezigzag_indices[ac_index]];
                if (*data != 0) {
                    if (getBit(jpeg, file_data_)) {
                        if ((*data & bit)==0) {
                            if (*data > 0) {
                                *data += bit;
                            }
                            else {
                                *data -= bit;
                            }
                        }
                    }
                }
            }
        } else {
            ac_index = jpeg->spec_start;
            do {
                combined_value = decodeHuffmanData(jpeg, huffman_ac);
                zeroes = combined_value >> 4;
                bit_length = combined_value & 15;
                if (bit_length == 0) {
                    if (zeroes < 15) {
                        jpeg->eob_run = (1 << zeroes) - 1;
                        if (zeroes) {
                            jpeg->eob_run += getBits(jpeg, file_data_, zeroes);
                        }
                        zeroes = 64; // force end of block
                    } else {
                        // zeroes =15 & bit_length=0 should write 16 0s, so we just do
                        // a run of 15 0s and then write bit_length (which is 0),
                        // so we don't have to do anything special here
                    }
                } else {
                    if (bit_length != 1) {
                        LOG(ERROR) << "bad huffman code, Corrupt JPEG";
                        return false;
                    }
                    if (getBit(jpeg, file_data_)) {  // sign bit
                        bit_length = bit;
                    }
                    else {
                        bit_length = -bit;
                    }
                }

                // advance by zeroes
                while (ac_index <= jpeg->spec_end) {
                    int16_t *data = &decoded_data[dezigzag_indices[ac_index++]];
                    if (*data != 0) {
                        if (getBit(jpeg, file_data_)) {
                            if ((*data & bit)==0) {
                                if (*data > 0) {
                                    *data += bit;
                                }
                                else {
                                    *data -= bit;
                                }
                            }
                        }
                    } else {
                        if (zeroes == 0) {
                            *data = (int16_t) bit_length;
                            break;
                        }
                        --zeroes;
                    }
                }
            } while (ac_index <= jpeg->spec_end);
        }
    }

    return true;
}

// decode one 64-entry block.
bool JpegDecoder::decodeBlock(JpegDecodeData *jpeg, int16_t decoded_data[64],
                              HuffmanLookupTable *huffman_dc,
                              HuffmanLookupTable *huffman_ac,
                              int32_t component_id, uint16_t *dequant_table) {
    // decode DC component.
    int32_t bit_length = decodeHuffmanData(jpeg, huffman_dc);
    if (bit_length < 0 || bit_length > 15) {
        LOG(ERROR) << "Invalid bit length of DC value from huffman decoding: "
                   << bit_length << ", valid value: 0-15.";
    }

    int32_t value = bit_length ?
                    extendReceive(jpeg, file_data_, bit_length) : 0;
    int32_t dc_value = jpeg->img_comp[component_id].dc_pred + value;
    jpeg->img_comp[component_id].dc_pred = dc_value;
    memset(decoded_data, 0, 64 * sizeof(int16_t));
    decoded_data[0] = (int16_t)(dc_value * dequant_table[0]);

    // decode AC components.
    int32_t combined_value, zeroes, zig_index, ac_index = 1;
    do {
        // combined_value: number of zero + bit length of incoming code of the
        // jpeg fixed encoding table.
        combined_value = decodeHuffmanData(jpeg, huffman_ac);
        zeroes = combined_value >> 4;
        bit_length = combined_value & 15;
        if (bit_length == 0) {
            if (zeroes != 0xF0) {
                break;  // end of block
            }
            ac_index += 16;
        } else {
            ac_index += zeroes;
            zig_index = dezigzag_indices[ac_index++];
            value = extendReceive(jpeg, file_data_, bit_length) *
                    dequant_table[zig_index];
            decoded_data[zig_index] = (int16_t)value;
        }
    } while (ac_index < 64);

    return true;
}

bool JpegDecoder::parseEntropyCodedData(JpegDecodeData *jpeg) {
    bool succeeded;
    resetJpegDecoder(jpeg);
    // initializeBitbuffer(file_data_, jpeg);
    // std::cout << "come in parseEntropyCodedData(). " << std::endl;
    // std::cout << "jpeg->todo: " << jpeg->todo << std::endl;
    if (!jpeg->progressive) {
        // std::cout << "come in non progressive processing." << std::endl;
        if (jpeg->scan_n == 1) {
            // std::cout << "before decodeBlock, scan_n: " << jpeg->scan_n << std::endl;
            int32_t i, j;
            SIMD_ALIGN16(int16_t, data[64]);
            int32_t comp_id = jpeg->order[0];
            // non-interleaved data, we just need to process one block at a time,
            // in trivial scanline order
            // number of blocks to do just depends on how many actual "pixels" this
            // component has, independent of interleaved MCU blocking and such
            int32_t width = (jpeg->img_comp[comp_id].x + 7) >> 3;
            int32_t height = (jpeg->img_comp[comp_id].y + 7) >> 3;
            // std::cout << "width: " << width << ", height: " << height << std::endl;
            for (j = 0; j < height; ++j) {
                for (i = 0; i < width; ++i) {
                    int32_t ac_id = jpeg->img_comp[comp_id].ac_id;
                    HuffmanLookupTable *huffman_dc = jpeg->huff_dc +
                        jpeg->img_comp[comp_id].dc_id;
                    HuffmanLookupTable *huffman_ac = jpeg->huff_ac + ac_id;
                    uint16_t *dequant_table =
                        jpeg->dequant[jpeg->img_comp[comp_id].quant_id];
                    succeeded = decodeBlock(jpeg, data, huffman_dc, huffman_ac,
                                            comp_id, dequant_table);
                    if (!succeeded) return false;
                    // std::cout << "after decodeBlock" << std::endl;
                    uint8_t *output = jpeg->img_comp[comp_id].data +
                        jpeg->img_comp[comp_id].w2 * j * 8 + i * 8;
                    jpeg->idctBlockKernel(output, jpeg->img_comp[comp_id].w2, data);

                    // every data block is an MCU, so countdown the restart interval
                    if (--jpeg->todo <= 0) {
                        // if it's NOT a restart, then just bail, so we get corrupt data
                        // rather than no data
                        if (!DRI_RESTART(jpeg->marker)) {
                            return true;
                        }
                        resetJpegDecoder(jpeg);
                        // initializeBitbuffer(file_data_, jpeg);
                    }
                }
            }
            return true;
        } else {  // interleaved
            // std::cout << "before decodeBlock, scan_n: " << jpeg->scan_n << std::endl;
            int32_t i, j, k, x, y;
            SIMD_ALIGN16(int16_t, data[64]);
            for (j = 0; j < jpeg->mcus_y; ++j) {
                for (i = 0; i < jpeg->mcus_x; ++i) {
                    // scan an interleaved mcu... process scan_n components in order
                    for (k = 0; k < jpeg->scan_n; ++k) {
                        int32_t comp_id = jpeg->order[k];
                        // scan out an mcu's worth of this component; that's just determined
                        // by the basic H and V specified for the component
                        for (y = 0; y < jpeg->img_comp[comp_id].vsampling; ++y) {
                            for (x = 0; x < jpeg->img_comp[comp_id].hsampling; ++x) {
                                int32_t x2 = (i * jpeg->img_comp[comp_id].hsampling +
                                              x) * 8;
                                int32_t y2 = (j * jpeg->img_comp[comp_id].vsampling +
                                              y) * 8;
                                int32_t ac_id = jpeg->img_comp[comp_id].ac_id;
                                HuffmanLookupTable *huffman_dc = jpeg->huff_dc
                                    + jpeg->img_comp[comp_id].dc_id;
                                HuffmanLookupTable *huffman_ac = jpeg->huff_ac
                                    + ac_id;
                                uint16_t *dequant_table =
                                    jpeg->dequant[jpeg->img_comp[comp_id].quant_id];
                                succeeded = decodeBlock(jpeg, data, huffman_dc,
                                    huffman_ac, comp_id, dequant_table);
                                if (!succeeded) return false;
                                uint8_t *output = jpeg->img_comp[comp_id].data +
                                    jpeg->img_comp[comp_id].w2 * y2 + x2;
                                jpeg->idctBlockKernel(output,
                                    jpeg->img_comp[comp_id].w2, data);
                            }
                        }
                    }
                    // after all interleaved components, that's an interleaved MCU,
                    // so now count down the restart interval
                    if (--jpeg->todo <= 0) {
                        if (!DRI_RESTART(jpeg->marker)) return true;
                        resetJpegDecoder(jpeg);
                        // initializeBitbuffer(file_data_, jpeg);
                    }
                }
            }
            return true;
        }
    } else {
        // std::cout << "come in progressive processing." << std::endl;
        if (jpeg->scan_n == 1) {
            int32_t i, j;
            int32_t comp_id = jpeg->order[0];
            // non-interleaved data, we just need to process one block at a time,
            // in trivial scanline order
            // number of blocks to do just depends on how many actual "pixels" this
            // component has, independent of interleaved MCU blocking and such
            int32_t width = (jpeg->img_comp[comp_id].x + 7) >> 3;
            int32_t height = (jpeg->img_comp[comp_id].y + 7) >> 3;
            for (j = 0; j < height; ++j) {
                for (i = 0; i < width; ++i) {
                    int16_t *data = jpeg->img_comp[comp_id].coeff +
                        64 * (i + j * jpeg->img_comp[comp_id].coeff_w);
                    if (jpeg->spec_start == 0) {
                        HuffmanLookupTable *huffman_dc =
                            &jpeg->huff_dc[jpeg->img_comp[comp_id].dc_id];
                        succeeded = decodeProgressiveDCBlock(jpeg, data,
                                                             huffman_dc, comp_id);
                        if (!succeeded) return false;
                    } else {
                        int32_t ac_id = jpeg->img_comp[comp_id].ac_id;
                        succeeded = decodeProgressiveACBlock(jpeg, data,
                                      &jpeg->huff_ac[ac_id]);
                        if (!succeeded) return false;
                    }
                    // every data block is an MCU, so countdown the restart interval
                    if (--jpeg->todo <= 0) {
                        if (!DRI_RESTART(jpeg->marker)) return true;
                        resetJpegDecoder(jpeg);
                        // initializeBitbuffer(file_data_, jpeg);
                    }
                }
            }
            return true;
        } else {  // interleaved
            int32_t i, j, k, x, y;
            for (j = 0; j < jpeg->mcus_y; ++j) {
                for (i = 0; i < jpeg->mcus_x; ++i) {
                    // scan an interleaved mcu... process scan_n components in order
                    for (k = 0; k < jpeg->scan_n; ++k) {
                        int32_t comp_id = jpeg->order[k];
                        // scan out an mcu's worth of this component; that's just determined
                        // by the basic H and V specified for the component
                        for (y = 0; y < jpeg->img_comp[comp_id].vsampling; ++y) {
                            for (x = 0; x < jpeg->img_comp[comp_id].hsampling; ++x) {
                                int32_t x2 = (i*jpeg->img_comp[comp_id].hsampling +
                                              x);
                                int32_t y2 = (j*jpeg->img_comp[comp_id].vsampling +
                                              y);
                                int16_t *data = jpeg->img_comp[comp_id].coeff +
                                    64 * (x2 + y2 * jpeg->img_comp[comp_id].coeff_w);
                                HuffmanLookupTable *huffman_dc =
                                    &jpeg->huff_dc[jpeg->img_comp[comp_id].dc_id];
                                succeeded = decodeProgressiveDCBlock(jpeg,
                                    data, huffman_dc, comp_id);
                                if (!succeeded) return false;
                            }
                        }
                    }
                    // after all interleaved components, that's an interleaved MCU,
                    // so now count down the restart interval
                    if (--jpeg->todo <= 0) {
                        if (!DRI_RESTART(jpeg->marker)) return true;
                        resetJpegDecoder(jpeg);
                        // initializeBitbuffer(file_data_, jpeg);
                    }
                }
            }
            return true;
        }
    }
}

bool JpegDecoder::sampleConvertColor(int32_t stride, uint8_t* image) {
    // JpegDecodeData *z, int32_t *out_x, int32_t *out_y, int32_t *comp, int32_t req_comp) {
    uint32_t n, decode_n, is_rgb;
    // jpeg_->components = 0; // make stbi__cleanup_jpeg safe

    // determine actual number of components to generate
    n = jpeg_->components >= 3 ? 3 : 1;  // n: target components, jpeg_->components: encoded components

    is_rgb = jpeg_->components == 3 && (jpeg_->rgb == 3 ||
                (jpeg_->app14_color_transform == 0 && !jpeg_->jfif));

    if (jpeg_->components == 3 && n < 3 && !is_rgb) {
        decode_n = 1;
    }
    else {
        decode_n = jpeg_->components;
    // std::cout << std::dec << "decode_n: " << decode_n << std::endl;
    }

    // resample and color-convert
    uint32_t k, i, j;
    // uint8_t *output;
    uint8_t *coutput[4] = { NULL, NULL, NULL, NULL };
    SampleData res_comp[4];

    for (k = 0; k < decode_n; ++k) {
        SampleData *r = &res_comp[k];

        // allocate line buffer big enough for upsampling off the edges
        // with upsample factor of 4
        jpeg_->img_comp[k].line_buffer = (uint8_t *) malloc(width_ + 3);
        if (!jpeg_->img_comp[k].line_buffer) {
            freeComponents(jpeg_, jpeg_->components);
            LOG(ERROR) << "Out of memory.";
            return false;
        }

        r->hs      = jpeg_->img_h_max / jpeg_->img_comp[k].hsampling;
        r->vs      = jpeg_->img_v_max / jpeg_->img_comp[k].vsampling;
        r->ystep   = r->vs >> 1;
        r->w_lores = (width_ + r->hs-1) / r->hs;
        r->ypos    = 0;
        r->line0   = r->line1 = jpeg_->img_comp[k].data;

        if (r->hs == 1 && r->vs == 1) {
            r->resample = resampleRow1;
        }
        else if (r->hs == 1 && r->vs == 2) {
            r->resample = resampleRowV2;
        }
        else if (r->hs == 2 && r->vs == 1) {
            r->resample = resampleRowH2;
        }
        else if (r->hs == 2 && r->vs == 2) {
            r->resample = jpeg_->resampleRowHV2Kernel;
        }
        else {
            r->resample = resampleRowGeneric;
        }
    }

    if (n == 3 && jpeg_->components == 3 && !is_rgb) {
        ycrcb2bgr_ = new YCrCb2BGR_i(width_, channels_);
    }
    // now go ahead and resample
    for (j = 0; j < height_; ++j) {
        // uint8_t *out = output + n * width_ * j;  // stride
        uint8_t *out = image + stride * j;  // stride
        for (k = 0; k < decode_n; ++k) {
            SampleData *r = &res_comp[k];   // optimize
            int32_t y_bot = r->ystep >= (r->vs >> 1);
            coutput[k] = r->resample(jpeg_->img_comp[k].line_buffer,
                                     y_bot ? r->line1 : r->line0,
                                     y_bot ? r->line0 : r->line1,
                                     r->w_lores, r->hs);
            if (++r->ystep >= r->vs) {
                r->ystep = 0;
                r->line0 = r->line1;
                if (++r->ypos < jpeg_->img_comp[k].y) {
                    r->line1 += jpeg_->img_comp[k].w2;
                }
            }
        }
        if (n == 3) {
            uint8_t *y = coutput[0];
            if (jpeg_->components == 3) {
                if (is_rgb) {  // input is rgb
                    for (i = 0; i < width_; ++i) {
                        out[0] = y[i];
                        out[1] = coutput[1][i];
                        out[2] = coutput[2][i];
                        // out[3] = 255;
                        out += n;
                    }
                } else {  // input is YCrCb
                    // std::cout << "come in YCbCr2BGRKernel." << std::endl;
                    // jpeg_->YCbCr2BGRKernel(out, y, coutput[1], coutput[2],
                    //                        width_, n);
                    ycrcb2bgr_->convertBGR(y, coutput[1], coutput[2], out);
                }
            } else if (jpeg_->components == 4) {
                if (jpeg_->app14_color_transform == 0) {  // CMYK
                    for (i=0; i < width_; ++i) {
                        uint8_t m = coutput[3][i];
                        out[0] = blinn8x8(coutput[0][i], m);
                        out[1] = blinn8x8(coutput[1][i], m);
                        out[2] = blinn8x8(coutput[2][i], m);
                        // out[3] = 255;
                        out += n;
                    }
                } else if (jpeg_->app14_color_transform == 2) { // YCCK
                    jpeg_->YCbCr2BGRKernel(out, y, coutput[1], coutput[2],
                                           width_, n);
                    for (i = 0; i < width_; ++i) {
                        uint8_t m = coutput[3][i];
                        out[0] = blinn8x8(255 - out[0], m);
                        out[1] = blinn8x8(255 - out[1], m);
                        out[2] = blinn8x8(255 - out[2], m);
                        out += n;
                    }
                } else { // YCbCr + alpha?  Ignore the fourth channel for now
                    jpeg_->YCbCr2BGRKernel(out, y, coutput[1], coutput[2],
                                           width_, n);
                }
            } else {
                for (i = 0; i < width_; ++i) {
                    out[0] = out[1] = out[2] = y[i];
                    // out[3] = 255; // not used if n==3
                    out += n;
                }
            }
        } else {  // n == 1
            if (is_rgb) {
                if (n == 1) {
                    for (i=0; i < width_; ++i) {
                        *out++ = computeY(coutput[0][i], coutput[1][i],
                                          coutput[2][i]);
                    }
                }
                else {
                    for (i = 0; i < width_; ++i, out += 2) {
                        out[0] = computeY(coutput[0][i], coutput[1][i],
                                          coutput[2][i]);
                        out[1] = 255;
                    }
                }
            } else if (jpeg_->components == 4 &&
                       jpeg_->app14_color_transform == 0) {
                for (i = 0; i < width_; ++i) {
                    uint8_t m = coutput[3][i];
                    uint8_t r = blinn8x8(coutput[0][i], m);
                    uint8_t g = blinn8x8(coutput[1][i], m);
                    uint8_t b = blinn8x8(coutput[2][i], m);
                    out[0] = computeY(r, g, b);
                    // out[1] = 255;
                    out += n;
                }
            } else if (jpeg_->components == 4 &&
                       jpeg_->app14_color_transform == 2) {
                for (i = 0; i < width_; ++i) {
                    out[0] = blinn8x8(255 - coutput[0][i], coutput[3][i]);
                    // out[1] = 255;
                    out += n;
                }
            } else {
                // std::cout << "come in 1 channel." << std::endl;
                uint8_t *y = coutput[0];
                if (n == 1) {
                    for (i = 0; i < width_; ++i) {
                        out[i] = y[i];
                    }
                }
                else {
                    for (i = 0; i < width_; ++i) {
                        *out++ = y[i];
                        *out++ = 255;
                    }
                }
            }
        }
    }
    freeComponents(jpeg_, jpeg_->components);
    if (n == 3 && jpeg_->components == 3 && !is_rgb) {
        delete ycrcb2bgr_;
    }
    return true;
}

void JpegDecoder::dequantizeData(int16_t *data, uint16_t *dequant_table) {
   for (int32_t i = 0; i < 64; ++i) {
      data[i] *= dequant_table[i];
   }
}

void JpegDecoder::finishProgressiveJpeg(JpegDecodeData *jpeg) {
    if (jpeg->progressive) {
        // dequantize and idct the data
        int32_t i, j, n;
        for (n = 0; n < jpeg->components; ++n) {
            int32_t width = (jpeg->img_comp[n].x + 7) >> 3;
            int32_t height = (jpeg->img_comp[n].y + 7) >> 3;
            for (j = 0; j < height; ++j) {
                for (i = 0; i < width; ++i) {
                    int16_t *data = jpeg->img_comp[n].coeff + 64 *
                                    (i + j * jpeg->img_comp[n].coeff_w);
                    dequantizeData(data,
                                   jpeg->dequant[jpeg->img_comp[n].quant_id]);
                    jpeg->idctBlockKernel(jpeg->img_comp[n].data +
                            jpeg->img_comp[n].w2 * j * 8 + i * 8,
                            jpeg->img_comp[n].w2, data);
                }
            }
        }
    }
}

bool JpegDecoder::readHeader() {
    // struct timeval start, end;
    // gettimeofday(&start, NULL);

    for (int32_t i = 0; i < 4; i++) {
        jpeg_->img_comp[i].raw_data  = nullptr;
        jpeg_->img_comp[i].raw_coeff = nullptr;
    }
    jpeg_->restart_interval = 0;
    jpeg_->jfif = 0;
    // valid values are 0(Unknown, 3->RGB, 4->CMYK), 1(YCbCr), 2(YCCK)
    jpeg_->app14_color_transform = -1;
    jpeg_->progressive = 0;

    bool succeeded;
    file_data_->skip(2);
    jpeg_->marker = NULL_MARKER;
    uint8_t marker = getMarker(jpeg_);
    while (marker != 0xDA && marker != 0xD9) {  // Start of scan or end of image
        succeeded = processSegments(jpeg_, marker);
        if (!succeeded) {
            freeComponents(jpeg_, jpeg_->components);
            return false;
        }

        marker = getMarker(jpeg_);
        if (marker == 0xDA || marker == 0xD9) {
            jpeg_->marker = marker;
        }
    }

    if (marker == 0xD9) {
        LOG(ERROR) << "No image data is datected.";
        return false;
    }

    // gettimeofday(&end, NULL);
    // int time = (end.tv_sec * 1000000 + end.tv_usec) -
    //             (start.tv_sec * 1000000 + start.tv_usec);
    // std::cout << "readHeader time: " << time << " us." << std::endl;

    return true;
}

bool JpegDecoder::decodeData(uint32_t stride, uint8_t* image) {
    // struct timeval start, end;
    // struct timeval start1, end1;
    // struct timeval start2, end2;
    // gettimeofday(&start, NULL);
    // ProfilerStart("jpeg_perf.prof");  // profiling
    // shortcode = 0;
    // longcode = 0;
    // fastcode = 0;
    // std::cout << "come in decodeData()." << std::endl;

    setJpegFunctions(jpeg_);

    bool succeeded;
    uint8_t marker = getMarker(jpeg_);
    // std::cout << "in decodeData(), getmarker0." << std::endl;
    while (marker != 0xD9) {  // end of image
        if (marker == 0xDA) {  // start of scan
            succeeded = parseSOS(jpeg_);
            if (!succeeded) {
                freeComponents(jpeg_, jpeg_->components);
                return false;
            }

            // gettimeofday(&start1, NULL);
            succeeded = parseEntropyCodedData(jpeg_);
            // std::cout << "after parseEntropyCodedData." << std::endl;
            if (!succeeded) {
                freeComponents(jpeg_, jpeg_->components);
                LOG(ERROR) << "Failed to decode the compressed data.";
                return false;
            }
            // gettimeofday(&end1, NULL);
            // int time = (end1.tv_sec * 1000000 + end1.tv_usec) -
            //             (start1.tv_sec * 1000000 + start1.tv_usec);
            // std::cout << "parseEntropyCodedData time: " << time << " us." << std::endl;
        }
        else {
            succeeded = processSegments(jpeg_, marker);
            if (!succeeded) {
                freeComponents(jpeg_, jpeg_->components);
                return false;
            }
        }

    // std::cout << "jpeg->code_bits: " << std::dec << jpeg_->code_bits << std::endl;
    // std::cout << "jpeg->code_buffer: " << std::hex << jpeg_->code_buffer << std::endl;
        // std::cout << "in decodeData(), berfore getmarker1." << std::endl;
        // std::cout << "in decodeData(), marker: " << std::hex << (int)marker << std::endl;
        // uint32_t previous0 = file_data_->getpreviousByte();
        // std::cout << "in decodeData(), previous0: " << std::hex << previous0 << std::endl;
        marker = getMarker(jpeg_);
        // uint32_t previous1 = file_data_->getpreviousByte();
        // std::cout << "in decodeData(), previous1: " << std::hex << previous1 << std::endl;
        // std::cout << "in decodeData(), marker: " << std::hex << (int)marker << std::endl;
        // std::cout << "in decodeData(), byte after marker: " << (int)file_data_->getByte() << std::endl;
        // std::cout << "in decodeData(), getmarker1." << std::endl;
    }

    if (jpeg_->progressive) {
        finishProgressiveJpeg(jpeg_);
    }

    // gettimeofday(&start2, NULL);
    succeeded = sampleConvertColor(stride, image);
    // gettimeofday(&end2, NULL);
    // int time2 = (end2.tv_sec * 1000000 + end2.tv_usec) -
    //             (start2.tv_sec * 1000000 + start2.tv_usec);
    // std::cout << "sampleConvertColor time: " << time2 << " us." << std::endl;
    freeComponents(jpeg_, jpeg_->components);
    if (!succeeded) {
        LOG(ERROR) << "Failed to sample and convert YCrCb data to the target"
                   << " color format.";
        return false;
    }

    // std::cout << "growbuffer_count: " << growbuffer_count << ", huffman_count: " << huffman_count << std::endl;
    // std::cout << "sizeof(size_t): " << sizeof(size_t) << ", sizeof(long long)" << sizeof(long long) << std::endl;
    // ProfilerStop();  // profiling
    // gettimeofday(&end, NULL);
    // int time = (end.tv_sec * 1000000 + end.tv_usec) -
    //             (start.tv_sec * 1000000 + start.tv_usec);
    // std::cout << "decodeData time: " << time << " us." << std::endl;
    // // std::cout << "shortcode: " << shortcode << std::endl;
    // // std::cout << "longcode: " << longcode << std::endl;
    // std::cout << "fastcode: " << fastcode << std::endl;

    return true;
}

} //! namespace x86
} //! namespace cv
} //! namespace ppl