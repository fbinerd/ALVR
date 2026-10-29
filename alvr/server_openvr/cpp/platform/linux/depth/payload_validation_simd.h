#ifndef MHL_DEPTH_PAYLOAD_VALIDATION_SIMD_H
#define MHL_DEPTH_PAYLOAD_VALIDATION_SIMD_H
/* Exact unsigned IEEE-754 bit validation, with no FP operations. The caller
 * validates row bounds first; vector loads never extend into row padding. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static inline int mhl_depth_invalid_row_scalar(const unsigned char *row,uint32_t count) {
    for(uint32_t x=0;x<count;x++){
        uint32_t bits;memcpy(&bits,row+(size_t)x*4,4);
        if(bits>=UINT32_C(0x7f800000))return 1;
    }
    return 0;
}
#if defined(__SSE2__) && !defined(MHL_DEPTH_SCALAR_ONLY)
#include <immintrin.h>
static inline int mhl_depth_invalid_row_sse2(const unsigned char *row,uint32_t count) {
    const __m128i sign=_mm_set1_epi32(-2147483647-1),limit=_mm_set1_epi32(-8388609);
    uint32_t x=0;
    for(;count-x>=4;x+=4){
        __m128i bits=_mm_loadu_si128((const __m128i *)(row+(size_t)x*4));
        if(_mm_movemask_epi8(_mm_cmpgt_epi32(_mm_xor_si128(bits,sign),limit)))return 1;
    }
    return mhl_depth_invalid_row_scalar(row+(size_t)x*4,count-x);
}
#if defined(__GNUC__) && !defined(MHL_DEPTH_SSE2_ONLY)
__attribute__((target("avx2")))
static int mhl_depth_invalid_row_avx2(const unsigned char *row,uint32_t count) {
    const __m256i sign=_mm256_set1_epi32(-2147483647-1),limit=_mm256_set1_epi32(-8388609);
    uint32_t x=0;
    for(;count-x>=8;x+=8){
        __m256i bits=_mm256_loadu_si256((const __m256i *)(row+(size_t)x*4));
        if(_mm256_movemask_epi8(_mm256_cmpgt_epi32(_mm256_xor_si256(bits,sign),limit)))return 1;
    }
    return mhl_depth_invalid_row_sse2(row+(size_t)x*4,count-x);
}
#endif
#endif
static inline int mhl_depth_invalid_row(const unsigned char *row,uint32_t count) {
#if defined(__SSE2__) && !defined(MHL_DEPTH_SCALAR_ONLY)
#if defined(__GNUC__) && !defined(MHL_DEPTH_SSE2_ONLY)
    if(__builtin_cpu_supports("avx2"))return mhl_depth_invalid_row_avx2(row,count);
#endif
    return mhl_depth_invalid_row_sse2(row,count);
#else
    return mhl_depth_invalid_row_scalar(row,count);
#endif
}
#endif
