#define _POSIX_C_SOURCE 200809L

#include <float.h>
#include <immintrin.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdalign.h>
#include <string.h>
#include <time.h>

/*
 * C port of simdtest's LuaJIT SIMD microbenchmark.
 *
 * Compares scalar code with explicit 128-bit XMM and 256-bit AVX2/YMM code.
 * The scalar functions are compiled with automatic vectorization disabled so
 * they remain meaningful scalar baselines.
 *
 * Build (GCC or Clang, x86-64):
 *   cc -O2 -std=c11 -Wall -Wextra -Wpedantic test/simd/bench.c \
 *      -lm -o simdtest
 *
 * Run:
 *   ./simdtest [reps]
 */

#if !defined(__x86_64__) && !defined(_M_X64)
# error "simdtest.c requires x86-64"
#endif

#if !defined(__GNUC__) && !defined(__clang__)
# error "This implementation currently requires GCC or Clang target attributes"
#endif

#define NOINLINE       __attribute__((noinline))
#define TARGET_SSE41   __attribute__((target("sse4.1")))
#define TARGET_AVX2    __attribute__((target("avx2")))

#if defined(__GNUC__) && !defined(__clang__)
# define SCALAR_FN __attribute__((noinline, optimize("no-tree-vectorize", "no-tree-slp-vectorize")))
#else
# define SCALAR_FN __attribute__((noinline))
#endif

#if defined(__clang__)
# define CLANG_NO_VECTORIZE \
    _Pragma("clang loop vectorize(disable)") \
    _Pragma("clang loop interleave(disable)")
#else
# define CLANG_NO_VECTORIZE
#endif

#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

#define N               (1u << 16)
#define PASSES          200
#define FIR_N           (1u << 15)
#define FIR_PASSES      60
#define POLY_N          (1u << 15)
#define POLY_PASSES     60
#define MANDEL_N        (1u << 14)
#define MANDEL_PASSES   4
#define MANDEL_ITERS    64
#define DELTA_N         (1u << 18)
#define DELTA_PASSES    80

#define RGBA_PIXELS     (1920u * 1080u)
#define RGBA_N          (RGBA_PIXELS * 4u)
#define RGBA_PASSES     3
#define CHECKSUM_N      (16u * 1024u * 1024u)
#define CHECKSUM_BLOCKS (CHECKSUM_N / 32u)
#define CHECKSUM_PASSES 3
#define SAD_N           (16u * 1024u * 1024u)
#define SAD_BLOCKS      (SAD_N / 32u)
#define SAD_PASSES      3
#define LUMA_PIXELS     (3840u * 2160u)
#define LUMA_TILES      (LUMA_PIXELS / 16u)
#define LUMA_PASSES     4
#define DEPTH_PIXELS    (3840u * 2160u)
#define DEPTH_TILES     (DEPTH_PIXELS / 16u)
#define DEPTH_PASSES    3
#define PCM_SECONDS     60u
#define PCM_SAMPLES     (48000u * PCM_SECONDS)
#define PCM_BLOCKS      (PCM_SAMPLES / 16u)
#define PCM_PASSES      12
#define GATE_SECONDS    60u
#define GATE_SAMPLES    (48000u * GATE_SECONDS)
#define GATE_PASSES     8
#define PCM_FIR_SAMPLES (8u * 1024u * 1024u)
#define PCM_FIR_BLOCKS  (PCM_FIR_SAMPLES / 16u)
#define PCM_FIR_PASSES  4
#define RESID_W         3840u
#define RESID_H         2160u
#define RESID_N         (RESID_W * RESID_H)
#define RESID_PASSES    4
#define ACT_N           (16u * 1024u * 1024u)
#define ACT_BLOCKS      (ACT_N / 32u)
#define ACT_PASSES      3
#define TERNARY_N       (16u * 1024u * 1024u)
#define TERNARY_BLOCKS  (TERNARY_N / 32u)
#define TERNARY_PASSES  4
#define BLUR_W          1920u
#define BLUR_H          1080u
#define BLUR_STRIDE     (BLUR_W + 4u)
#define BLUR_ROWS       (BLUR_H + 4u)
#define BLUR_N          (BLUR_W * BLUR_H)
#define BLUR_PASSES     2
#define MORPH_W         3840u
#define MORPH_H         2160u
#define MORPH_STRIDE    (MORPH_W + 2u)
#define MORPH_N         (MORPH_W * MORPH_H)
#define MORPH_PASSES    2
#define XFORM_N         (1u << 19)
#define XFORM_PASSES    12
#define VOXEL_N         (1u << 20)
#define VOXEL_PASSES    8
#define AUDIO_N         (1u << 18)
#define AUDIO_TAPS      64
#define AUDIO_PASSES    2
#define PARTICLE_N      (1u << 17)
#define PARTICLE_STEPS  32
#define CHACHA_BLOCKS   (1u << 13)
#define CHACHA_PASSES   16

alignas(64) static float xs[N];
alignas(64) static float ys_scalar[N];
alignas(64) static float ys_xmm[N];
alignas(64) static float ys_ymm[N];

alignas(64) static int32_t is[N];

alignas(64) static float fir_in[FIR_N + 7];
alignas(64) static float fir_scalar_out[FIR_N];
alignas(64) static float fir_xmm_out[FIR_N];
alignas(64) static float fir_ymm_out[FIR_N];

alignas(64) static double poly_in[POLY_N];
alignas(64) static double poly_scalar_out[POLY_N];
alignas(64) static double poly_xmm_out[POLY_N];
alignas(64) static double poly_ymm_out[POLY_N];

alignas(64) static double mandel_cx[MANDEL_N];
alignas(64) static double mandel_cy[MANDEL_N];

static int32_t *delta_in, *delta_out_s, *delta_out_x, *delta_out_y;
static int32_t *rgba_a, *rgba_b, *rgba_out;
static uint8_t *checksum_in, *checksum_s, *checksum_x, *checksum_y;
static uint8_t *sad_ref, *sad_cur;
static uint16_t *sad_out_s, *sad_out_x, *sad_out_y;
static uint16_t *luma_in, *luma_out_s, *luma_out_x, *luma_out_y;
static uint16_t *depth_in;
static uint16_t *depth_lo_s, *depth_hi_s, *depth_lo_x, *depth_hi_x;
static uint16_t *depth_lo_y, *depth_hi_y;
static int16_t *pcm_in;
static int16_t *pcm_lo_s, *pcm_hi_s, *pcm_lo_x, *pcm_hi_x;
static int16_t *pcm_lo_y, *pcm_hi_y;
static float *gate_in, *gate_out_s, *gate_out_x, *gate_out_y;
static int16_t *pcm_fir_in, *pcm_fir_out_s, *pcm_fir_out_x, *pcm_fir_out_y;
static int16_t *resid_cur, *resid_pred;
static int16_t *resid_out_s, *resid_out_x, *resid_out_y;
static int8_t *act_in, *act_lo_s, *act_hi_s, *act_lo_x, *act_hi_x;
static int8_t *act_lo_y, *act_hi_y;
static int8_t *ternary_in, *ternary_out_s, *ternary_out_x, *ternary_out_y;
static float *blur_in, *blur_tmp_s, *blur_tmp_x, *blur_tmp_y;
static float *blur_out_s, *blur_out_x, *blur_out_y;
static float *morph_in, *morph_out_s, *morph_out_x, *morph_out_y;
static float *xform_in, *xform_weight;
static float *xform_out_s, *xform_out_x, *xform_out_y;
static double *voxel_x, *voxel_y, *voxel_z;
static float *audio_in, *audio_coeff;
static float *audio_out_s, *audio_out_x, *audio_out_y;
static float *particle_x, *particle_y, *particle_z;
static float *particle_vx, *particle_vy, *particle_vz;
static float *particle_out_s, *particle_out_x, *particle_out_y;

static volatile double result_sink;
static int failures;

typedef double (*kernel_fn)(void);

typedef struct bench_result {
    double seconds;
    double value;
} bench_result;

static double now_seconds(void)
{
    struct timespec ts;
#ifdef CLOCK_PROCESS_CPUTIME_ID
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) {
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1.0e-9;
    }
#endif
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

static bench_result bench(kernel_fn fn, int reps)
{
    bench_result out = { .seconds = DBL_MAX, .value = 0.0 };

    for (int r = 0; r < reps; ++r) {
        const double t0 = now_seconds();
        const double value = fn();
        const double elapsed = now_seconds() - t0;

        result_sink = value;
        out.value = value;
        if (elapsed < out.seconds) {
            out.seconds = elapsed;
        }
    }

    return out;
}

static void print_difference2(double a, double b)
{
    printf("   ! results differ: %.17g vs %.17g\n", a, b);
}

static void print_difference3(double a, double b, double c)
{
    printf("   ! results differ: %.17g vs %.17g vs %.17g\n", a, b, c);
}

static bool same_result(double a, double b, double tolerance)
{
    if (tolerance < 0.0) {
        return true;
    }
    if (tolerance == 0.0) {
        return a == b;
    }
    const double scale = fmax(1.0, fmax(fabs(a), fabs(b)));
    return fabs(a - b) <= tolerance * scale;
}

static void report_xmm(const char *name,
                       bench_result scalar,
                       bench_result xmm,
                       double tolerance)
{
    printf("%-26s scalar %7.1f ms   XMM %7.1f ms   %5.2fx\n",
           name,
           scalar.seconds * 1000.0,
           xmm.seconds * 1000.0,
           scalar.seconds / xmm.seconds);

    if (!same_result(scalar.value, xmm.value, tolerance)) {
        ++failures;
        print_difference2(scalar.value, xmm.value);
    }
}

static void report_ymm(const char *name,
                       bench_result scalar,
                       bench_result xmm,
                       bench_result ymm,
                       double tolerance)
{
    printf("%-26s scalar %7.1f ms   XMM %7.1f ms %5.2fx   "
           "YMM %7.1f ms %5.2fx (%4.2fx/XMM)\n",
           name,
           scalar.seconds * 1000.0,
           xmm.seconds * 1000.0,
           scalar.seconds / xmm.seconds,
           ymm.seconds * 1000.0,
           scalar.seconds / ymm.seconds,
           xmm.seconds / ymm.seconds);

    if (!same_result(scalar.value, xmm.value, tolerance) ||
        !same_result(scalar.value, ymm.value, tolerance)) {
        ++failures;
        print_difference3(scalar.value, xmm.value, ymm.value);
    }
}

static void *xalloc(size_t count, size_t size)
{
    void *ptr = NULL;
    if (count != 0 && size > SIZE_MAX / count) {
        fputs("benchmark allocation size overflow\n", stderr);
        exit(EXIT_FAILURE);
    }
    const size_t bytes = count * size;
    if (posix_memalign(&ptr, 64u, bytes ? bytes : 64u) != 0 || ptr == NULL) {
        fprintf(stderr, "could not allocate %zu benchmark bytes\n", bytes);
        exit(EXIT_FAILURE);
    }
    memset(ptr, 0, bytes);
    return ptr;
}

#define ALLOC_ARRAY(name, count) \
    ((name) = xalloc((count), sizeof *(name)))

#define DEFINE_SAMPLE_SUM(suffix, type) \
static double sample_sum_##suffix(const type *buf, size_t n) \
{ \
    double sum = 0.0; \
    const size_t step = n / 257u > 0u ? n / 257u : 1u; \
    for (size_t i = 0; i < n; i += step) sum += (double)buf[i]; \
    return sum; \
}

DEFINE_SAMPLE_SUM(i8, int8_t)
DEFINE_SAMPLE_SUM(u8, uint8_t)
DEFINE_SAMPLE_SUM(i16, int16_t)
DEFINE_SAMPLE_SUM(u16, uint16_t)
DEFINE_SAMPLE_SUM(i32, int32_t)
DEFINE_SAMPLE_SUM(f32, float)

TARGET_SSE41 static inline int32_t hmax_i32x4(__m128i v)
{
    __m128i t = _mm_max_epi32(
        v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
    t = _mm_max_epi32(
        t, _mm_shuffle_epi32(t, _MM_SHUFFLE(1, 0, 3, 2)));
    return _mm_cvtsi128_si32(t);
}

TARGET_SSE41 static inline float hsum_f32x4(__m128 v)
{
    v = _mm_hadd_ps(v, v);
    v = _mm_hadd_ps(v, v);
    return _mm_cvtss_f32(v);
}

TARGET_SSE41 static inline int32_t hsum_i32x4(__m128i v)
{
    v = _mm_hadd_epi32(v, v);
    v = _mm_hadd_epi32(v, v);
    return _mm_cvtsi128_si32(v);
}

TARGET_AVX2 static inline int32_t hsum_i32x8(__m256i v)
{
    const __m128i s = _mm_add_epi32(
        _mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
    return hsum_i32x4(s);
}

TARGET_SSE41 static inline uint32_t hsum_u8x16(__m128i v)
{
    const __m128i s = _mm_sad_epu8(v, _mm_setzero_si128());
    return (uint32_t)_mm_cvtsi128_si64(s) +
           (uint32_t)_mm_extract_epi64(s, 1);
}

TARGET_AVX2 static inline uint32_t hsum_u8x32(__m256i v)
{
    const __m256i s = _mm256_sad_epu8(v, _mm256_setzero_si256());
    alignas(32) uint64_t lanes[4];
    _mm256_store_si256((__m256i *)lanes, s);
    return (uint32_t)(lanes[0] + lanes[1] + lanes[2] + lanes[3]);
}

TARGET_SSE41 static inline uint32_t hsum_u16x8(__m128i v)
{
    return (uint32_t)hsum_i32x4(_mm_madd_epi16(v, _mm_set1_epi16(1)));
}

TARGET_AVX2 static inline uint32_t hsum_u16x16(__m256i v)
{
    return (uint32_t)hsum_i32x8(
        _mm256_madd_epi16(v, _mm256_set1_epi16(1)));
}

TARGET_SSE41 static inline uint16_t hmin_u16x8(__m128i v)
{
    return (uint16_t)_mm_cvtsi128_si32(_mm_minpos_epu16(v));
}

TARGET_SSE41 static inline uint16_t hmax_u16x8(__m128i v)
{
    const __m128i inv = _mm_xor_si128(v, _mm_set1_epi16((short)-1));
    return (uint16_t)~(uint16_t)_mm_cvtsi128_si32(_mm_minpos_epu16(inv));
}

TARGET_AVX2 static inline uint16_t hmin_u16x16(__m256i v)
{
    return hmin_u16x8(_mm_min_epu16(
        _mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1)));
}

TARGET_AVX2 static inline uint16_t hmax_u16x16(__m256i v)
{
    return hmax_u16x8(_mm_max_epu16(
        _mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1)));
}

TARGET_SSE41 static inline __m128i swap_i16_pairs(__m128i v)
{
    v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
    return _mm_shufflehi_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
}

TARGET_SSE41 static inline int16_t hmin_i16x8(__m128i v)
{
    v = _mm_min_epi16(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
    v = _mm_min_epi16(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
    v = _mm_min_epi16(v, swap_i16_pairs(v));
    return (int16_t)_mm_cvtsi128_si32(v);
}

TARGET_SSE41 static inline int16_t hmax_i16x8(__m128i v)
{
    v = _mm_max_epi16(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
    v = _mm_max_epi16(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
    v = _mm_max_epi16(v, swap_i16_pairs(v));
    return (int16_t)_mm_cvtsi128_si32(v);
}

TARGET_AVX2 static inline int16_t hmin_i16x16(__m256i v)
{
    return hmin_i16x8(_mm_min_epi16(
        _mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1)));
}

TARGET_AVX2 static inline int16_t hmax_i16x16(__m256i v)
{
    return hmax_i16x8(_mm_max_epi16(
        _mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1)));
}

TARGET_SSE41 static inline __m128i swap_i8_pairs(__m128i v)
{
    const __m128i control = _mm_setr_epi8(
        1,0,3,2,5,4,7,6,9,8,11,10,13,12,15,14);
    return _mm_shuffle_epi8(v, control);
}

TARGET_SSE41 static inline int8_t hmin_i8x16(__m128i v)
{
    v = _mm_min_epi8(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
    v = _mm_min_epi8(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
    v = _mm_min_epi8(v, swap_i16_pairs(v));
    v = _mm_min_epi8(v, swap_i8_pairs(v));
    return (int8_t)_mm_cvtsi128_si32(v);
}

TARGET_SSE41 static inline int8_t hmax_i8x16(__m128i v)
{
    v = _mm_max_epi8(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
    v = _mm_max_epi8(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
    v = _mm_max_epi8(v, swap_i16_pairs(v));
    v = _mm_max_epi8(v, swap_i8_pairs(v));
    return (int8_t)_mm_cvtsi128_si32(v);
}

TARGET_AVX2 static inline int8_t hmin_i8x32(__m256i v)
{
    return hmin_i8x16(_mm_min_epi8(
        _mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1)));
}

TARGET_AVX2 static inline int8_t hmax_i8x32(__m256i v)
{
    return hmax_i8x16(_mm_max_epi8(
        _mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1)));
}

TARGET_SSE41 static inline int32_t dot_i16x8(__m128i a, __m128i b)
{
    return hsum_i32x4(_mm_madd_epi16(a, b));
}

TARGET_AVX2 static inline int32_t dot_i16x16(__m256i a, __m256i b)
{
    return hsum_i32x8(_mm256_madd_epi16(a, b));
}

TARGET_SSE41 static inline int32_t dot_i8x16(__m128i a, __m128i b)
{
    const __m128i al = _mm_cvtepi8_epi16(a);
    const __m128i bl = _mm_cvtepi8_epi16(b);
    const __m128i ah = _mm_cvtepi8_epi16(_mm_srli_si128(a, 8));
    const __m128i bh = _mm_cvtepi8_epi16(_mm_srli_si128(b, 8));
    return hsum_i32x4(_mm_add_epi32(
        _mm_madd_epi16(al, bl), _mm_madd_epi16(ah, bh)));
}

TARGET_AVX2 static inline int32_t dot_i8x32(__m256i a, __m256i b)
{
    return dot_i8x16(_mm256_castsi256_si128(a),
                     _mm256_castsi256_si128(b)) +
           dot_i8x16(_mm256_extracti128_si256(a, 1),
                     _mm256_extracti128_si256(b, 1));
}

static void initialize_data(bool has_ymm)
{
    for (size_t i = 0; i < N; ++i) {
        xs[i] = (float)(i % 17u);
        ys_scalar[i] = (float)(i % 13u);
        ys_xmm[i] = ys_scalar[i];
        ys_ymm[i] = ys_scalar[i];

        const uint64_t mixed = (uint64_t)i * UINT64_C(2654435761);
        is[i] = (int32_t)(mixed % UINT64_C(1000003)) - INT32_C(500000);
    }

    for (size_t i = 0; i < FIR_N + 7u; ++i) {
        const int value = (int)((i * 37u) % 1024u) - 512;
        fir_in[i] = (float)value / 256.0f;
    }

    for (size_t i = 0; i < POLY_N; ++i) {
        poly_in[i] = (double)(i % 4096u) / 2048.0 - 1.0;
    }

    for (size_t i = 0; i < MANDEL_N; ++i) {
        const size_t ix = i % 256u;
        const size_t iy = i / 256u;
        mandel_cx[i] = -2.0 + 3.0 * ((double)ix + 0.5) / 256.0;
        mandel_cy[i] = -1.0 + 2.0 * ((double)iy + 0.5) / 64.0;
    }

    ALLOC_ARRAY(delta_in, DELTA_N + 8u);
    ALLOC_ARRAY(delta_out_s, DELTA_N);
    ALLOC_ARRAY(delta_out_x, DELTA_N);
    if (has_ymm) ALLOC_ARRAY(delta_out_y, DELTA_N);
    for (size_t i = 0; i < DELTA_N + 8u; ++i) {
        delta_in[i] = (int32_t)(((uint64_t)i * UINT64_C(1103515245) +
                                 UINT64_C(12345)) % UINT64_C(1000003));
    }

    ALLOC_ARRAY(rgba_a, RGBA_N);
    ALLOC_ARRAY(rgba_b, RGBA_N);
    ALLOC_ARRAY(rgba_out, RGBA_N);
    for (size_t i = 0; i < RGBA_N; ++i) {
        rgba_a[i] = (int32_t)((i * 17u + 3u) % 65521u);
        rgba_b[i] = (int32_t)((i * 29u + 11u) % 65521u);
    }

    ALLOC_ARRAY(checksum_in, CHECKSUM_N);
    ALLOC_ARRAY(checksum_s, CHECKSUM_BLOCKS);
    ALLOC_ARRAY(checksum_x, CHECKSUM_BLOCKS);
    if (has_ymm) ALLOC_ARRAY(checksum_y, CHECKSUM_BLOCKS);
    for (size_t i = 0; i < CHECKSUM_N; ++i) {
        checksum_in[i] = (uint8_t)(i * 29u + i % 251u + 17u);
    }

    ALLOC_ARRAY(sad_ref, SAD_N);
    ALLOC_ARRAY(sad_cur, SAD_N);
    ALLOC_ARRAY(sad_out_s, SAD_BLOCKS);
    ALLOC_ARRAY(sad_out_x, SAD_BLOCKS);
    if (has_ymm) ALLOC_ARRAY(sad_out_y, SAD_BLOCKS);
    for (size_t i = 0; i < SAD_N; ++i) {
        const int base = (int)((i * 29u + (i / 32u) * 3u) % 240u) + 8;
        const int delta = (int)((i * 13u + (i / 32u) * 5u) % 15u) - 7;
        sad_ref[i] = (uint8_t)base;
        sad_cur[i] = (uint8_t)(base + delta);
    }

    ALLOC_ARRAY(luma_in, LUMA_PIXELS);
    ALLOC_ARRAY(luma_out_s, LUMA_TILES);
    ALLOC_ARRAY(luma_out_x, LUMA_TILES);
    if (has_ymm) ALLOC_ARRAY(luma_out_y, LUMA_TILES);
    for (size_t i = 0; i < LUMA_PIXELS; ++i) {
        const size_t row = i / 3840u;
        luma_in[i] = (uint16_t)((i * 37u + row * 13u +
                                (i % 16u) * 17u) % 4096u);
    }

    static const uint16_t depth_pattern[16] = {
        9000, 1200, 14800, 5100, 300, 11700, 7300, 2600,
        13200, 6400, 10100, 50, 8200, 3900, 12500, 1800,
    };
    ALLOC_ARRAY(depth_in, DEPTH_PIXELS);
    ALLOC_ARRAY(depth_lo_s, DEPTH_TILES);
    ALLOC_ARRAY(depth_hi_s, DEPTH_TILES);
    ALLOC_ARRAY(depth_lo_x, DEPTH_TILES);
    ALLOC_ARRAY(depth_hi_x, DEPTH_TILES);
    if (has_ymm) {
        ALLOC_ARRAY(depth_lo_y, DEPTH_TILES);
        ALLOC_ARRAY(depth_hi_y, DEPTH_TILES);
    }
    for (size_t i = 0; i < DEPTH_PIXELS; ++i) {
        depth_in[i] = (uint16_t)(((i / 16u) * 37u) % 50000u +
                                 depth_pattern[i % 16u]);
    }

    static const int16_t pcm_pattern[16] = {
        17000, -13000, 9000, -19000, 3000, -7000, 15000, -11000,
        5000, -17000, 19000, -3000, 11000, -15000, 7000, -9000,
    };
    ALLOC_ARRAY(pcm_in, PCM_SAMPLES);
    ALLOC_ARRAY(pcm_lo_s, PCM_BLOCKS);
    ALLOC_ARRAY(pcm_hi_s, PCM_BLOCKS);
    ALLOC_ARRAY(pcm_lo_x, PCM_BLOCKS);
    ALLOC_ARRAY(pcm_hi_x, PCM_BLOCKS);
    if (has_ymm) {
        ALLOC_ARRAY(pcm_lo_y, PCM_BLOCKS);
        ALLOC_ARRAY(pcm_hi_y, PCM_BLOCKS);
    }
    for (size_t i = 0; i < PCM_SAMPLES; ++i) {
        pcm_in[i] = (int16_t)(
            (int)(((i / 16u) * 37u) % 20000u) - 10000 +
            pcm_pattern[i % 16u]);
    }

    static const double gate_pattern[32] = {
        -0.91,-0.07, 0.03, 0.44, 0.11,-0.38, 0.72,-0.15,
         0.02, 0.63,-0.22, 0.09,-0.81, 0.17, 0.35,-0.04,
         0.56,-0.13,-0.29, 0.06, 0.95,-0.19, 0.14,-0.48,
        -0.01, 0.27,-0.69, 0.16,-0.33, 0.08, 0.77,-0.12,
    };
    ALLOC_ARRAY(gate_in, GATE_SAMPLES);
    ALLOC_ARRAY(gate_out_s, GATE_SAMPLES);
    ALLOC_ARRAY(gate_out_x, GATE_SAMPLES);
    if (has_ymm) ALLOC_ARRAY(gate_out_y, GATE_SAMPLES);
    for (size_t i = 0; i < GATE_SAMPLES; ++i) {
        gate_in[i] = (float)(gate_pattern[i % 32u] *
            (0.75 + (double)((i / 32u) % 17u) / 64.0));
    }

    ALLOC_ARRAY(pcm_fir_in, PCM_FIR_SAMPLES);
    ALLOC_ARRAY(pcm_fir_out_s, PCM_FIR_BLOCKS);
    ALLOC_ARRAY(pcm_fir_out_x, PCM_FIR_BLOCKS);
    if (has_ymm) ALLOC_ARRAY(pcm_fir_out_y, PCM_FIR_BLOCKS);
    for (size_t i = 0; i < PCM_FIR_SAMPLES; ++i) {
        pcm_fir_in[i] = (int16_t)(
            (int)((i * 29u + (i / 16u) * 7u) % 127u) - 63);
    }

    ALLOC_ARRAY(resid_cur, RESID_N);
    ALLOC_ARRAY(resid_pred, RESID_N);
    ALLOC_ARRAY(resid_out_s, RESID_N);
    ALLOC_ARRAY(resid_out_x, RESID_N);
    if (has_ymm) ALLOC_ARRAY(resid_out_y, RESID_N);
    for (size_t i = 0; i < RESID_N; ++i) {
        const size_t row = i / RESID_W;
        resid_cur[i] = (int16_t)((i * 37u + row * 13u + 17u) % 4096u);
        resid_pred[i] = (int16_t)((i * 19u + row * 29u + 101u) % 4096u);
    }

    static const int8_t act_pattern[32] = {
        71,-63,45,-77,29,-51,80,-35,17,-69,57,-23,39,-55,67,-41,
        53,-75,31,-47,73,-19,43,-65,13,-57,61,-27,35,-71,49,-33,
    };
    ALLOC_ARRAY(act_in, ACT_N);
    ALLOC_ARRAY(act_lo_s, ACT_BLOCKS);
    ALLOC_ARRAY(act_hi_s, ACT_BLOCKS);
    ALLOC_ARRAY(act_lo_x, ACT_BLOCKS);
    ALLOC_ARRAY(act_hi_x, ACT_BLOCKS);
    if (has_ymm) {
        ALLOC_ARRAY(act_lo_y, ACT_BLOCKS);
        ALLOC_ARRAY(act_hi_y, ACT_BLOCKS);
    }
    for (size_t i = 0; i < ACT_N; ++i) {
        act_in[i] = (int8_t)(
            (int)(((i / 32u) * 17u) % 80u) - 40 +
            act_pattern[i % 32u]);
    }

    ALLOC_ARRAY(ternary_in, TERNARY_N);
    ALLOC_ARRAY(ternary_out_s, TERNARY_BLOCKS);
    ALLOC_ARRAY(ternary_out_x, TERNARY_BLOCKS);
    if (has_ymm) ALLOC_ARRAY(ternary_out_y, TERNARY_BLOCKS);
    for (size_t i = 0; i < TERNARY_N; ++i) {
        ternary_in[i] = (int8_t)(
            (int)((i * 17u + (i / 32u) * 7u) % 3u) - 1);
    }

    ALLOC_ARRAY(blur_in, BLUR_STRIDE * BLUR_ROWS);
    ALLOC_ARRAY(blur_tmp_s, BLUR_W * BLUR_ROWS);
    ALLOC_ARRAY(blur_tmp_x, BLUR_W * BLUR_ROWS);
    ALLOC_ARRAY(blur_out_s, BLUR_N);
    ALLOC_ARRAY(blur_out_x, BLUR_N);
    if (has_ymm) {
        ALLOC_ARRAY(blur_tmp_y, BLUR_W * BLUR_ROWS);
        ALLOC_ARRAY(blur_out_y, BLUR_N);
    }
    for (size_t y = 0; y < BLUR_ROWS; ++y) {
        for (size_t x = 0; x < BLUR_STRIDE; ++x) {
            blur_in[y * BLUR_STRIDE + x] =
                (float)((x * 13u + y * 29u + (x * y) % 31u) % 1024u) /
                1024.0f;
        }
    }

    ALLOC_ARRAY(morph_in, MORPH_STRIDE * (MORPH_H + 2u));
    ALLOC_ARRAY(morph_out_s, MORPH_N);
    ALLOC_ARRAY(morph_out_x, MORPH_N);
    if (has_ymm) ALLOC_ARRAY(morph_out_y, MORPH_N);
    for (size_t y = 0; y < MORPH_H + 2u; ++y) {
        for (size_t x = 0; x < MORPH_STRIDE; ++x) {
            morph_in[y * MORPH_STRIDE + x] =
                (float)((x * 73u + y * 151u + (x * y) % 997u) % 65536u) /
                65536.0f;
        }
    }

    ALLOC_ARRAY(xform_in, XFORM_N * 4u);
    ALLOC_ARRAY(xform_weight, XFORM_N);
    ALLOC_ARRAY(xform_out_s, XFORM_N * 4u);
    ALLOC_ARRAY(xform_out_x, XFORM_N * 4u);
    if (has_ymm) ALLOC_ARRAY(xform_out_y, XFORM_N * 4u);
    for (size_t i = 0; i < XFORM_N; ++i) {
        const size_t p = i * 4u;
        xform_in[p] = ((float)((i * 17u) % 1024u) - 512.0f) / 128.0f;
        xform_in[p + 1u] =
            ((float)((i * 29u) % 2048u) - 1024.0f) / 256.0f;
        xform_in[p + 2u] =
            ((float)((i * 43u) % 4096u) - 2048.0f) / 512.0f;
        xform_in[p + 3u] = -999.0f;
        xform_weight[i] = 0.5f + (float)(i % 257u) / 256.0f;
    }

    ALLOC_ARRAY(voxel_x, VOXEL_N);
    ALLOC_ARRAY(voxel_y, VOXEL_N);
    ALLOC_ARRAY(voxel_z, VOXEL_N);
    for (size_t i = 0; i < VOXEL_N; ++i) {
        voxel_x[i] = ((double)(i % 100003u) - 50001.0) * 0.0009765625;
        voxel_y[i] = ((double)(i % 65521u) - 32760.0) * 0.001953125;
        voxel_z[i] = ((double)(i % 32749u) - 16374.0) * 0.00390625;
    }

    ALLOC_ARRAY(audio_in, AUDIO_N + AUDIO_TAPS - 1u);
    ALLOC_ARRAY(audio_coeff, AUDIO_TAPS);
    ALLOC_ARRAY(audio_out_s, AUDIO_N);
    ALLOC_ARRAY(audio_out_x, AUDIO_N);
    if (has_ymm) ALLOC_ARRAY(audio_out_y, AUDIO_N);
    for (size_t i = 0; i < AUDIO_N + AUDIO_TAPS - 1u; ++i) {
        audio_in[i] = (float)(sin((double)i * 0.017) * 0.6 +
                              sin((double)i * 0.071) * 0.25);
    }
    {
        const double pi = acos(-1.0);
        double sum = 0.0;
        for (size_t k = 0; k < AUDIO_TAPS; ++k) {
            const double x = (double)k - (AUDIO_TAPS - 1u) * 0.5;
            const double sinc = sin(0.22 * pi * x) / (pi * x);
            const double window =
                0.54 - 0.46 * cos(2.0 * pi * (double)k /
                                  (double)(AUDIO_TAPS - 1u));
            audio_coeff[k] = (float)(sinc * window);
            sum += (double)audio_coeff[k];
        }
        for (size_t k = 0; k < AUDIO_TAPS; ++k) {
            audio_coeff[k] = (float)((double)audio_coeff[k] / sum);
        }
    }

    ALLOC_ARRAY(particle_x, PARTICLE_N);
    ALLOC_ARRAY(particle_y, PARTICLE_N);
    ALLOC_ARRAY(particle_z, PARTICLE_N);
    ALLOC_ARRAY(particle_vx, PARTICLE_N);
    ALLOC_ARRAY(particle_vy, PARTICLE_N);
    ALLOC_ARRAY(particle_vz, PARTICLE_N);
    ALLOC_ARRAY(particle_out_s, PARTICLE_N * 3u);
    ALLOC_ARRAY(particle_out_x, PARTICLE_N * 3u);
    if (has_ymm) ALLOC_ARRAY(particle_out_y, PARTICLE_N * 3u);
    {
        const double pi = acos(-1.0);
        for (size_t i = 0; i < PARTICLE_N; ++i) {
            const double a = (double)(i % 4096u) * (2.0 * pi / 4096.0);
            const double ring = 1.5 + (double)(i % 97u) / 97.0;
            particle_x[i] = (float)(cos(a) * ring);
            particle_y[i] = (float)(-1.99 + (double)(i % 211u) / 140.0);
            particle_z[i] = (float)(sin(a) * ring);
            particle_vx[i] = (float)(-sin(a) * 0.42);
            particle_vy[i] = (float)(((int)(i % 31u) - 15) * 0.03);
            particle_vz[i] = (float)(cos(a) * 0.42);
        }
    }
}

/* ---------------------------------------------------------------- saxpy */

SCALAR_FN static double saxpy_scalar(void)
{
    const double a = 2.5;
    double s = 0.0;

    for (int pass = 0; pass < PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < N; ++i) {
            ys_scalar[i] = (float)(a * (double)xs[i] + (double)ys_scalar[i]);
        }
        s += (double)ys_scalar[0];
    }

    return s + (double)ys_scalar[1];
}

TARGET_SSE41 NOINLINE static double saxpy_xmm(void)
{
    const __m128 a = _mm_set1_ps(2.5f);
    double s = 0.0;

    for (int pass = 0; pass < PASSES; ++pass) {
        for (size_t i = 0; i < N; i += 4u) {
            const __m128 x = _mm_load_ps(&xs[i]);
            const __m128 y = _mm_load_ps(&ys_xmm[i]);
            _mm_store_ps(&ys_xmm[i], _mm_add_ps(_mm_mul_ps(a, x), y));
        }
        s += (double)ys_xmm[0];
    }

    return s + (double)ys_xmm[1];
}

TARGET_AVX2 NOINLINE static double saxpy_ymm(void)
{
    const __m256 a = _mm256_set1_ps(2.5f);
    double s = 0.0;

    for (int pass = 0; pass < PASSES; ++pass) {
        for (size_t i = 0; i < N; i += 8u) {
            const __m256 x = _mm256_load_ps(&xs[i]);
            const __m256 y = _mm256_load_ps(&ys_ymm[i]);
            _mm256_store_ps(&ys_ymm[i], _mm256_add_ps(_mm256_mul_ps(a, x), y));
        }
        s += (double)ys_ymm[0];
    }

    return s + (double)ys_ymm[1];
}

/* ------------------------------------------------------------------ dot */

SCALAR_FN static double dot_scalar(void)
{
    double s = 0.0;

    for (int pass = 0; pass < PASSES; ++pass) {
        double acc = 0.0;
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < N; ++i) {
            const double v = (double)xs[i];
            acc += v * v;
        }
        s += acc;
    }

    return s;
}

TARGET_SSE41 NOINLINE static double dot_xmm(void)
{
    double s = 0.0;

    for (int pass = 0; pass < PASSES; ++pass) {
        __m128 acc = _mm_setzero_ps();
        for (size_t i = 0; i < N; i += 4u) {
            const __m128 v = _mm_load_ps(&xs[i]);
            acc = _mm_add_ps(acc, _mm_mul_ps(v, v));
        }
        s += (double)hsum_f32x4(acc);
    }

    return s;
}

TARGET_AVX2 NOINLINE static double dot_ymm(void)
{
    double s = 0.0;

    for (int pass = 0; pass < PASSES; ++pass) {
        __m256 acc = _mm256_setzero_ps();
        for (size_t i = 0; i < N; i += 8u) {
            const __m256 v = _mm256_load_ps(&xs[i]);
            acc = _mm256_add_ps(acc, _mm256_mul_ps(v, v));
        }

        alignas(32) float lanes[8];
        _mm256_store_ps(lanes, acc);
        for (int lane = 0; lane < 8; ++lane) {
            s += (double)lanes[lane];
        }
    }

    return s;
}

/* ---------------------------------------------------------- integer max */

SCALAR_FN static double max_scalar(void)
{
    int64_t s = 0;

    for (int pass = 0; pass < PASSES; ++pass) {
        int32_t m = INT32_MIN;
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < N; ++i) {
            if (is[i] > m) {
                m = is[i];
            }
        }
        s += m;
    }

    return (double)s;
}

TARGET_SSE41 NOINLINE static double max_xmm(void)
{
    int64_t s = 0;

    for (int pass = 0; pass < PASSES; ++pass) {
        __m128i m = _mm_set1_epi32(INT32_MIN);
        for (size_t i = 0; i < N; i += 4u) {
            m = _mm_max_epi32(m, _mm_load_si128((const __m128i *)&is[i]));
        }
        s += hmax_i32x4(m);
    }

    return (double)s;
}

TARGET_AVX2 NOINLINE static double max_ymm(void)
{
    int64_t s = 0;

    for (int pass = 0; pass < PASSES; ++pass) {
        __m256i m = _mm256_set1_epi32(INT32_MIN);
        for (size_t i = 0; i < N; i += 8u) {
            m = _mm256_max_epi32(
                m, _mm256_load_si256((const __m256i *)&is[i]));
        }

        const __m128i lo = _mm256_castsi256_si128(m);
        const __m128i hi = _mm256_extracti128_si256(m, 1);
        s += hmax_i32x4(_mm_max_epi32(lo, hi));
    }

    return (double)s;
}

/* ---------------------------------------------------------------- clamp */

SCALAR_FN static double clamp_scalar(void)
{
    double s = 0.0;

    for (int pass = 0; pass < PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < N; ++i) {
            double x = (double)xs[i];
            if (x < 1.0) {
                x = 1.0;
            } else if (x > 9.0) {
                x = 9.0;
            }
            ys_scalar[i] = (float)x;
        }
        s += (double)ys_scalar[0];
    }

    return s;
}

TARGET_SSE41 NOINLINE static double clamp_xmm(void)
{
    const __m128 lo = _mm_set1_ps(1.0f);
    const __m128 hi = _mm_set1_ps(9.0f);
    double s = 0.0;

    for (int pass = 0; pass < PASSES; ++pass) {
        for (size_t i = 0; i < N; i += 4u) {
            const __m128 x = _mm_load_ps(&xs[i]);
            _mm_store_ps(&ys_xmm[i], _mm_min_ps(_mm_max_ps(x, lo), hi));
        }
        s += (double)ys_xmm[0];
    }

    return s;
}

TARGET_AVX2 NOINLINE static double clamp_ymm(void)
{
    const __m256 lo = _mm256_set1_ps(1.0f);
    const __m256 hi = _mm256_set1_ps(9.0f);
    double s = 0.0;

    for (int pass = 0; pass < PASSES; ++pass) {
        for (size_t i = 0; i < N; i += 8u) {
            const __m256 x = _mm256_load_ps(&xs[i]);
            _mm256_store_ps(&ys_ymm[i], _mm256_min_ps(_mm256_max_ps(x, lo), hi));
        }
        s += (double)ys_ymm[0];
    }

    return s;
}

/* -------------------------------------------------------- delta encoding */

SCALAR_FN static double delta_scalar(void)
{
    for (int pass = 0; pass < DELTA_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < DELTA_N; ++i) {
            delta_out_s[i] = delta_in[i + 1u] - delta_in[i];
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i32(delta_out_s, DELTA_N);
}

TARGET_SSE41 NOINLINE static double delta_xmm(void)
{
    for (int pass = 0; pass < DELTA_PASSES; ++pass) {
        __m128i cur = _mm_load_si128((const __m128i *)delta_in);
        for (size_t i = 0; i < DELTA_N; i += 4u) {
            const __m128i next =
                _mm_load_si128((const __m128i *)&delta_in[i + 4u]);
            const __m128i shifted = _mm_alignr_epi8(next, cur, 4);
            _mm_store_si128((__m128i *)&delta_out_x[i],
                            _mm_sub_epi32(shifted, cur));
            cur = next;
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i32(delta_out_x, DELTA_N);
}

TARGET_AVX2 NOINLINE static double delta_ymm(void)
{
    for (int pass = 0; pass < DELTA_PASSES; ++pass) {
        __m256i cur = _mm256_load_si256((const __m256i *)delta_in);
        for (size_t i = 0; i < DELTA_N; i += 8u) {
            const __m256i next =
                _mm256_load_si256((const __m256i *)&delta_in[i + 8u]);
            const __m256i bridge =
                _mm256_permute2x128_si256(cur, next, 0x21);
            const __m256i shifted = _mm256_alignr_epi8(bridge, cur, 4);
            _mm256_store_si256((__m256i *)&delta_out_y[i],
                               _mm256_sub_epi32(shifted, cur));
            cur = next;
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i32(delta_out_y, DELTA_N);
}

/* ----------------------------------------------------- RGBA channel merge */

SCALAR_FN static double rgba_scalar(void)
{
    for (int pass = 0; pass < RGBA_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t p = 0; p < RGBA_PIXELS; ++p) {
            const size_t i = p * 4u;
            rgba_out[i] = rgba_a[i];
            rgba_out[i + 1u] = rgba_b[i + 1u];
            rgba_out[i + 2u] = rgba_a[i + 2u];
            rgba_out[i + 3u] = rgba_b[i + 3u];
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i32(rgba_out, RGBA_N);
}

TARGET_SSE41 NOINLINE static double rgba_xmm(void)
{
    for (int pass = 0; pass < RGBA_PASSES; ++pass) {
        for (size_t i = 0; i < RGBA_N; i += 4u) {
            const __m128i a = _mm_load_si128((const __m128i *)&rgba_a[i]);
            const __m128i b = _mm_load_si128((const __m128i *)&rgba_b[i]);
            _mm_store_si128((__m128i *)&rgba_out[i],
                            _mm_blend_epi16(a, b, 0xcc));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i32(rgba_out, RGBA_N);
}

TARGET_AVX2 NOINLINE static double rgba_ymm(void)
{
    for (int pass = 0; pass < RGBA_PASSES; ++pass) {
        for (size_t i = 0; i < RGBA_N; i += 8u) {
            const __m256i a =
                _mm256_load_si256((const __m256i *)&rgba_a[i]);
            const __m256i b =
                _mm256_load_si256((const __m256i *)&rgba_b[i]);
            _mm256_store_si256((__m256i *)&rgba_out[i],
                               _mm256_blend_epi32(a, b, 0xaa));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i32(rgba_out, RGBA_N);
}

/* ------------------------------------------------------ block checksums */

SCALAR_FN static double checksum_scalar(void)
{
    for (int pass = 0; pass < CHECKSUM_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < CHECKSUM_BLOCKS; ++i) {
            const size_t p = i * 32u;
            uint32_t sum = 0;
            CLANG_NO_VECTORIZE
            for (size_t j = 0; j < 32u; ++j) sum += checksum_in[p + j];
            checksum_s[i] = (uint8_t)sum;
        }
        COMPILER_BARRIER();
    }
    return sample_sum_u8(checksum_s, CHECKSUM_BLOCKS);
}

TARGET_SSE41 NOINLINE static double checksum_xmm(void)
{
    for (int pass = 0; pass < CHECKSUM_PASSES; ++pass) {
        for (size_t i = 0; i < CHECKSUM_BLOCKS; ++i) {
            const size_t p = i * 32u;
            checksum_x[i] = (uint8_t)(
                hsum_u8x16(_mm_load_si128((const __m128i *)&checksum_in[p])) +
                hsum_u8x16(_mm_load_si128(
                    (const __m128i *)&checksum_in[p + 16u])));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_u8(checksum_x, CHECKSUM_BLOCKS);
}

TARGET_AVX2 NOINLINE static double checksum_ymm(void)
{
    for (int pass = 0; pass < CHECKSUM_PASSES; ++pass) {
        for (size_t i = 0; i < CHECKSUM_BLOCKS; ++i) {
            checksum_y[i] = (uint8_t)hsum_u8x32(
                _mm256_load_si256(
                    (const __m256i *)&checksum_in[i * 32u]));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_u8(checksum_y, CHECKSUM_BLOCKS);
}

/* ------------------------------------------------------------- block SAD */

SCALAR_FN static double sad_scalar(void)
{
    for (int pass = 0; pass < SAD_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < SAD_BLOCKS; ++i) {
            const size_t p = i * 32u;
            uint32_t sum = 0;
            CLANG_NO_VECTORIZE
            for (size_t j = 0; j < 32u; ++j) {
                const int d = (int)sad_ref[p + j] - (int)sad_cur[p + j];
                sum += (uint32_t)(d < 0 ? -d : d);
            }
            sad_out_s[i] = (uint16_t)sum;
        }
        COMPILER_BARRIER();
    }
    return sample_sum_u16(sad_out_s, SAD_BLOCKS);
}

TARGET_SSE41 NOINLINE static double sad_xmm(void)
{
    for (int pass = 0; pass < SAD_PASSES; ++pass) {
        for (size_t i = 0; i < SAD_BLOCKS; ++i) {
            const size_t p = i * 32u;
            const __m128i s0 = _mm_sad_epu8(
                _mm_load_si128((const __m128i *)&sad_ref[p]),
                _mm_load_si128((const __m128i *)&sad_cur[p]));
            const __m128i s1 = _mm_sad_epu8(
                _mm_load_si128((const __m128i *)&sad_ref[p + 16u]),
                _mm_load_si128((const __m128i *)&sad_cur[p + 16u]));
            sad_out_x[i] = (uint16_t)(
                (uint64_t)_mm_cvtsi128_si64(s0) +
                (uint64_t)_mm_extract_epi64(s0, 1) +
                (uint64_t)_mm_cvtsi128_si64(s1) +
                (uint64_t)_mm_extract_epi64(s1, 1));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_u16(sad_out_x, SAD_BLOCKS);
}

TARGET_AVX2 NOINLINE static double sad_ymm(void)
{
    for (int pass = 0; pass < SAD_PASSES; ++pass) {
        for (size_t i = 0; i < SAD_BLOCKS; ++i) {
            const size_t p = i * 32u;
            const __m256i s = _mm256_sad_epu8(
                _mm256_load_si256((const __m256i *)&sad_ref[p]),
                _mm256_load_si256((const __m256i *)&sad_cur[p]));
            alignas(32) uint64_t lanes[4];
            _mm256_store_si256((__m256i *)lanes, s);
            sad_out_y[i] =
                (uint16_t)(lanes[0] + lanes[1] + lanes[2] + lanes[3]);
        }
        COMPILER_BARRIER();
    }
    return sample_sum_u16(sad_out_y, SAD_BLOCKS);
}

/* ------------------------------------------------------ 4K luma tile sums */

SCALAR_FN static double luma_scalar(void)
{
    for (int pass = 0; pass < LUMA_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < LUMA_TILES; ++i) {
            const size_t p = i * 16u;
            uint32_t sum = 0;
            CLANG_NO_VECTORIZE
            for (size_t j = 0; j < 16u; ++j) sum += luma_in[p + j];
            luma_out_s[i] = (uint16_t)sum;
        }
        COMPILER_BARRIER();
    }
    return sample_sum_u16(luma_out_s, LUMA_TILES);
}

TARGET_SSE41 NOINLINE static double luma_xmm(void)
{
    for (int pass = 0; pass < LUMA_PASSES; ++pass) {
        for (size_t i = 0; i < LUMA_TILES; ++i) {
            const size_t p = i * 16u;
            luma_out_x[i] = (uint16_t)(
                hsum_u16x8(_mm_load_si128((const __m128i *)&luma_in[p])) +
                hsum_u16x8(_mm_load_si128(
                    (const __m128i *)&luma_in[p + 8u])));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_u16(luma_out_x, LUMA_TILES);
}

TARGET_AVX2 NOINLINE static double luma_ymm(void)
{
    for (int pass = 0; pass < LUMA_PASSES; ++pass) {
        for (size_t i = 0; i < LUMA_TILES; ++i) {
            luma_out_y[i] = (uint16_t)hsum_u16x16(
                _mm256_load_si256((const __m256i *)&luma_in[i * 16u]));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_u16(luma_out_y, LUMA_TILES);
}

static inline void extrema16_u16(const uint16_t *src, size_t p,
                                 uint16_t *lo, uint16_t *hi)
{
    uint16_t l = src[p], h = src[p];
    for (size_t j = 1; j < 16u; ++j) {
        const uint16_t v = src[p + j];
        if (v < l) l = v;
        if (v > h) h = v;
    }
    *lo = l;
    *hi = h;
}

static inline void extrema16_i16(const int16_t *src, size_t p,
                                 int16_t *lo, int16_t *hi)
{
    int16_t l = src[p], h = src[p];
    for (size_t j = 1; j < 16u; ++j) {
        const int16_t v = src[p + j];
        if (v < l) l = v;
        if (v > h) h = v;
    }
    *lo = l;
    *hi = h;
}

static inline void extrema16_i8(const int8_t *src, size_t p,
                                int8_t *lo, int8_t *hi)
{
    int8_t l = src[p], h = src[p];
    for (size_t j = 1; j < 16u; ++j) {
        const int8_t v = src[p + j];
        if (v < l) l = v;
        if (v > h) h = v;
    }
    *lo = l;
    *hi = h;
}

/* ---------------------------------------------------- 4K depth tile range */

SCALAR_FN static double depth_scalar(void)
{
    for (int pass = 0; pass < DEPTH_PASSES; ++pass) {
        for (size_t i = 0; i < DEPTH_TILES; ++i) {
            extrema16_u16(depth_in, i * 16u,
                          &depth_lo_s[i], &depth_hi_s[i]);
        }
        COMPILER_BARRIER();
    }
    return sample_sum_u16(depth_lo_s, DEPTH_TILES) +
           sample_sum_u16(depth_hi_s, DEPTH_TILES);
}

TARGET_SSE41 NOINLINE static double depth_xmm(void)
{
    for (int pass = 0; pass < DEPTH_PASSES; ++pass) {
        for (size_t i = 0; i < DEPTH_TILES; ++i) {
            const size_t p = i * 16u;
            const __m128i a =
                _mm_load_si128((const __m128i *)&depth_in[p]);
            const __m128i b =
                _mm_load_si128((const __m128i *)&depth_in[p + 8u]);
            depth_lo_x[i] = hmin_u16x8(_mm_min_epu16(a, b));
            depth_hi_x[i] = hmax_u16x8(_mm_max_epu16(a, b));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_u16(depth_lo_x, DEPTH_TILES) +
           sample_sum_u16(depth_hi_x, DEPTH_TILES);
}

TARGET_AVX2 NOINLINE static double depth_ymm(void)
{
    for (int pass = 0; pass < DEPTH_PASSES; ++pass) {
        for (size_t i = 0; i < DEPTH_TILES; ++i) {
            const __m256i v = _mm256_load_si256(
                (const __m256i *)&depth_in[i * 16u]);
            depth_lo_y[i] = hmin_u16x16(v);
            depth_hi_y[i] = hmax_u16x16(v);
        }
        COMPILER_BARRIER();
    }
    return sample_sum_u16(depth_lo_y, DEPTH_TILES) +
           sample_sum_u16(depth_hi_y, DEPTH_TILES);
}

/* ------------------------------------------------------ PCM peak envelope */

SCALAR_FN static double pcm_scalar(void)
{
    for (int pass = 0; pass < PCM_PASSES; ++pass) {
        for (size_t i = 0; i < PCM_BLOCKS; ++i) {
            extrema16_i16(pcm_in, i * 16u, &pcm_lo_s[i], &pcm_hi_s[i]);
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i16(pcm_lo_s, PCM_BLOCKS) +
           sample_sum_i16(pcm_hi_s, PCM_BLOCKS);
}

TARGET_SSE41 NOINLINE static double pcm_xmm(void)
{
    for (int pass = 0; pass < PCM_PASSES; ++pass) {
        for (size_t i = 0; i < PCM_BLOCKS; ++i) {
            const size_t p = i * 16u;
            const __m128i a = _mm_load_si128((const __m128i *)&pcm_in[p]);
            const __m128i b =
                _mm_load_si128((const __m128i *)&pcm_in[p + 8u]);
            pcm_lo_x[i] = hmin_i16x8(_mm_min_epi16(a, b));
            pcm_hi_x[i] = hmax_i16x8(_mm_max_epi16(a, b));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i16(pcm_lo_x, PCM_BLOCKS) +
           sample_sum_i16(pcm_hi_x, PCM_BLOCKS);
}

TARGET_AVX2 NOINLINE static double pcm_ymm(void)
{
    for (int pass = 0; pass < PCM_PASSES; ++pass) {
        for (size_t i = 0; i < PCM_BLOCKS; ++i) {
            const __m256i v =
                _mm256_load_si256((const __m256i *)&pcm_in[i * 16u]);
            pcm_lo_y[i] = hmin_i16x16(v);
            pcm_hi_y[i] = hmax_i16x16(v);
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i16(pcm_lo_y, PCM_BLOCKS) +
           sample_sum_i16(pcm_hi_y, PCM_BLOCKS);
}

/* --------------------------------------------------- float PCM noise gate */

SCALAR_FN static double gate_scalar(void)
{
    const double threshold = 0.18, gain = 1.75;
    for (int pass = 0; pass < GATE_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < GATE_SAMPLES; ++i) {
            const double x = (double)gate_in[i];
            gate_out_s[i] =
                (float)(fabs(x) > threshold ? x * gain : 0.0);
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(gate_out_s, GATE_SAMPLES);
}

TARGET_SSE41 NOINLINE static double gate_xmm(void)
{
    const __m128 threshold = _mm_set1_ps(0.18f);
    const __m128 gain = _mm_set1_ps(1.75f);
    const __m128 absmask =
        _mm_castsi128_ps(_mm_set1_epi32(INT32_C(0x7fffffff)));
    for (int pass = 0; pass < GATE_PASSES; ++pass) {
        for (size_t i = 0; i < GATE_SAMPLES; i += 4u) {
            const __m128 x = _mm_load_ps(&gate_in[i]);
            const __m128 active =
                _mm_cmpgt_ps(_mm_and_ps(x, absmask), threshold);
            _mm_store_ps(&gate_out_x[i],
                         _mm_and_ps(active, _mm_mul_ps(x, gain)));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(gate_out_x, GATE_SAMPLES);
}

TARGET_AVX2 NOINLINE static double gate_ymm(void)
{
    const __m256 threshold = _mm256_set1_ps(0.18f);
    const __m256 gain = _mm256_set1_ps(1.75f);
    const __m256 absmask =
        _mm256_castsi256_ps(_mm256_set1_epi32(INT32_C(0x7fffffff)));
    for (int pass = 0; pass < GATE_PASSES; ++pass) {
        for (size_t i = 0; i < GATE_SAMPLES; i += 8u) {
            const __m256 x = _mm256_load_ps(&gate_in[i]);
            const __m256 active = _mm256_cmp_ps(
                _mm256_and_ps(x, absmask), threshold, _CMP_GT_OQ);
            _mm256_store_ps(&gate_out_y[i],
                            _mm256_and_ps(active, _mm256_mul_ps(x, gain)));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(gate_out_y, GATE_SAMPLES);
}

/* ---------------------------------------------- PCM16 16-tap decimation */

static const int16_t pcm_fir_coeff[16] = {
    -3, -2, -1, 0, 1, 2, 3, 4, 4, 3, 2, 1, 0, -1, -2, -3,
};

SCALAR_FN static double pcm_fir_scalar(void)
{
    for (int pass = 0; pass < PCM_FIR_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < PCM_FIR_BLOCKS; ++i) {
            const size_t p = i * 16u;
            int32_t sum = 0;
            CLANG_NO_VECTORIZE
            for (size_t j = 0; j < 16u; ++j) {
                sum += (int32_t)pcm_fir_in[p + j] * pcm_fir_coeff[j];
            }
            pcm_fir_out_s[i] = (int16_t)sum;
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i16(pcm_fir_out_s, PCM_FIR_BLOCKS);
}

TARGET_SSE41 NOINLINE static double pcm_fir_xmm(void)
{
    const __m128i c0 = _mm_loadu_si128((const __m128i *)pcm_fir_coeff);
    const __m128i c1 =
        _mm_loadu_si128((const __m128i *)&pcm_fir_coeff[8]);
    for (int pass = 0; pass < PCM_FIR_PASSES; ++pass) {
        for (size_t i = 0; i < PCM_FIR_BLOCKS; ++i) {
            const size_t p = i * 16u;
            pcm_fir_out_x[i] = (int16_t)(
                dot_i16x8(_mm_load_si128((const __m128i *)&pcm_fir_in[p]), c0) +
                dot_i16x8(_mm_load_si128(
                    (const __m128i *)&pcm_fir_in[p + 8u]), c1));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i16(pcm_fir_out_x, PCM_FIR_BLOCKS);
}

TARGET_AVX2 NOINLINE static double pcm_fir_ymm(void)
{
    const __m256i c =
        _mm256_loadu_si256((const __m256i *)pcm_fir_coeff);
    for (int pass = 0; pass < PCM_FIR_PASSES; ++pass) {
        for (size_t i = 0; i < PCM_FIR_BLOCKS; ++i) {
            pcm_fir_out_y[i] = (int16_t)dot_i16x16(
                _mm256_load_si256(
                    (const __m256i *)&pcm_fir_in[i * 16u]), c);
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i16(pcm_fir_out_y, PCM_FIR_BLOCKS);
}

/* ---------------------------------------------------------- eight-tap FIR */

static const double fc0 = 0.125;
static const double fc1 = -0.25;
static const double fc2 = 0.375;
static const double fc3 = 0.5;
static const double fc4 = -0.5;
static const double fc5 = 0.25;
static const double fc6 = -0.125;
static const double fc7 = 0.0625;

SCALAR_FN static double fir_scalar(void)
{
    for (int pass = 0; pass < FIR_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < FIR_N; ++i) {
            double acc = fc0 * (double)fir_in[i];
            acc += fc1 * (double)fir_in[i + 1u];
            acc += fc2 * (double)fir_in[i + 2u];
            acc += fc3 * (double)fir_in[i + 3u];
            acc += fc4 * (double)fir_in[i + 4u];
            acc += fc5 * (double)fir_in[i + 5u];
            acc += fc6 * (double)fir_in[i + 6u];
            fir_scalar_out[i] = (float)(acc + fc7 * (double)fir_in[i + 7u]);
        }
        COMPILER_BARRIER();
    }

    return (double)fir_scalar_out[0] +
           (double)fir_scalar_out[FIR_N / 2u] +
           (double)fir_scalar_out[FIR_N - 1u];
}

TARGET_SSE41 NOINLINE static double fir_xmm(void)
{
    const __m128 c0 = _mm_set1_ps((float)fc0);
    const __m128 c1 = _mm_set1_ps((float)fc1);
    const __m128 c2 = _mm_set1_ps((float)fc2);
    const __m128 c3 = _mm_set1_ps((float)fc3);
    const __m128 c4 = _mm_set1_ps((float)fc4);
    const __m128 c5 = _mm_set1_ps((float)fc5);
    const __m128 c6 = _mm_set1_ps((float)fc6);
    const __m128 c7 = _mm_set1_ps((float)fc7);

    for (int pass = 0; pass < FIR_PASSES; ++pass) {
        for (size_t i = 0; i < FIR_N; i += 4u) {
            __m128 acc = _mm_mul_ps(c0, _mm_loadu_ps(&fir_in[i]));
            acc = _mm_add_ps(acc, _mm_mul_ps(c1, _mm_loadu_ps(&fir_in[i + 1u])));
            acc = _mm_add_ps(acc, _mm_mul_ps(c2, _mm_loadu_ps(&fir_in[i + 2u])));
            acc = _mm_add_ps(acc, _mm_mul_ps(c3, _mm_loadu_ps(&fir_in[i + 3u])));
            acc = _mm_add_ps(acc, _mm_mul_ps(c4, _mm_loadu_ps(&fir_in[i + 4u])));
            acc = _mm_add_ps(acc, _mm_mul_ps(c5, _mm_loadu_ps(&fir_in[i + 5u])));
            acc = _mm_add_ps(acc, _mm_mul_ps(c6, _mm_loadu_ps(&fir_in[i + 6u])));
            acc = _mm_add_ps(acc, _mm_mul_ps(c7, _mm_loadu_ps(&fir_in[i + 7u])));
            _mm_store_ps(&fir_xmm_out[i], acc);
        }
        COMPILER_BARRIER();
    }

    return (double)fir_xmm_out[0] +
           (double)fir_xmm_out[FIR_N / 2u] +
           (double)fir_xmm_out[FIR_N - 1u];
}

TARGET_AVX2 NOINLINE static double fir_ymm(void)
{
    const __m256 c0 = _mm256_set1_ps((float)fc0);
    const __m256 c1 = _mm256_set1_ps((float)fc1);
    const __m256 c2 = _mm256_set1_ps((float)fc2);
    const __m256 c3 = _mm256_set1_ps((float)fc3);
    const __m256 c4 = _mm256_set1_ps((float)fc4);
    const __m256 c5 = _mm256_set1_ps((float)fc5);
    const __m256 c6 = _mm256_set1_ps((float)fc6);
    const __m256 c7 = _mm256_set1_ps((float)fc7);

    for (int pass = 0; pass < FIR_PASSES; ++pass) {
        for (size_t i = 0; i < FIR_N; i += 8u) {
            __m256 acc = _mm256_mul_ps(c0, _mm256_loadu_ps(&fir_in[i]));
            acc = _mm256_add_ps(acc, _mm256_mul_ps(c1, _mm256_loadu_ps(&fir_in[i + 1u])));
            acc = _mm256_add_ps(acc, _mm256_mul_ps(c2, _mm256_loadu_ps(&fir_in[i + 2u])));
            acc = _mm256_add_ps(acc, _mm256_mul_ps(c3, _mm256_loadu_ps(&fir_in[i + 3u])));
            acc = _mm256_add_ps(acc, _mm256_mul_ps(c4, _mm256_loadu_ps(&fir_in[i + 4u])));
            acc = _mm256_add_ps(acc, _mm256_mul_ps(c5, _mm256_loadu_ps(&fir_in[i + 5u])));
            acc = _mm256_add_ps(acc, _mm256_mul_ps(c6, _mm256_loadu_ps(&fir_in[i + 6u])));
            acc = _mm256_add_ps(acc, _mm256_mul_ps(c7, _mm256_loadu_ps(&fir_in[i + 7u])));
            _mm256_store_ps(&fir_ymm_out[i], acc);
        }
        COMPILER_BARRIER();
    }

    return (double)fir_ymm_out[0] +
           (double)fir_ymm_out[FIR_N / 2u] +
           (double)fir_ymm_out[FIR_N - 1u];
}

/* --------------------------------------------------- degree-11 polynomial */

static const double pc0 = 0.25;
static const double pc1 = -0.5;
static const double pc2 = 0.75;
static const double pc3 = -1.0;
static const double pc4 = 0.125;
static const double pc5 = 0.375;
static const double pc6 = -0.625;
static const double pc7 = 0.875;
static const double pc8 = -0.03125;
static const double pc9 = 0.0625;
static const double pc10 = -0.09375;
static const double pc11 = 0.015625;

SCALAR_FN static double poly_scalar(void)
{
    for (int pass = 0; pass < POLY_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < POLY_N; ++i) {
            const double x = poly_in[i];
            double acc = pc11;
            acc = acc * x + pc10;
            acc = acc * x + pc9;
            acc = acc * x + pc8;
            acc = acc * x + pc7;
            acc = acc * x + pc6;
            acc = acc * x + pc5;
            acc = acc * x + pc4;
            acc = acc * x + pc3;
            acc = acc * x + pc2;
            acc = acc * x + pc1;
            poly_scalar_out[i] = acc * x + pc0;
        }
        COMPILER_BARRIER();
    }

    return poly_scalar_out[0] +
           poly_scalar_out[POLY_N / 2u] +
           poly_scalar_out[POLY_N - 1u];
}

TARGET_SSE41 NOINLINE static double poly_xmm(void)
{
    const __m128d c0 = _mm_set1_pd(pc0);
    const __m128d c1 = _mm_set1_pd(pc1);
    const __m128d c2 = _mm_set1_pd(pc2);
    const __m128d c3 = _mm_set1_pd(pc3);
    const __m128d c4 = _mm_set1_pd(pc4);
    const __m128d c5 = _mm_set1_pd(pc5);
    const __m128d c6 = _mm_set1_pd(pc6);
    const __m128d c7 = _mm_set1_pd(pc7);
    const __m128d c8 = _mm_set1_pd(pc8);
    const __m128d c9 = _mm_set1_pd(pc9);
    const __m128d c10 = _mm_set1_pd(pc10);
    const __m128d c11 = _mm_set1_pd(pc11);

    for (int pass = 0; pass < POLY_PASSES; ++pass) {
        for (size_t i = 0; i < POLY_N; i += 2u) {
            const __m128d x = _mm_load_pd(&poly_in[i]);
            __m128d acc = c11;
            acc = _mm_add_pd(_mm_mul_pd(acc, x), c10);
            acc = _mm_add_pd(_mm_mul_pd(acc, x), c9);
            acc = _mm_add_pd(_mm_mul_pd(acc, x), c8);
            acc = _mm_add_pd(_mm_mul_pd(acc, x), c7);
            acc = _mm_add_pd(_mm_mul_pd(acc, x), c6);
            acc = _mm_add_pd(_mm_mul_pd(acc, x), c5);
            acc = _mm_add_pd(_mm_mul_pd(acc, x), c4);
            acc = _mm_add_pd(_mm_mul_pd(acc, x), c3);
            acc = _mm_add_pd(_mm_mul_pd(acc, x), c2);
            acc = _mm_add_pd(_mm_mul_pd(acc, x), c1);
            _mm_store_pd(&poly_xmm_out[i], _mm_add_pd(_mm_mul_pd(acc, x), c0));
        }
        COMPILER_BARRIER();
    }

    return poly_xmm_out[0] +
           poly_xmm_out[POLY_N / 2u] +
           poly_xmm_out[POLY_N - 1u];
}

TARGET_AVX2 NOINLINE static double poly_ymm(void)
{
    const __m256d c0 = _mm256_set1_pd(pc0);
    const __m256d c1 = _mm256_set1_pd(pc1);
    const __m256d c2 = _mm256_set1_pd(pc2);
    const __m256d c3 = _mm256_set1_pd(pc3);
    const __m256d c4 = _mm256_set1_pd(pc4);
    const __m256d c5 = _mm256_set1_pd(pc5);
    const __m256d c6 = _mm256_set1_pd(pc6);
    const __m256d c7 = _mm256_set1_pd(pc7);
    const __m256d c8 = _mm256_set1_pd(pc8);
    const __m256d c9 = _mm256_set1_pd(pc9);
    const __m256d c10 = _mm256_set1_pd(pc10);
    const __m256d c11 = _mm256_set1_pd(pc11);

    for (int pass = 0; pass < POLY_PASSES; ++pass) {
        for (size_t i = 0; i < POLY_N; i += 4u) {
            const __m256d x = _mm256_load_pd(&poly_in[i]);
            __m256d acc = c11;
            acc = _mm256_add_pd(_mm256_mul_pd(acc, x), c10);
            acc = _mm256_add_pd(_mm256_mul_pd(acc, x), c9);
            acc = _mm256_add_pd(_mm256_mul_pd(acc, x), c8);
            acc = _mm256_add_pd(_mm256_mul_pd(acc, x), c7);
            acc = _mm256_add_pd(_mm256_mul_pd(acc, x), c6);
            acc = _mm256_add_pd(_mm256_mul_pd(acc, x), c5);
            acc = _mm256_add_pd(_mm256_mul_pd(acc, x), c4);
            acc = _mm256_add_pd(_mm256_mul_pd(acc, x), c3);
            acc = _mm256_add_pd(_mm256_mul_pd(acc, x), c2);
            acc = _mm256_add_pd(_mm256_mul_pd(acc, x), c1);
            _mm256_store_pd(
                &poly_ymm_out[i],
                _mm256_add_pd(_mm256_mul_pd(acc, x), c0));
        }
        COMPILER_BARRIER();
    }

    return poly_ymm_out[0] +
           poly_ymm_out[POLY_N / 2u] +
           poly_ymm_out[POLY_N - 1u];
}

/* -------------------------------------------- fixed-iteration Mandelbrot */

SCALAR_FN static double mandel_scalar(void)
{
    int64_t total = 0;

    for (int pass = 0; pass < MANDEL_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < MANDEL_N; ++i) {
            const double cx = mandel_cx[i];
            const double cy = mandel_cy[i];
            double zr = 0.0;
            double zi = 0.0;
            int count = 0;
            bool active = true;

            for (int iter = 0; iter < MANDEL_ITERS; ++iter) {
                const double zr2 = zr * zr;
                const double zi2 = zi * zi;
                if (active && zr2 + zi2 <= 4.0) {
                    ++count;
                } else {
                    active = false;
                }
                zi = 2.0 * zr * zi + cy;
                zr = zr2 - zi2 + cx;
            }

            total += count;
        }
    }

    return (double)total;
}

TARGET_SSE41 NOINLINE static double mandel_xmm(void)
{
    const __m128d zero = _mm_setzero_pd();
    const __m128d two = _mm_set1_pd(2.0);
    const __m128d four = _mm_set1_pd(4.0);
    const __m128i izero = _mm_setzero_si128();
    const __m128i ones = _mm_set1_epi64x(INT64_C(-1));
    __m128i total = izero;

    for (int pass = 0; pass < MANDEL_PASSES; ++pass) {
        for (size_t i = 0; i < MANDEL_N; i += 2u) {
            const __m128d cx = _mm_load_pd(&mandel_cx[i]);
            const __m128d cy = _mm_load_pd(&mandel_cy[i]);
            __m128d zr = zero;
            __m128d zi = zero;
            __m128i count = izero;
            __m128i active = ones;

            for (int iter = 0; iter < MANDEL_ITERS; ++iter) {
                const __m128d zr2 = _mm_mul_pd(zr, zr);
                const __m128d zi2 = _mm_mul_pd(zi, zi);
                const __m128d live = _mm_cmple_pd(_mm_add_pd(zr2, zi2), four);
                active = _mm_and_si128(active, _mm_castpd_si128(live));
                count = _mm_sub_epi64(count, active);
                zi = _mm_add_pd(_mm_mul_pd(two, _mm_mul_pd(zr, zi)), cy);
                zr = _mm_add_pd(_mm_sub_pd(zr2, zi2), cx);
            }

            total = _mm_add_epi64(total, count);
        }
    }

    alignas(16) int64_t lanes[2];
    _mm_store_si128((__m128i *)lanes, total);
    return (double)lanes[0] + (double)lanes[1];
}

TARGET_AVX2 NOINLINE static double mandel_ymm(void)
{
    const __m256d zero = _mm256_setzero_pd();
    const __m256d two = _mm256_set1_pd(2.0);
    const __m256d four = _mm256_set1_pd(4.0);
    const __m256i izero = _mm256_setzero_si256();
    const __m256i ones = _mm256_set1_epi64x(INT64_C(-1));
    __m256i total = izero;

    for (int pass = 0; pass < MANDEL_PASSES; ++pass) {
        for (size_t i = 0; i < MANDEL_N; i += 4u) {
            const __m256d cx = _mm256_load_pd(&mandel_cx[i]);
            const __m256d cy = _mm256_load_pd(&mandel_cy[i]);
            __m256d zr = zero;
            __m256d zi = zero;
            __m256i count = izero;
            __m256i active = ones;

            for (int iter = 0; iter < MANDEL_ITERS; ++iter) {
                const __m256d zr2 = _mm256_mul_pd(zr, zr);
                const __m256d zi2 = _mm256_mul_pd(zi, zi);
                const __m256d live = _mm256_cmp_pd(
                    _mm256_add_pd(zr2, zi2), four, _CMP_LE_OQ);
                active = _mm256_and_si256(active, _mm256_castpd_si256(live));
                count = _mm256_sub_epi64(count, active);
                zi = _mm256_add_pd(
                    _mm256_mul_pd(two, _mm256_mul_pd(zr, zi)), cy);
                zr = _mm256_add_pd(_mm256_sub_pd(zr2, zi2), cx);
            }

            total = _mm256_add_epi64(total, count);
        }
    }

    alignas(32) int64_t lanes[4];
    _mm256_store_si256((__m256i *)lanes, total);
    return (double)lanes[0] + (double)lanes[1] +
           (double)lanes[2] + (double)lanes[3];
}

TARGET_SSE41 static inline int16_t wrap_hsum_i16x8(__m128i v)
{
    alignas(16) int16_t lanes[8];
    uint16_t sum = 0;
    _mm_store_si128((__m128i *)lanes, v);
    for (int i = 0; i < 8; ++i) sum = (uint16_t)(sum + (uint16_t)lanes[i]);
    return (int16_t)sum;
}

TARGET_AVX2 static inline int16_t wrap_hsum_i16x16(__m256i v)
{
    alignas(32) int16_t lanes[16];
    uint16_t sum = 0;
    _mm256_store_si256((__m256i *)lanes, v);
    for (int i = 0; i < 16; ++i) sum = (uint16_t)(sum + (uint16_t)lanes[i]);
    return (int16_t)sum;
}

/* ------------------------------------------- 4K residual quantization */

SCALAR_FN static double resid_scalar(void)
{
    for (int pass = 0; pass < RESID_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < RESID_N; ++i) {
            const int d = (int)resid_cur[i] - (int)resid_pred[i];
            int a0 = d + 1536, a1 = d + 512;
            int a2 = d - 512, a3 = d - 1536;
            if (a0 < 0) a0 = -a0;
            if (a1 < 0) a1 = -a1;
            if (a2 < 0) a2 = -a2;
            if (a3 < 0) a3 = -a3;
            const int m0 = a0 < a1 ? a0 : a1;
            const int m1 = a2 < a3 ? a2 : a3;
            const int mag = m0 < m1 ? m0 : m1;
            resid_out_s[i] = (int16_t)((mag * 7 + 8) >> 4);
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i16(resid_out_s, RESID_N);
}

TARGET_SSE41 NOINLINE static double resid_xmm(void)
{
    const __m128i k1536 = _mm_set1_epi16(1536);
    const __m128i k512 = _mm_set1_epi16(512);
    const __m128i k7 = _mm_set1_epi16(7);
    const __m128i k8 = _mm_set1_epi16(8);
    for (int pass = 0; pass < RESID_PASSES; ++pass) {
        for (size_t i = 0; i < RESID_N; i += 8u) {
            const __m128i d = _mm_sub_epi16(
                _mm_load_si128((const __m128i *)&resid_cur[i]),
                _mm_load_si128((const __m128i *)&resid_pred[i]));
            const __m128i a0 = _mm_abs_epi16(_mm_add_epi16(d, k1536));
            const __m128i a1 = _mm_abs_epi16(_mm_add_epi16(d, k512));
            const __m128i a2 = _mm_abs_epi16(_mm_sub_epi16(d, k512));
            const __m128i a3 = _mm_abs_epi16(_mm_sub_epi16(d, k1536));
            const __m128i mag = _mm_min_epi16(
                _mm_min_epi16(a0, a1), _mm_min_epi16(a2, a3));
            _mm_store_si128((__m128i *)&resid_out_x[i],
                _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(mag, k7), k8),
                               4));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i16(resid_out_x, RESID_N);
}

TARGET_AVX2 NOINLINE static double resid_ymm(void)
{
    const __m256i k1536 = _mm256_set1_epi16(1536);
    const __m256i k512 = _mm256_set1_epi16(512);
    const __m256i k7 = _mm256_set1_epi16(7);
    const __m256i k8 = _mm256_set1_epi16(8);
    for (int pass = 0; pass < RESID_PASSES; ++pass) {
        for (size_t i = 0; i < RESID_N; i += 16u) {
            const __m256i d = _mm256_sub_epi16(
                _mm256_load_si256((const __m256i *)&resid_cur[i]),
                _mm256_load_si256((const __m256i *)&resid_pred[i]));
            const __m256i a0 = _mm256_abs_epi16(_mm256_add_epi16(d, k1536));
            const __m256i a1 = _mm256_abs_epi16(_mm256_add_epi16(d, k512));
            const __m256i a2 = _mm256_abs_epi16(_mm256_sub_epi16(d, k512));
            const __m256i a3 = _mm256_abs_epi16(_mm256_sub_epi16(d, k1536));
            const __m256i mag = _mm256_min_epi16(
                _mm256_min_epi16(a0, a1), _mm256_min_epi16(a2, a3));
            _mm256_store_si256((__m256i *)&resid_out_y[i],
                _mm256_srli_epi16(
                    _mm256_add_epi16(_mm256_mullo_epi16(mag, k7), k8), 4));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i16(resid_out_y, RESID_N);
}

SCALAR_FN static double resid_signed_scalar(void)
{
    int64_t a1 = 0, a3 = 0, a5 = 0, a7 = 0;
    for (int pass = 0; pass < RESID_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < RESID_N; ++i) {
            const int d = (int)resid_cur[i] - (int)resid_pred[i];
            const int mag = d < 0 ? -d : d;
            int q1 = (mag + 8) >> 4;
            int q3 = (mag * 3 + 8) >> 4;
            int q5 = (mag * 5 + 8) >> 4;
            int q7 = (mag * 7 + 8) >> 4;
            if (d < 0) {
                q1 = -q1; q3 = -q3; q5 = -q5; q7 = -q7;
            }
            a1 += q1; a3 += q3; a5 += q5; a7 += q7;
        }
    }
    return (double)(int16_t)a1 + (double)(int16_t)a3 +
           (double)(int16_t)a5 + (double)(int16_t)a7;
}

TARGET_SSE41 NOINLINE static double resid_signed_xmm(void)
{
    const __m128i k3 = _mm_set1_epi16(3);
    const __m128i k5 = _mm_set1_epi16(5);
    const __m128i k7 = _mm_set1_epi16(7);
    const __m128i k8 = _mm_set1_epi16(8);
    __m128i a1 = _mm_setzero_si128(), a3 = a1, a5 = a1, a7 = a1;
    for (int pass = 0; pass < RESID_PASSES; ++pass) {
        for (size_t i = 0; i < RESID_N; i += 8u) {
            const __m128i d = _mm_sub_epi16(
                _mm_load_si128((const __m128i *)&resid_cur[i]),
                _mm_load_si128((const __m128i *)&resid_pred[i]));
            const __m128i mag = _mm_abs_epi16(d);
            __m128i q1 = _mm_srli_epi16(_mm_add_epi16(mag, k8), 4);
            __m128i q3 = _mm_srli_epi16(
                _mm_add_epi16(_mm_mullo_epi16(mag, k3), k8), 4);
            __m128i q5 = _mm_srli_epi16(
                _mm_add_epi16(_mm_mullo_epi16(mag, k5), k8), 4);
            __m128i q7 = _mm_srli_epi16(
                _mm_add_epi16(_mm_mullo_epi16(mag, k7), k8), 4);
            q1 = _mm_sign_epi16(q1, d);
            q3 = _mm_sign_epi16(q3, d);
            q5 = _mm_sign_epi16(q5, d);
            q7 = _mm_sign_epi16(q7, d);
            a1 = _mm_add_epi16(a1, q1);
            a3 = _mm_add_epi16(a3, q3);
            a5 = _mm_add_epi16(a5, q5);
            a7 = _mm_add_epi16(a7, q7);
        }
    }
    return (double)wrap_hsum_i16x8(a1) + (double)wrap_hsum_i16x8(a3) +
           (double)wrap_hsum_i16x8(a5) + (double)wrap_hsum_i16x8(a7);
}

TARGET_AVX2 NOINLINE static double resid_signed_ymm(void)
{
    const __m256i k3 = _mm256_set1_epi16(3);
    const __m256i k5 = _mm256_set1_epi16(5);
    const __m256i k7 = _mm256_set1_epi16(7);
    const __m256i k8 = _mm256_set1_epi16(8);
    __m256i a1 = _mm256_setzero_si256(), a3 = a1, a5 = a1, a7 = a1;
    for (int pass = 0; pass < RESID_PASSES; ++pass) {
        for (size_t i = 0; i < RESID_N; i += 16u) {
            const __m256i d = _mm256_sub_epi16(
                _mm256_load_si256((const __m256i *)&resid_cur[i]),
                _mm256_load_si256((const __m256i *)&resid_pred[i]));
            const __m256i mag = _mm256_abs_epi16(d);
            __m256i q1 = _mm256_srli_epi16(_mm256_add_epi16(mag, k8), 4);
            __m256i q3 = _mm256_srli_epi16(
                _mm256_add_epi16(_mm256_mullo_epi16(mag, k3), k8), 4);
            __m256i q5 = _mm256_srli_epi16(
                _mm256_add_epi16(_mm256_mullo_epi16(mag, k5), k8), 4);
            __m256i q7 = _mm256_srli_epi16(
                _mm256_add_epi16(_mm256_mullo_epi16(mag, k7), k8), 4);
            q1 = _mm256_sign_epi16(q1, d);
            q3 = _mm256_sign_epi16(q3, d);
            q5 = _mm256_sign_epi16(q5, d);
            q7 = _mm256_sign_epi16(q7, d);
            a1 = _mm256_add_epi16(a1, q1);
            a3 = _mm256_add_epi16(a3, q3);
            a5 = _mm256_add_epi16(a5, q5);
            a7 = _mm256_add_epi16(a7, q7);
        }
    }
    return (double)wrap_hsum_i16x16(a1) + (double)wrap_hsum_i16x16(a3) +
           (double)wrap_hsum_i16x16(a5) + (double)wrap_hsum_i16x16(a7);
}

/* -------------------------------------------------- INT8 activation range */

SCALAR_FN static double act_scalar(void)
{
    for (int pass = 0; pass < ACT_PASSES; ++pass) {
        for (size_t i = 0; i < ACT_BLOCKS; ++i) {
            int8_t l0, h0, l1, h1;
            extrema16_i8(act_in, i * 32u, &l0, &h0);
            extrema16_i8(act_in, i * 32u + 16u, &l1, &h1);
            act_lo_s[i] = l0 < l1 ? l0 : l1;
            act_hi_s[i] = h0 > h1 ? h0 : h1;
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i8(act_lo_s, ACT_BLOCKS) +
           sample_sum_i8(act_hi_s, ACT_BLOCKS);
}

TARGET_SSE41 NOINLINE static double act_xmm(void)
{
    for (int pass = 0; pass < ACT_PASSES; ++pass) {
        for (size_t i = 0; i < ACT_BLOCKS; ++i) {
            const size_t p = i * 32u;
            const __m128i a = _mm_load_si128((const __m128i *)&act_in[p]);
            const __m128i b =
                _mm_load_si128((const __m128i *)&act_in[p + 16u]);
            act_lo_x[i] = hmin_i8x16(_mm_min_epi8(a, b));
            act_hi_x[i] = hmax_i8x16(_mm_max_epi8(a, b));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i8(act_lo_x, ACT_BLOCKS) +
           sample_sum_i8(act_hi_x, ACT_BLOCKS);
}

TARGET_AVX2 NOINLINE static double act_ymm(void)
{
    for (int pass = 0; pass < ACT_PASSES; ++pass) {
        for (size_t i = 0; i < ACT_BLOCKS; ++i) {
            const __m256i v =
                _mm256_load_si256((const __m256i *)&act_in[i * 32u]);
            act_lo_y[i] = hmin_i8x32(v);
            act_hi_y[i] = hmax_i8x32(v);
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i8(act_lo_y, ACT_BLOCKS) +
           sample_sum_i8(act_hi_y, ACT_BLOCKS);
}

/* ---------------------------------------------------- INT8 ternary filter */

static const int8_t ternary_weight[32] = {
    -2,-1,0,1,2,1,0,-1, -2,0,2,0,-2,1,-1,2,
     2,1,0,-1,-2,-1,0,1, 2,0,-2,0,2,-1,1,-2,
};

SCALAR_FN static double ternary_scalar(void)
{
    for (int pass = 0; pass < TERNARY_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < TERNARY_BLOCKS; ++i) {
            const size_t p = i * 32u;
            int sum = 0;
            CLANG_NO_VECTORIZE
            for (size_t j = 0; j < 32u; ++j) {
                sum += (int)ternary_in[p + j] * (int)ternary_weight[j];
            }
            ternary_out_s[i] = (int8_t)sum;
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i8(ternary_out_s, TERNARY_BLOCKS);
}

TARGET_SSE41 NOINLINE static double ternary_xmm(void)
{
    const __m128i w0 =
        _mm_loadu_si128((const __m128i *)ternary_weight);
    const __m128i w1 =
        _mm_loadu_si128((const __m128i *)&ternary_weight[16]);
    for (int pass = 0; pass < TERNARY_PASSES; ++pass) {
        for (size_t i = 0; i < TERNARY_BLOCKS; ++i) {
            const size_t p = i * 32u;
            ternary_out_x[i] = (int8_t)(
                dot_i8x16(_mm_load_si128((const __m128i *)&ternary_in[p]), w0) +
                dot_i8x16(_mm_load_si128(
                    (const __m128i *)&ternary_in[p + 16u]), w1));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i8(ternary_out_x, TERNARY_BLOCKS);
}

TARGET_AVX2 NOINLINE static double ternary_ymm(void)
{
    const __m256i w =
        _mm256_loadu_si256((const __m256i *)ternary_weight);
    for (int pass = 0; pass < TERNARY_PASSES; ++pass) {
        for (size_t i = 0; i < TERNARY_BLOCKS; ++i) {
            ternary_out_y[i] = (int8_t)dot_i8x32(
                _mm256_load_si256(
                    (const __m256i *)&ternary_in[i * 32u]), w);
        }
        COMPILER_BARRIER();
    }
    return sample_sum_i8(ternary_out_y, TERNARY_BLOCKS);
}

/* -------------------------------------------------------- 1080p Gaussian */

SCALAR_FN static double blur_scalar(void)
{
    for (int pass = 0; pass < BLUR_PASSES; ++pass) {
        for (size_t y = 0; y < BLUR_ROWS; ++y) {
            const size_t si = y * BLUR_STRIDE, di = y * BLUR_W;
            CLANG_NO_VECTORIZE
            for (size_t x = 0; x < BLUR_W; ++x) {
                const float edge = blur_in[si + x] + blur_in[si + x + 4u];
                const float near =
                    (blur_in[si + x + 1u] + blur_in[si + x + 3u]) * 4.0f;
                blur_tmp_s[di + x] =
                    (edge + near + blur_in[si + x + 2u] * 6.0f) * 0.0625f;
            }
        }
        for (size_t y = 0; y < BLUR_H; ++y) {
            const size_t r0 = y * BLUR_W, d = r0;
            const size_t r1 = r0 + BLUR_W, r2 = r1 + BLUR_W;
            const size_t r3 = r2 + BLUR_W, r4 = r3 + BLUR_W;
            CLANG_NO_VECTORIZE
            for (size_t x = 0; x < BLUR_W; ++x) {
                const float edge = blur_tmp_s[r0 + x] + blur_tmp_s[r4 + x];
                const float near =
                    (blur_tmp_s[r1 + x] + blur_tmp_s[r3 + x]) * 4.0f;
                blur_out_s[d + x] =
                    (edge + near + blur_tmp_s[r2 + x] * 6.0f) * 0.0625f;
            }
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(blur_out_s, BLUR_N);
}

TARGET_SSE41 NOINLINE static double blur_xmm(void)
{
    const __m128 four = _mm_set1_ps(4.0f);
    const __m128 six = _mm_set1_ps(6.0f);
    const __m128 inv = _mm_set1_ps(0.0625f);
    for (int pass = 0; pass < BLUR_PASSES; ++pass) {
        for (size_t y = 0; y < BLUR_ROWS; ++y) {
            const size_t si = y * BLUR_STRIDE, di = y * BLUR_W;
            for (size_t x = 0; x < BLUR_W; x += 4u) {
                const __m128 edge = _mm_add_ps(
                    _mm_loadu_ps(&blur_in[si + x]),
                    _mm_loadu_ps(&blur_in[si + x + 4u]));
                const __m128 near = _mm_mul_ps(_mm_add_ps(
                    _mm_loadu_ps(&blur_in[si + x + 1u]),
                    _mm_loadu_ps(&blur_in[si + x + 3u])), four);
                _mm_store_ps(&blur_tmp_x[di + x], _mm_mul_ps(
                    _mm_add_ps(_mm_add_ps(edge, near), _mm_mul_ps(
                        _mm_loadu_ps(&blur_in[si + x + 2u]), six)), inv));
            }
        }
        for (size_t y = 0; y < BLUR_H; ++y) {
            const size_t r0 = y * BLUR_W, r1 = r0 + BLUR_W;
            const size_t r2 = r1 + BLUR_W, r3 = r2 + BLUR_W;
            const size_t r4 = r3 + BLUR_W;
            for (size_t x = 0; x < BLUR_W; x += 4u) {
                const __m128 edge = _mm_add_ps(
                    _mm_load_ps(&blur_tmp_x[r0 + x]),
                    _mm_load_ps(&blur_tmp_x[r4 + x]));
                const __m128 near = _mm_mul_ps(_mm_add_ps(
                    _mm_load_ps(&blur_tmp_x[r1 + x]),
                    _mm_load_ps(&blur_tmp_x[r3 + x])), four);
                _mm_store_ps(&blur_out_x[r0 + x], _mm_mul_ps(
                    _mm_add_ps(_mm_add_ps(edge, near), _mm_mul_ps(
                        _mm_load_ps(&blur_tmp_x[r2 + x]), six)), inv));
            }
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(blur_out_x, BLUR_N);
}

TARGET_AVX2 NOINLINE static double blur_ymm(void)
{
    const __m256 four = _mm256_set1_ps(4.0f);
    const __m256 six = _mm256_set1_ps(6.0f);
    const __m256 inv = _mm256_set1_ps(0.0625f);
    for (int pass = 0; pass < BLUR_PASSES; ++pass) {
        for (size_t y = 0; y < BLUR_ROWS; ++y) {
            const size_t si = y * BLUR_STRIDE, di = y * BLUR_W;
            for (size_t x = 0; x < BLUR_W; x += 8u) {
                const __m256 edge = _mm256_add_ps(
                    _mm256_loadu_ps(&blur_in[si + x]),
                    _mm256_loadu_ps(&blur_in[si + x + 4u]));
                const __m256 near = _mm256_mul_ps(_mm256_add_ps(
                    _mm256_loadu_ps(&blur_in[si + x + 1u]),
                    _mm256_loadu_ps(&blur_in[si + x + 3u])), four);
                _mm256_store_ps(&blur_tmp_y[di + x], _mm256_mul_ps(
                    _mm256_add_ps(_mm256_add_ps(edge, near), _mm256_mul_ps(
                        _mm256_loadu_ps(&blur_in[si + x + 2u]), six)), inv));
            }
        }
        for (size_t y = 0; y < BLUR_H; ++y) {
            const size_t r0 = y * BLUR_W, r1 = r0 + BLUR_W;
            const size_t r2 = r1 + BLUR_W, r3 = r2 + BLUR_W;
            const size_t r4 = r3 + BLUR_W;
            for (size_t x = 0; x < BLUR_W; x += 8u) {
                const __m256 edge = _mm256_add_ps(
                    _mm256_load_ps(&blur_tmp_y[r0 + x]),
                    _mm256_load_ps(&blur_tmp_y[r4 + x]));
                const __m256 near = _mm256_mul_ps(_mm256_add_ps(
                    _mm256_load_ps(&blur_tmp_y[r1 + x]),
                    _mm256_load_ps(&blur_tmp_y[r3 + x])), four);
                _mm256_store_ps(&blur_out_y[r0 + x], _mm256_mul_ps(
                    _mm256_add_ps(_mm256_add_ps(edge, near), _mm256_mul_ps(
                        _mm256_load_ps(&blur_tmp_y[r2 + x]), six)), inv));
            }
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(blur_out_y, BLUR_N);
}

/* ---------------------------------------------------------- 4K dilation */

SCALAR_FN static double morph_scalar(void)
{
    for (int pass = 0; pass < MORPH_PASSES; ++pass) {
        for (size_t y = 0; y < MORPH_H; ++y) {
            const size_t r0 = y * MORPH_STRIDE;
            const size_t r1 = r0 + MORPH_STRIDE;
            const size_t r2 = r1 + MORPH_STRIDE;
            const size_t d = y * MORPH_W;
            CLANG_NO_VECTORIZE
            for (size_t x = 0; x < MORPH_W; ++x) {
                float m = morph_in[r0 + x];
                const float v1 = morph_in[r0 + x + 1u];
                const float v2 = morph_in[r0 + x + 2u];
                const float v3 = morph_in[r1 + x];
                const float v4 = morph_in[r1 + x + 1u];
                const float v5 = morph_in[r1 + x + 2u];
                const float v6 = morph_in[r2 + x];
                const float v7 = morph_in[r2 + x + 1u];
                const float v8 = morph_in[r2 + x + 2u];
                if (v1 > m) m = v1;
                if (v2 > m) m = v2;
                if (v3 > m) m = v3;
                if (v4 > m) m = v4;
                if (v5 > m) m = v5;
                if (v6 > m) m = v6;
                if (v7 > m) m = v7;
                if (v8 > m) m = v8;
                morph_out_s[d + x] = m;
            }
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(morph_out_s, MORPH_N);
}

TARGET_SSE41 static inline __m128 select_gt_xmm(__m128 a, __m128 b)
{
    return _mm_blendv_ps(a, b, _mm_cmpgt_ps(b, a));
}

TARGET_AVX2 static inline __m256 select_gt_ymm(__m256 a, __m256 b)
{
    return _mm256_blendv_ps(a, b, _mm256_cmp_ps(b, a, _CMP_GT_OQ));
}

TARGET_SSE41 NOINLINE static double morph_xmm(void)
{
    for (int pass = 0; pass < MORPH_PASSES; ++pass) {
        for (size_t y = 0; y < MORPH_H; ++y) {
            const size_t r0 = y * MORPH_STRIDE;
            const size_t r1 = r0 + MORPH_STRIDE;
            const size_t r2 = r1 + MORPH_STRIDE;
            const size_t d = y * MORPH_W;
            for (size_t x = 0; x < MORPH_W; x += 4u) {
                __m128 m = _mm_loadu_ps(&morph_in[r0 + x]);
                m = select_gt_xmm(m, _mm_loadu_ps(&morph_in[r0 + x + 1u]));
                m = select_gt_xmm(m, _mm_loadu_ps(&morph_in[r0 + x + 2u]));
                m = select_gt_xmm(m, _mm_loadu_ps(&morph_in[r1 + x]));
                m = select_gt_xmm(m, _mm_loadu_ps(&morph_in[r1 + x + 1u]));
                m = select_gt_xmm(m, _mm_loadu_ps(&morph_in[r1 + x + 2u]));
                m = select_gt_xmm(m, _mm_loadu_ps(&morph_in[r2 + x]));
                m = select_gt_xmm(m, _mm_loadu_ps(&morph_in[r2 + x + 1u]));
                m = select_gt_xmm(m, _mm_loadu_ps(&morph_in[r2 + x + 2u]));
                _mm_store_ps(&morph_out_x[d + x], m);
            }
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(morph_out_x, MORPH_N);
}

TARGET_AVX2 NOINLINE static double morph_ymm(void)
{
    for (int pass = 0; pass < MORPH_PASSES; ++pass) {
        for (size_t y = 0; y < MORPH_H; ++y) {
            const size_t r0 = y * MORPH_STRIDE;
            const size_t r1 = r0 + MORPH_STRIDE;
            const size_t r2 = r1 + MORPH_STRIDE;
            const size_t d = y * MORPH_W;
            for (size_t x = 0; x < MORPH_W; x += 8u) {
                __m256 m = _mm256_loadu_ps(&morph_in[r0 + x]);
                m = select_gt_ymm(m, _mm256_loadu_ps(&morph_in[r0 + x + 1u]));
                m = select_gt_ymm(m, _mm256_loadu_ps(&morph_in[r0 + x + 2u]));
                m = select_gt_ymm(m, _mm256_loadu_ps(&morph_in[r1 + x]));
                m = select_gt_ymm(m, _mm256_loadu_ps(&morph_in[r1 + x + 1u]));
                m = select_gt_ymm(m, _mm256_loadu_ps(&morph_in[r1 + x + 2u]));
                m = select_gt_ymm(m, _mm256_loadu_ps(&morph_in[r2 + x]));
                m = select_gt_ymm(m, _mm256_loadu_ps(&morph_in[r2 + x + 1u]));
                m = select_gt_ymm(m, _mm256_loadu_ps(&morph_in[r2 + x + 2u]));
                _mm256_store_ps(&morph_out_y[d + x], m);
            }
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(morph_out_y, MORPH_N);
}

/* ------------------------------------------------ weighted point transform */

SCALAR_FN static double xform_scalar(void)
{
    for (int pass = 0; pass < XFORM_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < XFORM_N; ++i) {
            const size_t p = i * 4u;
            const float x = xform_in[p], y = xform_in[p + 1u];
            const float z = xform_in[p + 2u], w = xform_weight[i];
            xform_out_s[p] =
                ((1.125f*x + -0.375f*y) + 0.25f*z) + 3.5f*w;
            xform_out_s[p + 1u] =
                ((0.125f*x + 0.875f*y) + -0.5f*z) + -2.25f*w;
            xform_out_s[p + 2u] =
                ((-0.25f*x + 0.375f*y) + 1.25f*z) + 0.75f*w;
            xform_out_s[p + 3u] =
                ((0.03125f*x + -0.0625f*y) + 0.125f*z) + w;
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(xform_out_s, XFORM_N * 4u);
}

TARGET_SSE41 NOINLINE static double xform_xmm(void)
{
    const __m128 c0 = _mm_setr_ps(1.125f, 0.125f, -0.25f, 0.03125f);
    const __m128 c1 = _mm_setr_ps(-0.375f, 0.875f, 0.375f, -0.0625f);
    const __m128 c2 = _mm_setr_ps(0.25f, -0.5f, 1.25f, 0.125f);
    const __m128 c3 = _mm_setr_ps(3.5f, -2.25f, 0.75f, 1.0f);
    for (int pass = 0; pass < XFORM_PASSES; ++pass) {
        for (size_t i = 0; i < XFORM_N; ++i) {
            __m128 p = _mm_load_ps(&xform_in[i * 4u]);
            p = _mm_insert_ps(p, _mm_load_ss(&xform_weight[i]), 0x30);
            const __m128 x = _mm_shuffle_ps(p, p, 0x00);
            const __m128 y = _mm_shuffle_ps(p, p, 0x55);
            const __m128 z = _mm_shuffle_ps(p, p, 0xaa);
            const __m128 w = _mm_shuffle_ps(p, p, 0xff);
            _mm_store_ps(&xform_out_x[i * 4u], _mm_add_ps(
                _mm_add_ps(_mm_add_ps(_mm_mul_ps(c0, x),
                                      _mm_mul_ps(c1, y)),
                           _mm_mul_ps(c2, z)), _mm_mul_ps(c3, w)));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(xform_out_x, XFORM_N * 4u);
}

TARGET_AVX2 NOINLINE static double xform_ymm(void)
{
    const __m256 c0 = _mm256_setr_ps(
        1.125f,0.125f,-0.25f,0.03125f,1.125f,0.125f,-0.25f,0.03125f);
    const __m256 c1 = _mm256_setr_ps(
        -0.375f,0.875f,0.375f,-0.0625f,-0.375f,0.875f,0.375f,-0.0625f);
    const __m256 c2 = _mm256_setr_ps(
        0.25f,-0.5f,1.25f,0.125f,0.25f,-0.5f,1.25f,0.125f);
    const __m256 c3 = _mm256_setr_ps(
        3.5f,-2.25f,0.75f,1.0f,3.5f,-2.25f,0.75f,1.0f);
    for (int pass = 0; pass < XFORM_PASSES; ++pass) {
        for (size_t i = 0; i < XFORM_N; i += 2u) {
            const __m256 raw = _mm256_load_ps(&xform_in[i * 4u]);
            __m128 lo = _mm256_castps256_ps128(raw);
            __m128 hi = _mm256_extractf128_ps(raw, 1);
            lo = _mm_insert_ps(lo, _mm_load_ss(&xform_weight[i]), 0x30);
            hi = _mm_insert_ps(hi, _mm_load_ss(&xform_weight[i + 1u]), 0x30);
            __m256 p = _mm256_castps128_ps256(lo);
            p = _mm256_insertf128_ps(p, hi, 1);
            const __m256 x = _mm256_permute_ps(p, 0x00);
            const __m256 y = _mm256_permute_ps(p, 0x55);
            const __m256 z = _mm256_permute_ps(p, 0xaa);
            const __m256 w = _mm256_permute_ps(p, 0xff);
            _mm256_store_ps(&xform_out_y[i * 4u], _mm256_add_ps(
                _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(c0, x),
                                            _mm256_mul_ps(c1, y)),
                              _mm256_mul_ps(c2, z)), _mm256_mul_ps(c3, w)));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(xform_out_y, XFORM_N * 4u);
}

/* ----------------------------------------------- 3D voxel index checksum */

TARGET_SSE41 static inline __m128i cvttpd_i64x2(__m128d v)
{
    const int64_t lo = _mm_cvttsd_si64(v);
    const int64_t hi = _mm_cvttsd_si64(_mm_unpackhi_pd(v, v));
    return _mm_set_epi64x(hi, lo);
}

TARGET_AVX2 static inline __m256i cvttpd_i64x4(__m256d v)
{
    const __m128i lo = cvttpd_i64x2(_mm256_castpd256_pd128(v));
    const __m128i hi = cvttpd_i64x2(_mm256_extractf128_pd(v, 1));
    __m256i out = _mm256_castsi128_si256(lo);
    return _mm256_inserti128_si256(out, hi, 1);
}

SCALAR_FN static double voxel_scalar(void)
{
    int64_t acc = 0;
    for (int pass = 0; pass < VOXEL_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < VOXEL_N; ++i) {
            const int64_t x = (int64_t)(voxel_x[i] * 1048576.0 + 17.75);
            const int64_t y = (int64_t)(voxel_y[i] * 524288.0 - 31.25);
            const int64_t z = (int64_t)(voxel_z[i] * 262144.0 + 7.5);
            acc += x + y * 3 + z * 5;
        }
    }
    return (double)acc;
}

TARGET_SSE41 NOINLINE static double voxel_xmm(void)
{
    const __m128d sx = _mm_set1_pd(1048576.0);
    const __m128d sy = _mm_set1_pd(524288.0);
    const __m128d sz = _mm_set1_pd(262144.0);
    const __m128d bx = _mm_set1_pd(17.75);
    const __m128d by = _mm_set1_pd(-31.25);
    const __m128d bz = _mm_set1_pd(7.5);
    __m128i acc = _mm_setzero_si128();
    for (int pass = 0; pass < VOXEL_PASSES; ++pass) {
        for (size_t i = 0; i < VOXEL_N; i += 2u) {
            const __m128i x = cvttpd_i64x2(
                _mm_add_pd(_mm_mul_pd(_mm_load_pd(&voxel_x[i]), sx), bx));
            const __m128i y = cvttpd_i64x2(
                _mm_add_pd(_mm_mul_pd(_mm_load_pd(&voxel_y[i]), sy), by));
            const __m128i z = cvttpd_i64x2(
                _mm_add_pd(_mm_mul_pd(_mm_load_pd(&voxel_z[i]), sz), bz));
            acc = _mm_add_epi64(acc, _mm_add_epi64(
                _mm_add_epi64(x, _mm_add_epi64(y, _mm_slli_epi64(y, 1))),
                _mm_add_epi64(z, _mm_slli_epi64(z, 2))));
        }
    }
    alignas(16) int64_t lanes[2];
    _mm_store_si128((__m128i *)lanes, acc);
    return (double)lanes[0] + (double)lanes[1];
}

TARGET_AVX2 NOINLINE static double voxel_ymm(void)
{
    const __m256d sx = _mm256_set1_pd(1048576.0);
    const __m256d sy = _mm256_set1_pd(524288.0);
    const __m256d sz = _mm256_set1_pd(262144.0);
    const __m256d bx = _mm256_set1_pd(17.75);
    const __m256d by = _mm256_set1_pd(-31.25);
    const __m256d bz = _mm256_set1_pd(7.5);
    __m256i acc = _mm256_setzero_si256();
    for (int pass = 0; pass < VOXEL_PASSES; ++pass) {
        for (size_t i = 0; i < VOXEL_N; i += 4u) {
            const __m256i x = cvttpd_i64x4(_mm256_add_pd(
                _mm256_mul_pd(_mm256_load_pd(&voxel_x[i]), sx), bx));
            const __m256i y = cvttpd_i64x4(_mm256_add_pd(
                _mm256_mul_pd(_mm256_load_pd(&voxel_y[i]), sy), by));
            const __m256i z = cvttpd_i64x4(_mm256_add_pd(
                _mm256_mul_pd(_mm256_load_pd(&voxel_z[i]), sz), bz));
            acc = _mm256_add_epi64(acc, _mm256_add_epi64(
                _mm256_add_epi64(x, _mm256_add_epi64(
                    y, _mm256_slli_epi64(y, 1))),
                _mm256_add_epi64(z, _mm256_slli_epi64(z, 2))));
        }
    }
    alignas(32) int64_t lanes[4];
    _mm256_store_si256((__m256i *)lanes, acc);
    return (double)lanes[0] + (double)lanes[1] +
           (double)lanes[2] + (double)lanes[3];
}

/* ----------------------------------------------------------- 64-tap FIR */

SCALAR_FN static double audio_scalar(void)
{
    for (int pass = 0; pass < AUDIO_PASSES; ++pass) {
        CLANG_NO_VECTORIZE
        for (size_t i = 0; i < AUDIO_N; ++i) {
            float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
            CLANG_NO_VECTORIZE
            for (size_t k = 0; k < 16u; ++k) {
                a0 += audio_in[i + k] * audio_coeff[k];
                a1 += audio_in[i + k + 16u] * audio_coeff[k + 16u];
                a2 += audio_in[i + k + 32u] * audio_coeff[k + 32u];
                a3 += audio_in[i + k + 48u] * audio_coeff[k + 48u];
            }
            audio_out_s[i] = (a0 + a1) + (a2 + a3);
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(audio_out_s, AUDIO_N);
}

TARGET_SSE41 NOINLINE static double audio_xmm(void)
{
    __m128 c[AUDIO_TAPS];
    for (size_t k = 0; k < AUDIO_TAPS; ++k) c[k] = _mm_set1_ps(audio_coeff[k]);
    for (int pass = 0; pass < AUDIO_PASSES; ++pass) {
        for (size_t i = 0; i < AUDIO_N; i += 4u) {
            __m128 a0 = _mm_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
            for (size_t k = 0; k < 16u; ++k) {
                a0 = _mm_add_ps(a0, _mm_mul_ps(
                    _mm_loadu_ps(&audio_in[i + k]), c[k]));
                a1 = _mm_add_ps(a1, _mm_mul_ps(
                    _mm_loadu_ps(&audio_in[i + k + 16u]), c[k + 16u]));
                a2 = _mm_add_ps(a2, _mm_mul_ps(
                    _mm_loadu_ps(&audio_in[i + k + 32u]), c[k + 32u]));
                a3 = _mm_add_ps(a3, _mm_mul_ps(
                    _mm_loadu_ps(&audio_in[i + k + 48u]), c[k + 48u]));
            }
            _mm_store_ps(&audio_out_x[i],
                         _mm_add_ps(_mm_add_ps(a0, a1), _mm_add_ps(a2, a3)));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(audio_out_x, AUDIO_N);
}

TARGET_AVX2 NOINLINE static double audio_ymm(void)
{
    __m256 c[AUDIO_TAPS];
    for (size_t k = 0; k < AUDIO_TAPS; ++k)
        c[k] = _mm256_set1_ps(audio_coeff[k]);
    for (int pass = 0; pass < AUDIO_PASSES; ++pass) {
        for (size_t i = 0; i < AUDIO_N; i += 8u) {
            __m256 a0 = _mm256_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
            for (size_t k = 0; k < 16u; ++k) {
                a0 = _mm256_add_ps(a0, _mm256_mul_ps(
                    _mm256_loadu_ps(&audio_in[i + k]), c[k]));
                a1 = _mm256_add_ps(a1, _mm256_mul_ps(
                    _mm256_loadu_ps(&audio_in[i + k + 16u]), c[k + 16u]));
                a2 = _mm256_add_ps(a2, _mm256_mul_ps(
                    _mm256_loadu_ps(&audio_in[i + k + 32u]), c[k + 32u]));
                a3 = _mm256_add_ps(a3, _mm256_mul_ps(
                    _mm256_loadu_ps(&audio_in[i + k + 48u]), c[k + 48u]));
            }
            _mm256_store_ps(&audio_out_y[i], _mm256_add_ps(
                _mm256_add_ps(a0, a1), _mm256_add_ps(a2, a3)));
        }
        COMPILER_BARRIER();
    }
    return sample_sum_f32(audio_out_y, AUDIO_N);
}

/* ------------------------------------------------ particle simulation */

SCALAR_FN static double particle_scalar(void)
{
    const double dt = 0.0125;
    const double mu = -1.35;
    const double soft = 0.15;
    const double drag = 0.9992;
    const double floor_y = -2.0;
    const double bounce = 0.72;

    CLANG_NO_VECTORIZE
    for (size_t i = 0; i < PARTICLE_N; ++i) {
        double x = particle_x[i], y = particle_y[i], z = particle_z[i];
        double vx = particle_vx[i], vy = particle_vy[i], vz = particle_vz[i];
        for (int step = 0; step < PARTICLE_STEPS; ++step) {
            const double r2 = (x * x + y * y) + (z * z + soft);
            const double inv = 1.0 / sqrt(r2);
            const double force = mu * ((inv * inv) * inv);
            vx = (vx + x * force * dt) * drag;
            vy = (vy + y * force * dt) * drag;
            vz = (vz + z * force * dt) * drag;
            x += vx * dt;
            y += vy * dt;
            z += vz * dt;
            if (y < floor_y) {
                y = 2.0 * floor_y - y;
                vy = -vy * bounce;
            }
        }
        particle_out_s[i] = (float)x;
        particle_out_s[PARTICLE_N + i] = (float)y;
        particle_out_s[2u * PARTICLE_N + i] = (float)z;
    }
    COMPILER_BARRIER();
    return sample_sum_f32(particle_out_s, PARTICLE_N * 3u);
}

TARGET_SSE41 NOINLINE static double particle_xmm(void)
{
    const __m128 dt = _mm_set1_ps(0.0125f);
    const __m128 mu = _mm_set1_ps(-1.35f);
    const __m128 soft = _mm_set1_ps(0.15f);
    const __m128 drag = _mm_set1_ps(0.9992f);
    const __m128 one = _mm_set1_ps(1.0f);
    const __m128 floor_y = _mm_set1_ps(-2.0f);
    const __m128 floor2 = _mm_set1_ps(-4.0f);
    const __m128 bounce = _mm_set1_ps(0.72f);
    const __m128 zero = _mm_setzero_ps();

    for (size_t i = 0; i < PARTICLE_N; i += 4u) {
        __m128 x = _mm_load_ps(&particle_x[i]);
        __m128 y = _mm_load_ps(&particle_y[i]);
        __m128 z = _mm_load_ps(&particle_z[i]);
        __m128 vx = _mm_load_ps(&particle_vx[i]);
        __m128 vy = _mm_load_ps(&particle_vy[i]);
        __m128 vz = _mm_load_ps(&particle_vz[i]);
        for (int step = 0; step < PARTICLE_STEPS; ++step) {
            const __m128 r2 = _mm_add_ps(
                _mm_add_ps(_mm_mul_ps(x, x), _mm_mul_ps(y, y)),
                _mm_add_ps(_mm_mul_ps(z, z), soft));
            const __m128 inv = _mm_div_ps(one, _mm_sqrt_ps(r2));
            const __m128 force = _mm_mul_ps(
                mu, _mm_mul_ps(_mm_mul_ps(inv, inv), inv));
            vx = _mm_mul_ps(_mm_add_ps(
                vx, _mm_mul_ps(_mm_mul_ps(x, force), dt)), drag);
            vy = _mm_mul_ps(_mm_add_ps(
                vy, _mm_mul_ps(_mm_mul_ps(y, force), dt)), drag);
            vz = _mm_mul_ps(_mm_add_ps(
                vz, _mm_mul_ps(_mm_mul_ps(z, force), dt)), drag);
            x = _mm_add_ps(x, _mm_mul_ps(vx, dt));
            y = _mm_add_ps(y, _mm_mul_ps(vy, dt));
            z = _mm_add_ps(z, _mm_mul_ps(vz, dt));
            const __m128 hit = _mm_cmplt_ps(y, floor_y);
            y = _mm_blendv_ps(y, _mm_sub_ps(floor2, y), hit);
            vy = _mm_blendv_ps(
                vy, _mm_mul_ps(_mm_sub_ps(zero, vy), bounce), hit);
        }
        _mm_store_ps(&particle_out_x[i], x);
        _mm_store_ps(&particle_out_x[PARTICLE_N + i], y);
        _mm_store_ps(&particle_out_x[2u * PARTICLE_N + i], z);
    }
    COMPILER_BARRIER();
    return sample_sum_f32(particle_out_x, PARTICLE_N * 3u);
}

TARGET_AVX2 NOINLINE static double particle_ymm(void)
{
    const __m256 dt = _mm256_set1_ps(0.0125f);
    const __m256 mu = _mm256_set1_ps(-1.35f);
    const __m256 soft = _mm256_set1_ps(0.15f);
    const __m256 drag = _mm256_set1_ps(0.9992f);
    const __m256 one = _mm256_set1_ps(1.0f);
    const __m256 floor_y = _mm256_set1_ps(-2.0f);
    const __m256 floor2 = _mm256_set1_ps(-4.0f);
    const __m256 bounce = _mm256_set1_ps(0.72f);
    const __m256 zero = _mm256_setzero_ps();

    for (size_t i = 0; i < PARTICLE_N; i += 8u) {
        __m256 x = _mm256_load_ps(&particle_x[i]);
        __m256 y = _mm256_load_ps(&particle_y[i]);
        __m256 z = _mm256_load_ps(&particle_z[i]);
        __m256 vx = _mm256_load_ps(&particle_vx[i]);
        __m256 vy = _mm256_load_ps(&particle_vy[i]);
        __m256 vz = _mm256_load_ps(&particle_vz[i]);
        for (int step = 0; step < PARTICLE_STEPS; ++step) {
            const __m256 r2 = _mm256_add_ps(
                _mm256_add_ps(_mm256_mul_ps(x, x), _mm256_mul_ps(y, y)),
                _mm256_add_ps(_mm256_mul_ps(z, z), soft));
            const __m256 inv = _mm256_div_ps(one, _mm256_sqrt_ps(r2));
            const __m256 force = _mm256_mul_ps(
                mu, _mm256_mul_ps(_mm256_mul_ps(inv, inv), inv));
            vx = _mm256_mul_ps(_mm256_add_ps(
                vx, _mm256_mul_ps(_mm256_mul_ps(x, force), dt)), drag);
            vy = _mm256_mul_ps(_mm256_add_ps(
                vy, _mm256_mul_ps(_mm256_mul_ps(y, force), dt)), drag);
            vz = _mm256_mul_ps(_mm256_add_ps(
                vz, _mm256_mul_ps(_mm256_mul_ps(z, force), dt)), drag);
            x = _mm256_add_ps(x, _mm256_mul_ps(vx, dt));
            y = _mm256_add_ps(y, _mm256_mul_ps(vy, dt));
            z = _mm256_add_ps(z, _mm256_mul_ps(vz, dt));
            const __m256 hit = _mm256_cmp_ps(y, floor_y, _CMP_LT_OQ);
            y = _mm256_blendv_ps(y, _mm256_sub_ps(floor2, y), hit);
            vy = _mm256_blendv_ps(
                vy, _mm256_mul_ps(_mm256_sub_ps(zero, vy), bounce), hit);
        }
        _mm256_store_ps(&particle_out_y[i], x);
        _mm256_store_ps(&particle_out_y[PARTICLE_N + i], y);
        _mm256_store_ps(&particle_out_y[2u * PARTICLE_N + i], z);
    }
    COMPILER_BARRIER();
    return sample_sum_f32(particle_out_y, PARTICLE_N * 3u);
}

/* ------------------------------------------------- ChaCha20 block core */

static inline uint32_t rol32(uint32_t value, unsigned count)
{
    return (value << count) | (value >> (32u - count));
}

static inline void chacha_qr_scalar_words(
    uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    *a += *b; *d = rol32(*d ^ *a, 16);
    *c += *d; *b = rol32(*b ^ *c, 12);
    *a += *b; *d = rol32(*d ^ *a, 8);
    *c += *d; *b = rol32(*b ^ *c, 7);
}

static inline int32_t u32_as_i32(uint32_t value)
{
    int32_t result;
    memcpy(&result, &value, sizeof result);
    return result;
}

SCALAR_FN static double chacha_scalar(void)
{
    static const uint32_t initial[16] = {
        UINT32_C(0x61707865), UINT32_C(0x3320646e),
        UINT32_C(0x79622d32), UINT32_C(0x6b206574),
        UINT32_C(0x03020100), UINT32_C(0x07060504),
        UINT32_C(0x0b0a0908), UINT32_C(0x0f0e0d0c),
        UINT32_C(0x13121110), UINT32_C(0x17161514),
        UINT32_C(0x1b1a1918), UINT32_C(0x1f1e1d1c),
        0, UINT32_C(0x09000000), UINT32_C(0x4a000000), 0
    };
    uint32_t checksum = 0;
    uint32_t counter = 0;

    for (int pass = 0; pass < CHACHA_PASSES; ++pass) {
        for (size_t block = 0; block < CHACHA_BLOCKS; ++block) {
            uint32_t x[16];
            memcpy(x, initial, sizeof x);
            x[12] = counter;
            for (int round = 0; round < 10; ++round) {
                chacha_qr_scalar_words(&x[0], &x[4], &x[8], &x[12]);
                chacha_qr_scalar_words(&x[1], &x[5], &x[9], &x[13]);
                chacha_qr_scalar_words(&x[2], &x[6], &x[10], &x[14]);
                chacha_qr_scalar_words(&x[3], &x[7], &x[11], &x[15]);
                chacha_qr_scalar_words(&x[0], &x[5], &x[10], &x[15]);
                chacha_qr_scalar_words(&x[1], &x[6], &x[11], &x[12]);
                chacha_qr_scalar_words(&x[2], &x[7], &x[8], &x[13]);
                chacha_qr_scalar_words(&x[3], &x[4], &x[9], &x[14]);
            }
            for (int word = 0; word < 16; ++word) {
                x[word] += word == 12 ? counter : initial[word];
                checksum += x[word];
            }
            ++counter;
        }
    }
    COMPILER_BARRIER();
    return (double)u32_as_i32(checksum);
}

TARGET_SSE41 static inline __m128i chacha_rol16_xmm(__m128i value)
{
    const __m128i control = _mm_setr_epi8(
        2,3,0,1, 6,7,4,5, 10,11,8,9, 14,15,12,13);
    return _mm_shuffle_epi8(value, control);
}

TARGET_SSE41 static inline __m128i chacha_rol8_xmm(__m128i value)
{
    const __m128i control = _mm_setr_epi8(
        3,0,1,2, 7,4,5,6, 11,8,9,10, 15,12,13,14);
    return _mm_shuffle_epi8(value, control);
}

TARGET_SSE41 static inline __m128i chacha_rol12_xmm(__m128i value)
{
    return _mm_or_si128(_mm_slli_epi32(value, 12),
                        _mm_srli_epi32(value, 20));
}

TARGET_SSE41 static inline __m128i chacha_rol7_xmm(__m128i value)
{
    return _mm_or_si128(_mm_slli_epi32(value, 7),
                        _mm_srli_epi32(value, 25));
}

#define CHACHA_QR_XMM(a, b, c, d) do { \
    (a) = _mm_add_epi32((a), (b)); \
    (d) = chacha_rol16_xmm(_mm_xor_si128((d), (a))); \
    (c) = _mm_add_epi32((c), (d)); \
    (b) = chacha_rol12_xmm(_mm_xor_si128((b), (c))); \
    (a) = _mm_add_epi32((a), (b)); \
    (d) = chacha_rol8_xmm(_mm_xor_si128((d), (a))); \
    (c) = _mm_add_epi32((c), (d)); \
    (b) = chacha_rol7_xmm(_mm_xor_si128((b), (c))); \
} while (0)

TARGET_SSE41 NOINLINE static double chacha_xmm(void)
{
    const __m128i C0 = _mm_set1_epi32((int)UINT32_C(0x61707865));
    const __m128i C1 = _mm_set1_epi32((int)UINT32_C(0x3320646e));
    const __m128i C2 = _mm_set1_epi32((int)UINT32_C(0x79622d32));
    const __m128i C3 = _mm_set1_epi32((int)UINT32_C(0x6b206574));
    const __m128i K0 = _mm_set1_epi32((int)UINT32_C(0x03020100));
    const __m128i K1 = _mm_set1_epi32((int)UINT32_C(0x07060504));
    const __m128i K2 = _mm_set1_epi32((int)UINT32_C(0x0b0a0908));
    const __m128i K3 = _mm_set1_epi32((int)UINT32_C(0x0f0e0d0c));
    const __m128i K4 = _mm_set1_epi32((int)UINT32_C(0x13121110));
    const __m128i K5 = _mm_set1_epi32((int)UINT32_C(0x17161514));
    const __m128i K6 = _mm_set1_epi32((int)UINT32_C(0x1b1a1918));
    const __m128i K7 = _mm_set1_epi32((int)UINT32_C(0x1f1e1d1c));
    const __m128i N0 = _mm_set1_epi32((int)UINT32_C(0x09000000));
    const __m128i N1 = _mm_set1_epi32((int)UINT32_C(0x4a000000));
    const __m128i N2 = _mm_setzero_si128();
    __m128i counter = _mm_setr_epi32(0, 1, 2, 3);
    const __m128i step = _mm_set1_epi32(4);
    __m128i checksum = _mm_setzero_si128();

    for (size_t block = 0;
         block < CHACHA_PASSES * CHACHA_BLOCKS / 4u; ++block) {
        __m128i x0 = C0, x1 = C1, x2 = C2, x3 = C3;
        __m128i x4 = K0, x5 = K1, x6 = K2, x7 = K3;
        __m128i x8 = K4, x9 = K5, x10 = K6, x11 = K7;
        __m128i x12 = counter, x13 = N0, x14 = N1, x15 = N2;
        for (int round = 0; round < 10; ++round) {
            CHACHA_QR_XMM(x0, x4, x8, x12);
            CHACHA_QR_XMM(x1, x5, x9, x13);
            CHACHA_QR_XMM(x2, x6, x10, x14);
            CHACHA_QR_XMM(x3, x7, x11, x15);
            CHACHA_QR_XMM(x0, x5, x10, x15);
            CHACHA_QR_XMM(x1, x6, x11, x12);
            CHACHA_QR_XMM(x2, x7, x8, x13);
            CHACHA_QR_XMM(x3, x4, x9, x14);
        }
        x0 = _mm_add_epi32(x0, C0);
        x1 = _mm_add_epi32(x1, C1);
        x2 = _mm_add_epi32(x2, C2);
        x3 = _mm_add_epi32(x3, C3);
        x4 = _mm_add_epi32(x4, K0);
        x5 = _mm_add_epi32(x5, K1);
        x6 = _mm_add_epi32(x6, K2);
        x7 = _mm_add_epi32(x7, K3);
        x8 = _mm_add_epi32(x8, K4);
        x9 = _mm_add_epi32(x9, K5);
        x10 = _mm_add_epi32(x10, K6);
        x11 = _mm_add_epi32(x11, K7);
        x12 = _mm_add_epi32(x12, counter);
        x13 = _mm_add_epi32(x13, N0);
        x14 = _mm_add_epi32(x14, N1);
        x15 = _mm_add_epi32(x15, N2);
#define ADD_CHECKSUM_XMM(v) checksum = _mm_add_epi32(checksum, (v))
        ADD_CHECKSUM_XMM(x0);  ADD_CHECKSUM_XMM(x1);
        ADD_CHECKSUM_XMM(x2);  ADD_CHECKSUM_XMM(x3);
        ADD_CHECKSUM_XMM(x4);  ADD_CHECKSUM_XMM(x5);
        ADD_CHECKSUM_XMM(x6);  ADD_CHECKSUM_XMM(x7);
        ADD_CHECKSUM_XMM(x8);  ADD_CHECKSUM_XMM(x9);
        ADD_CHECKSUM_XMM(x10); ADD_CHECKSUM_XMM(x11);
        ADD_CHECKSUM_XMM(x12); ADD_CHECKSUM_XMM(x13);
        ADD_CHECKSUM_XMM(x14); ADD_CHECKSUM_XMM(x15);
#undef ADD_CHECKSUM_XMM
        counter = _mm_add_epi32(counter, step);
    }
    alignas(16) uint32_t lanes[4];
    _mm_store_si128((__m128i *)lanes, checksum);
    const uint32_t sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    COMPILER_BARRIER();
    return (double)u32_as_i32(sum);
}

TARGET_AVX2 static inline __m256i chacha_rol16_ymm(__m256i value)
{
    const __m256i control = _mm256_setr_epi8(
        2,3,0,1, 6,7,4,5, 10,11,8,9, 14,15,12,13,
        2,3,0,1, 6,7,4,5, 10,11,8,9, 14,15,12,13);
    return _mm256_shuffle_epi8(value, control);
}

TARGET_AVX2 static inline __m256i chacha_rol8_ymm(__m256i value)
{
    const __m256i control = _mm256_setr_epi8(
        3,0,1,2, 7,4,5,6, 11,8,9,10, 15,12,13,14,
        3,0,1,2, 7,4,5,6, 11,8,9,10, 15,12,13,14);
    return _mm256_shuffle_epi8(value, control);
}

TARGET_AVX2 static inline __m256i chacha_rol12_ymm(__m256i value)
{
    return _mm256_or_si256(_mm256_slli_epi32(value, 12),
                           _mm256_srli_epi32(value, 20));
}

TARGET_AVX2 static inline __m256i chacha_rol7_ymm(__m256i value)
{
    return _mm256_or_si256(_mm256_slli_epi32(value, 7),
                           _mm256_srli_epi32(value, 25));
}

#define CHACHA_QR_YMM(a, b, c, d) do { \
    (a) = _mm256_add_epi32((a), (b)); \
    (d) = chacha_rol16_ymm(_mm256_xor_si256((d), (a))); \
    (c) = _mm256_add_epi32((c), (d)); \
    (b) = chacha_rol12_ymm(_mm256_xor_si256((b), (c))); \
    (a) = _mm256_add_epi32((a), (b)); \
    (d) = chacha_rol8_ymm(_mm256_xor_si256((d), (a))); \
    (c) = _mm256_add_epi32((c), (d)); \
    (b) = chacha_rol7_ymm(_mm256_xor_si256((b), (c))); \
} while (0)

TARGET_AVX2 NOINLINE static double chacha_ymm(void)
{
    const __m256i C0 = _mm256_set1_epi32((int)UINT32_C(0x61707865));
    const __m256i C1 = _mm256_set1_epi32((int)UINT32_C(0x3320646e));
    const __m256i C2 = _mm256_set1_epi32((int)UINT32_C(0x79622d32));
    const __m256i C3 = _mm256_set1_epi32((int)UINT32_C(0x6b206574));
    const __m256i K0 = _mm256_set1_epi32((int)UINT32_C(0x03020100));
    const __m256i K1 = _mm256_set1_epi32((int)UINT32_C(0x07060504));
    const __m256i K2 = _mm256_set1_epi32((int)UINT32_C(0x0b0a0908));
    const __m256i K3 = _mm256_set1_epi32((int)UINT32_C(0x0f0e0d0c));
    const __m256i K4 = _mm256_set1_epi32((int)UINT32_C(0x13121110));
    const __m256i K5 = _mm256_set1_epi32((int)UINT32_C(0x17161514));
    const __m256i K6 = _mm256_set1_epi32((int)UINT32_C(0x1b1a1918));
    const __m256i K7 = _mm256_set1_epi32((int)UINT32_C(0x1f1e1d1c));
    const __m256i N0 = _mm256_set1_epi32((int)UINT32_C(0x09000000));
    const __m256i N1 = _mm256_set1_epi32((int)UINT32_C(0x4a000000));
    const __m256i N2 = _mm256_setzero_si256();
    __m256i counter = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const __m256i step = _mm256_set1_epi32(8);
    __m256i checksum = _mm256_setzero_si256();

    for (size_t block = 0;
         block < CHACHA_PASSES * CHACHA_BLOCKS / 8u; ++block) {
        __m256i x0 = C0, x1 = C1, x2 = C2, x3 = C3;
        __m256i x4 = K0, x5 = K1, x6 = K2, x7 = K3;
        __m256i x8 = K4, x9 = K5, x10 = K6, x11 = K7;
        __m256i x12 = counter, x13 = N0, x14 = N1, x15 = N2;
        for (int round = 0; round < 10; ++round) {
            CHACHA_QR_YMM(x0, x4, x8, x12);
            CHACHA_QR_YMM(x1, x5, x9, x13);
            CHACHA_QR_YMM(x2, x6, x10, x14);
            CHACHA_QR_YMM(x3, x7, x11, x15);
            CHACHA_QR_YMM(x0, x5, x10, x15);
            CHACHA_QR_YMM(x1, x6, x11, x12);
            CHACHA_QR_YMM(x2, x7, x8, x13);
            CHACHA_QR_YMM(x3, x4, x9, x14);
        }
        x0 = _mm256_add_epi32(x0, C0);
        x1 = _mm256_add_epi32(x1, C1);
        x2 = _mm256_add_epi32(x2, C2);
        x3 = _mm256_add_epi32(x3, C3);
        x4 = _mm256_add_epi32(x4, K0);
        x5 = _mm256_add_epi32(x5, K1);
        x6 = _mm256_add_epi32(x6, K2);
        x7 = _mm256_add_epi32(x7, K3);
        x8 = _mm256_add_epi32(x8, K4);
        x9 = _mm256_add_epi32(x9, K5);
        x10 = _mm256_add_epi32(x10, K6);
        x11 = _mm256_add_epi32(x11, K7);
        x12 = _mm256_add_epi32(x12, counter);
        x13 = _mm256_add_epi32(x13, N0);
        x14 = _mm256_add_epi32(x14, N1);
        x15 = _mm256_add_epi32(x15, N2);
#define ADD_CHECKSUM_YMM(v) checksum = _mm256_add_epi32(checksum, (v))
        ADD_CHECKSUM_YMM(x0);  ADD_CHECKSUM_YMM(x1);
        ADD_CHECKSUM_YMM(x2);  ADD_CHECKSUM_YMM(x3);
        ADD_CHECKSUM_YMM(x4);  ADD_CHECKSUM_YMM(x5);
        ADD_CHECKSUM_YMM(x6);  ADD_CHECKSUM_YMM(x7);
        ADD_CHECKSUM_YMM(x8);  ADD_CHECKSUM_YMM(x9);
        ADD_CHECKSUM_YMM(x10); ADD_CHECKSUM_YMM(x11);
        ADD_CHECKSUM_YMM(x12); ADD_CHECKSUM_YMM(x13);
        ADD_CHECKSUM_YMM(x14); ADD_CHECKSUM_YMM(x15);
#undef ADD_CHECKSUM_YMM
        counter = _mm256_add_epi32(counter, step);
    }
    alignas(32) uint32_t lanes[8];
    _mm256_store_si256((__m256i *)lanes, checksum);
    uint32_t sum = 0;
    for (int lane = 0; lane < 8; ++lane) sum += lanes[lane];
    COMPILER_BARRIER();
    return (double)u32_as_i32(sum);
}

#undef CHACHA_QR_XMM
#undef CHACHA_QR_YMM

static int parse_reps(int argc, char **argv)
{
    if (argc < 2) {
        return 5;
    }

    char *end = NULL;
    const long value = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || value <= 0 || value > INT32_MAX) {
        fprintf(stderr, "usage: %s [positive-reps]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    return (int)value;
}

static void run_kernel(const char *name,
                       kernel_fn scalar_fn,
                       kernel_fn xmm_fn,
                       kernel_fn ymm_fn,
                       int reps,
                       bool has_ymm,
                       double tolerance)
{
    const bench_result scalar = bench(scalar_fn, reps);
    const bench_result xmm = bench(xmm_fn, reps);
    if (has_ymm) {
        const bench_result ymm = bench(ymm_fn, reps);
        report_ymm(name, scalar, xmm, ymm, tolerance);
    } else {
        report_xmm(name, scalar, xmm, tolerance);
    }
}

int main(int argc, char **argv)
{
    const int reps = parse_reps(argc, argv);

    __builtin_cpu_init();
    const bool has_sse41 = __builtin_cpu_supports("sse4.1") != 0;
    const bool has_ymm = __builtin_cpu_supports("avx2") != 0;

    if (!has_sse41) {
        fputs("simdtest requires SSE4.1 for its XMM integer-max kernel\n", stderr);
        return EXIT_FAILURE;
    }

    initialize_data(has_ymm);

    printf("N=%u, %d passes, best of %d\n", N, PASSES, reps);
    printf("runtime SIMD: SSE4.1=yes, AVX2=%s\n", has_ymm ? "yes" : "no");

    run_kernel("saxpy (float)", saxpy_scalar, saxpy_xmm, saxpy_ymm,
               reps, has_ymm, 0.0);
    run_kernel("dot product (float)", dot_scalar, dot_xmm, dot_ymm,
               reps, has_ymm, -1.0);
    run_kernel("horizontal max (int32)", max_scalar, max_xmm, max_ymm,
               reps, has_ymm, 0.0);
    run_kernel("clamp (float)", clamp_scalar, clamp_xmm, clamp_ymm,
               reps, has_ymm, 0.0);
    run_kernel("delta encode (int32)", delta_scalar, delta_xmm, delta_ymm,
               reps, has_ymm, 0.0);

    printf("\nHeavy kernels: FIR %ux%d, polynomial %ux%d, Mandelbrot %ux%d\n",
           FIR_N, FIR_PASSES,
           POLY_N, POLY_PASSES,
           MANDEL_N, MANDEL_PASSES);

    run_kernel("8-tap FIR (float)", fir_scalar, fir_xmm, fir_ymm,
               reps, has_ymm, 0.0);
    run_kernel("degree-11 poly (double)", poly_scalar, poly_xmm, poly_ymm,
               reps, has_ymm, 0.0);
    run_kernel("Mandelbrot 64-it (double)",
               mandel_scalar, mandel_xmm, mandel_ymm,
               reps, has_ymm, 0.0);

    printf(
        "\nReal-world kernels: 1080p RGBA merge + blur, 4K luma sums + "
        "depth + dilation + %.1fMP residual codebook/signed quantization, "
        "%.0f MiB checksums + %.0f MiB block SAD, %.0f MiB INT8 ranges + "
        "%.0f MiB ternary dots, %.1fM weighted points + %.1fM voxelized "
        "points, %us PCM envelope, %us float gate, %.1fs fixed FIR, "
        "%.2fs float FIR, %u particles, %u ChaCha blocks\n",
        (double)RESID_N / 1000000.0,
        (double)CHECKSUM_N / (1024.0 * 1024.0),
        (double)SAD_N / (1024.0 * 1024.0),
        (double)ACT_N / (1024.0 * 1024.0),
        (double)TERNARY_N / (1024.0 * 1024.0),
        (double)XFORM_N / 1000000.0,
        (double)(VOXEL_N * VOXEL_PASSES) / 1000000.0,
        PCM_SECONDS, GATE_SECONDS,
        (double)PCM_FIR_SAMPLES / 48000.0,
        (double)AUDIO_N / 48000.0,
        PARTICLE_N, CHACHA_BLOCKS * CHACHA_PASSES);

    run_kernel("RGBA channel merge", rgba_scalar, rgba_xmm, rgba_ymm,
               reps, has_ymm, 0.0);
    run_kernel("32-byte block checksum",
               checksum_scalar, checksum_xmm, checksum_ymm,
               reps, has_ymm, 0.0);
    run_kernel("32-byte block SAD", sad_scalar, sad_xmm, sad_ymm,
               reps, has_ymm, 0.0);
    run_kernel("4K 16-pixel luma sum", luma_scalar, luma_xmm, luma_ymm,
               reps, has_ymm, 0.0);
    run_kernel("4K depth tile extrema", depth_scalar, depth_xmm, depth_ymm,
               reps, has_ymm, 0.0);
    run_kernel("PCM16 peak envelope", pcm_scalar, pcm_xmm, pcm_ymm,
               reps, has_ymm, 0.0);
    run_kernel("float PCM noise gate", gate_scalar, gate_xmm, gate_ymm,
               reps, has_ymm, 0.0);
    run_kernel("PCM16 16-tap decimator",
               pcm_fir_scalar, pcm_fir_xmm, pcm_fir_ymm,
               reps, has_ymm, 0.0);
    run_kernel("4K residual codebook", resid_scalar, resid_xmm, resid_ymm,
               reps, has_ymm, 0.0);
    run_kernel("4K four-rate quantize",
               resid_signed_scalar, resid_signed_xmm, resid_signed_ymm,
               reps, has_ymm, 0.0);
    run_kernel("INT8 activation range", act_scalar, act_xmm, act_ymm,
               reps, has_ymm, 0.0);
    run_kernel("INT8 32-tap ternary filter",
               ternary_scalar, ternary_xmm, ternary_ymm,
               reps, has_ymm, 0.0);
    run_kernel("5x5 Gaussian (1080p)", blur_scalar, blur_xmm, blur_ymm,
               reps, has_ymm, 2e-5);
    run_kernel("3x3 dilation (4K)", morph_scalar, morph_xmm, morph_ymm,
               reps, has_ymm, 0.0);
    run_kernel("weighted 4x4 point transform",
               xform_scalar, xform_xmm, xform_ymm,
               reps, has_ymm, 2e-5);
    run_kernel("3D voxel index checksum", voxel_scalar, voxel_xmm, voxel_ymm,
               reps, has_ymm, 0.0);
    run_kernel("64-tap audio FIR", audio_scalar, audio_xmm, audio_ymm,
               reps, has_ymm, 5e-5);
    run_kernel("gravity particles x32",
               particle_scalar, particle_xmm, particle_ymm,
               reps, has_ymm, 2e-4);
    run_kernel("ChaCha20 block core", chacha_scalar, chacha_xmm, chacha_ymm,
               reps, has_ymm, 0.0);

    if (failures != 0) {
        printf("\n%d benchmark(s) produced a wrong result\n", failures);
        return EXIT_FAILURE;
    }
    return result_sink == -DBL_MAX ? EXIT_FAILURE : EXIT_SUCCESS;
}
