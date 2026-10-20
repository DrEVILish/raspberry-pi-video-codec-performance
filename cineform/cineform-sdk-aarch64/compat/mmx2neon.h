/* MMX (64-bit __m64) intrinsics the CineForm SDK uses and sse2neon does not provide, on NEON 64-bit vectors.
 * __m64 is sse2neon's int64x1_t. Semantics follow Intel's definitions (saturation, shift counts past the lane
 * width giving 0, or the sign for arithmetic shifts). MIT/Apache-2.0, as the SDK. */
#pragma once
#include <arm_neon.h>
#include <string.h>

#define M64(x) vreinterpret_s64_s16(x)
#define S16(x) vreinterpret_s16_s64(x)
#define U16(x) vreinterpret_u16_s64(x)
#define S32(x) vreinterpret_s32_s64(x)
#define U32(x) vreinterpret_u32_s64(x)
#define S8(x) vreinterpret_s8_s64(x)
#define U8(x) vreinterpret_u8_s64(x)
#define MI static inline __m64 __attribute__((always_inline))

MI _mm_setzero_si64(void) { return vdup_n_s64(0); }
MI _mm_cvtsi32_si64(int i) { return vreinterpret_s64_u64(vdup_n_u64((uint32_t)i)); }
MI _mm_set1_pi8(char v) { return vreinterpret_s64_s8(vdup_n_s8(v)); }
MI _mm_set1_pi16(short v) { return M64(vdup_n_s16(v)); }
MI _mm_set1_pi32(int v) { return vreinterpret_s64_s32(vdup_n_s32(v)); }
MI _mm_set_pi16(short e3, short e2, short e1, short e0) { int16_t t[4] = { e0, e1, e2, e3 }; return M64(vld1_s16(t)); }
MI _mm_set_pi8(char e7, char e6, char e5, char e4, char e3, char e2, char e1, char e0) {
	int8_t t[8] = { e0, e1, e2, e3, e4, e5, e6, e7 }; return vreinterpret_s64_s8(vld1_s8(t)); }
MI _mm_setr_pi8(char e0, char e1, char e2, char e3, char e4, char e5, char e6, char e7) {
	int8_t t[8] = { e0, e1, e2, e3, e4, e5, e6, e7 }; return vreinterpret_s64_s8(vld1_s8(t)); }
/* Non-standard helpers the SDK's temporal transform uses on aligned __m64 pointers. */
MI _mm_load_si64(const __m64 *p) { return *p; }
static inline void _mm_store_si64(__m64 *p, __m64 v) { *p = v; }

MI _mm_and_si64(__m64 a, __m64 b) { return vand_s64(a, b); }
MI _mm_or_si64(__m64 a, __m64 b) { return vorr_s64(a, b); }
MI _mm_xor_si64(__m64 a, __m64 b) { return veor_s64(a, b); }
MI _mm_andnot_si64(__m64 a, __m64 b) { return vbic_s64(b, a); }

MI _mm_add_pi16(__m64 a, __m64 b) { return M64(vadd_s16(S16(a), S16(b))); }
MI _mm_add_pi32(__m64 a, __m64 b) { return vreinterpret_s64_s32(vadd_s32(S32(a), S32(b))); }
MI _mm_adds_pi16(__m64 a, __m64 b) { return M64(vqadd_s16(S16(a), S16(b))); }
MI _mm_adds_pu16(__m64 a, __m64 b) { return vreinterpret_s64_u16(vqadd_u16(U16(a), U16(b))); }
MI _mm_adds_pu8(__m64 a, __m64 b) { return vreinterpret_s64_u8(vqadd_u8(U8(a), U8(b))); }
MI _mm_sub_pi8(__m64 a, __m64 b) { return vreinterpret_s64_s8(vsub_s8(S8(a), S8(b))); }
MI _mm_sub_pi16(__m64 a, __m64 b) { return M64(vsub_s16(S16(a), S16(b))); }
MI _mm_sub_pi32(__m64 a, __m64 b) { return vreinterpret_s64_s32(vsub_s32(S32(a), S32(b))); }
MI _mm_subs_pi8(__m64 a, __m64 b) { return vreinterpret_s64_s8(vqsub_s8(S8(a), S8(b))); }
MI _mm_subs_pi16(__m64 a, __m64 b) { return M64(vqsub_s16(S16(a), S16(b))); }
MI _mm_subs_pu8(__m64 a, __m64 b) { return vreinterpret_s64_u8(vqsub_u8(U8(a), U8(b))); }
MI _mm_subs_pu16(__m64 a, __m64 b) { return vreinterpret_s64_u16(vqsub_u16(U16(a), U16(b))); }
MI _mm_mullo_pi16(__m64 a, __m64 b) { return M64(vmul_s16(S16(a), S16(b))); }
MI _mm_mulhi_pi16(__m64 a, __m64 b) { return M64(vshrn_n_s32(vmull_s16(S16(a), S16(b)), 16)); }

MI _mm_cmpeq_pi8(__m64 a, __m64 b) { return vreinterpret_s64_u8(vceq_s8(S8(a), S8(b))); }
MI _mm_cmpeq_pi16(__m64 a, __m64 b) { return vreinterpret_s64_u16(vceq_s16(S16(a), S16(b))); }
MI _mm_cmpgt_pi8(__m64 a, __m64 b) { return vreinterpret_s64_u8(vcgt_s8(S8(a), S8(b))); }
MI _mm_cmpgt_pi16(__m64 a, __m64 b) { return vreinterpret_s64_u16(vcgt_s16(S16(a), S16(b))); }

MI _mm_packs_pi16(__m64 a, __m64 b) { return vreinterpret_s64_s8(vqmovn_s16(vcombine_s16(S16(a), S16(b)))); }
MI _mm_packs_pi32(__m64 a, __m64 b) { return M64(vqmovn_s32(vcombine_s32(S32(a), S32(b)))); }
MI _mm_packs_pu16(__m64 a, __m64 b) { return vreinterpret_s64_u8(vqmovun_s16(vcombine_s16(S16(a), S16(b)))); }

MI _mm_unpacklo_pi8(__m64 a, __m64 b) { return vreinterpret_s64_s8(vzip1_s8(S8(a), S8(b))); }
MI _mm_unpackhi_pi8(__m64 a, __m64 b) { return vreinterpret_s64_s8(vzip2_s8(S8(a), S8(b))); }
MI _mm_unpacklo_pi16(__m64 a, __m64 b) { return M64(vzip1_s16(S16(a), S16(b))); }
MI _mm_unpackhi_pi16(__m64 a, __m64 b) { return M64(vzip2_s16(S16(a), S16(b))); }
MI _mm_unpacklo_pi32(__m64 a, __m64 b) { return vreinterpret_s64_s32(vzip1_s32(S32(a), S32(b))); }
MI _mm_unpackhi_pi32(__m64 a, __m64 b) { return vreinterpret_s64_s32(vzip2_s32(S32(a), S32(b))); }

/* Shifts: counts past the lane width give 0 (logical) or the sign (arithmetic), as on x86. */
MI _mm_slli_pi16(__m64 a, int n) { return n > 15 ? vdup_n_s64(0) : M64(vshl_s16(S16(a), vdup_n_s16(n))); }
MI _mm_slli_pi32(__m64 a, int n) { return n > 31 ? vdup_n_s64(0) : vreinterpret_s64_s32(vshl_s32(S32(a), vdup_n_s32(n))); }
MI _mm_slli_si64(__m64 a, int n) { return n > 63 ? vdup_n_s64(0) : vreinterpret_s64_u64(vshl_u64(vreinterpret_u64_s64(a), vdup_n_s64(n))); }
MI _mm_srli_pi16(__m64 a, int n) { return n > 15 ? vdup_n_s64(0) : vreinterpret_s64_u16(vshl_u16(U16(a), vdup_n_s16(-n))); }
MI _mm_srli_pi32(__m64 a, int n) { return n > 31 ? vdup_n_s64(0) : vreinterpret_s64_u32(vshl_u32(U32(a), vdup_n_s32(-n))); }
MI _mm_srli_si64(__m64 a, int n) { return n > 63 ? vdup_n_s64(0) : vreinterpret_s64_u64(vshl_u64(vreinterpret_u64_s64(a), vdup_n_s64(-n))); }
MI _mm_srai_pi16(__m64 a, int n) { return M64(vshl_s16(S16(a), vdup_n_s16(-(n > 15 ? 15 : n)))); }
MI _mm_srai_pi32(__m64 a, int n) { return vreinterpret_s64_s32(vshl_s32(S32(a), vdup_n_s32(-(n > 31 ? 31 : n)))); }
MI _mm_sra_pi16(__m64 a, __m64 c) { uint64_t n = vget_lane_u64(vreinterpret_u64_s64(c), 0); return _mm_srai_pi16(a, n > 15 ? 15 : (int)n); }
