// probe.c — find out which SIMD extensions *execute* on this machine (not what CPUID says)
// and what the operations a SIMD compiler cares about cost.
// build: gcc -O2 -o probe probe.c   (no -m flags: every probe is hand-encoded via target attributes)
#define _GNU_SOURCE
#include <immintrin.h>
#include <cpuid.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>

static sigjmp_buf jb;
static void on_sigill(int s) { siglongjmp(jb, 1); }

#define T(x) __attribute__((target(x), noinline))
static volatile uint64_t sink;
static volatile int three=3;
#define OPAQ(v) __asm__ volatile(""::"x"(v):)


// Four probes are written in assembly: the compiler had turned their intrinsics into something else
// (scalar adds, plain adc). hw/check_probe.py checks every probe against its disassembly.
T("sse2") static void p_sse2(void){ __asm__ volatile("paddd %%xmm0,%%xmm0":::"xmm0"); sink=three; }
T("ssse3") static void p_ssse3(void){ __m128i a=_mm_set1_epi8(three); sink=_mm_cvtsi128_si32(_mm_shuffle_epi8(a,a)); }
T("sse4.1") static void p_sse41(void){ __m128i a=_mm_set1_epi32(three); sink=_mm_extract_epi32(_mm_mullo_epi32(a,a),1); }
T("sse4.2") static void p_sse42(void){ sink=_mm_crc32_u64(1,2); }
T("popcnt") static void p_popcnt(void){ sink=_mm_popcnt_u64(sink|5); }
T("pclmul") static void p_pclmul(void){ __m128i a=_mm_set1_epi32(three); sink=_mm_cvtsi128_si32(_mm_clmulepi64_si128(a,a,0)); }
T("aes") static void p_aes(void){ __m128i a=_mm_set1_epi32(three); sink=_mm_cvtsi128_si32(_mm_aesenc_si128(a,a)); }
T("sha") static void p_sha(void){ __m128i a=_mm_set1_epi32(three); sink=_mm_cvtsi128_si32(_mm_sha1msg1_epu32(a,a)); }
T("avx") static void p_avx(void){ __asm__ volatile("vaddps %%ymm0,%%ymm0,%%ymm0\n vzeroupper":::"xmm0"); sink=three; }
T("avx2") static void p_avx2(void){ __asm__ volatile("vpaddd %%ymm0,%%ymm0,%%ymm0\n vzeroupper":::"xmm0"); sink=three; }
T("fma") static void p_fma(void){ __m256 a=_mm256_set1_ps(three); sink=(uint64_t)_mm256_cvtss_f32(_mm256_fmadd_ps(a,a,a)); }
T("f16c") static void p_f16c(void){ __m128 a=_mm_set1_ps(three); sink=_mm_cvtsi128_si32(_mm_cvtps_ph(a,0)); }
T("bmi") static void p_bmi1(void){ sink=_tzcnt_u64(sink|8)+_blsr_u64(sink|3); }
T("bmi2") static void p_bmi2(void){ sink=_pext_u64(sink|0xff,0xf0f0)+_pdep_u64(sink,0xf0f0)+_bzhi_u64(sink,5); }
T("lzcnt") static void p_lzcnt(void){ sink=_lzcnt_u64(sink|1); }
T("adx") static void p_adx(void){ __asm__ volatile("adcx %%rax,%%rax\n adox %%rax,%%rax":::"rax","cc"); sink=three; }
T("movbe") static void p_movbe(void){ uint64_t v=sink; sink=__builtin_bswap64(v); __asm__ volatile("movbe %0,%%rax"::"m"(v):"rax"); }
T("avx512f") static void p_512f(void){ __m512i a=_mm512_set1_epi32(three); sink=_mm512_reduce_add_epi32(_mm512_add_epi32(a,a)); }
T("avx512f") static void p_512f_k(void){ __m512i a=_mm512_set1_epi32(three); __mmask16 k=_mm512_cmpeq_epi32_mask(a,a); sink=k; }
T("avx512f") static void p_512f_compress(void){ __m512i a=_mm512_set1_epi32(three); sink=_mm512_reduce_add_epi32(_mm512_maskz_compress_epi32(0x5555,a)); }
T("avx512f") static void p_512f_gather(void){ static int t[64]; __m512i i=_mm512_set1_epi32(three); sink=_mm512_reduce_add_epi32(_mm512_i32gather_epi32(i,t,4)); }
T("avx512f") static void p_512f_scatter(void){ static int t[64]; __m512i i=_mm512_setr_epi32(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15); _mm512_i32scatter_epi32(t,i,i,4); sink=t[5]; }
T("avx512f,avx512bw,avx512dq,avx512vl,avx512dq") static void p_512dq(void){ __m512i a=_mm512_set1_epi64(three); __mmask16 k=_mm512_movepi32_mask(a); sink=_mm512_reduce_add_epi64(_mm512_mullo_epi64(a,a))+k; }
T("avx512f,avx512bw,avx512dq,avx512vl,avx512bw") static void p_512bw(void){ __m512i a=_mm512_set1_epi8(three); sink=_mm512_cmpeq_epi8_mask(_mm512_add_epi8(a,a),a); }
T("avx512f,avx512bw,avx512dq,avx512vl") static void p_512vl(void){ __m256i a=_mm256_set1_epi32(three); sink=_mm256_cmpeq_epi32_mask(a,a); }
T("avx512f,avx512bw,avx512dq,avx512vl,avx512cd") static void p_512cd(void){ __m512i a=_mm512_set1_epi32(three); sink=_mm512_reduce_add_epi32(_mm512_conflict_epi32(a))+_mm512_reduce_add_epi32(_mm512_lzcnt_epi32(a)); }
T("avx512f,avx512bw,avx512dq,avx512vl,avx512ifma") static void p_512ifma(void){ __m512i a=_mm512_set1_epi64(three); sink=_mm512_reduce_add_epi64(_mm512_madd52lo_epu64(a,a,a)); }
T("avx512f,avx512bw,avx512dq,avx512vl,avx512vbmi") static void p_512vbmi(void){ __m512i a=_mm512_set1_epi8(three); sink=_mm512_cmpeq_epi8_mask(_mm512_permutexvar_epi8(a,a),a); }
T("avx512f,avx512bw,avx512dq,avx512vl,avx512vbmi2") static void p_512vbmi2(void){ __m512i a=_mm512_set1_epi8(three); sink=_mm512_cmpeq_epi8_mask(_mm512_maskz_compress_epi8(0x5555555555555555ull,a),a); }
T("avx512f,avx512bw,avx512dq,avx512vl,avx512vnni") static void p_512vnni(void){ __m512i a=_mm512_set1_epi8(three); sink=_mm512_reduce_add_epi32(_mm512_dpbusd_epi32(a,a,a)); }
T("avx512f,avx512bw,avx512dq,avx512vl,avx512bitalg") static void p_512bitalg(void){ __m512i a=_mm512_set1_epi8(three); sink=_mm512_cmpeq_epi8_mask(_mm512_popcnt_epi8(a),a); }
T("avx512f,avx512bw,avx512dq,avx512vl,avx512vpopcntdq") static void p_512vpopcnt(void){ __m512i a=_mm512_set1_epi32(three); sink=_mm512_reduce_add_epi32(_mm512_popcnt_epi32(a)); }
T("avx512f,avx512bw,avx512dq,avx512vl,avx512bf16") static void p_512bf16(void){ __m512 a=_mm512_set1_ps(three); __m512bh b=_mm512_cvtne2ps_pbh(a,a); sink=_mm512_reduce_add_epi32((__m512i)b); }
T("gfni,avx512f,avx512bw,avx512dq,avx512vl") static void p_gfni(void){ __m512i a=_mm512_set1_epi8(three); sink=_mm512_cmpeq_epi8_mask(_mm512_gf2p8affine_epi64_epi8(a,a,0),a); }
T("vaes,avx512f,avx512bw,avx512dq,avx512vl") static void p_vaes(void){ __m512i a=_mm512_set1_epi8(three); sink=_mm512_reduce_add_epi32(_mm512_aesenc_epi128(a,a)); }
T("vpclmulqdq,avx512f,avx512bw,avx512dq,avx512vl") static void p_vpclmul(void){ __m512i a=_mm512_set1_epi8(three); sink=_mm512_reduce_add_epi64(_mm512_clmulepi64_epi128(a,a,0)); }
// things CPUID here does NOT report — expect SIGILL
T("avx512fp16,avx512f,avx512bw,avx512dq,avx512vl") static void p_fp16(void){ __m512h a=_mm512_set1_ph(three); sink=(uint64_t)_mm512_reduce_add_ph(_mm512_add_ph(a,a)); }
T("avx512vp2intersect,avx512f,avx512bw,avx512dq,avx512vl") static void p_vp2i(void){ __m512i a=_mm512_set1_epi32(three); __mmask16 x,y; _mm512_2intersect_epi32(a,a,&x,&y); sink=x+y; }
T("avxvnni") static void p_avxvnni(void){ __m256i a=_mm256_set1_epi8(three); sink=_mm256_extract_epi32(_mm256_dpbusd_avx_epi32(a,a,a),0); }
static void p_sse4a(void){ __asm__ volatile("movq %%rax,%%xmm0; .byte 0x66,0x0f,0x78,0xc0,0x08,0x00"/*extrq $0,$8,%xmm0*/:::"xmm0"); }
static void p_amx(void){ __asm__ volatile(".byte 0xc4,0xe2,0x78,0x49,0xc0" /*tilerelease*/); }
static void p_apx(void){ __asm__ volatile(".byte 0xd5,0x10,0x89,0xc0" /*REX2 mov*/:::"rax"); }

struct { const char *name; void (*f)(void); const char *cpuflag; } probes[] = {
 {"SSE2",p_sse2,"sse2"},{"SSSE3",p_ssse3,"ssse3"},{"SSE4.1",p_sse41,"sse4_1"},{"SSE4.2 (crc32)",p_sse42,"sse4_2"},
 {"SSE4a (extrq)",p_sse4a,"sse4a"},{"POPCNT",p_popcnt,"popcnt"},{"LZCNT",p_lzcnt,"abm"},{"BMI1",p_bmi1,"bmi1"},{"BMI2 (pext/pdep/bzhi)",p_bmi2,"bmi2"},
 {"ADX",p_adx,"adx"},{"MOVBE",p_movbe,"movbe"},{"PCLMULQDQ",p_pclmul,"pclmulqdq"},{"AES-NI",p_aes,"aes"},{"SHA-NI",p_sha,"sha_ni"},
 {"AVX",p_avx,"avx"},{"AVX2",p_avx2,"avx2"},{"FMA3",p_fma,"fma"},{"F16C",p_f16c,"f16c"},
 {"AVX-512F (zmm add)",p_512f,"avx512f"},{"AVX-512F (k-mask compare)",p_512f_k,"avx512f"},{"AVX-512F vpcompressd",p_512f_compress,"avx512f"},
 {"AVX-512F vpgatherdd",p_512f_gather,"avx512f"},{"AVX-512F vpscatterdd",p_512f_scatter,"avx512f"},
 {"AVX-512DQ",p_512dq,"avx512dq"},{"AVX-512BW",p_512bw,"avx512bw"},{"AVX-512VL",p_512vl,"avx512vl"},{"AVX-512CD (vpconflictd, vplzcntd)",p_512cd,"avx512cd"},
 {"AVX-512IFMA",p_512ifma,"avx512ifma"},{"AVX-512VBMI (vpermb)",p_512vbmi,"avx512vbmi"},{"AVX-512VBMI2 (vpcompressb)",p_512vbmi2,"avx512_vbmi2"},
 {"AVX-512VNNI",p_512vnni,"avx512_vnni"},{"AVX-512BITALG",p_512bitalg,"avx512_bitalg"},{"AVX-512VPOPCNTDQ",p_512vpopcnt,"avx512_vpopcntdq"},
 {"AVX-512BF16",p_512bf16,"avx512_bf16"},{"GFNI (zmm)",p_gfni,"gfni"},{"VAES (zmm)",p_vaes,"vaes"},{"VPCLMULQDQ (zmm)",p_vpclmul,"vpclmulqdq"},
 {"AVX-512FP16",p_fp16,"avx512_fp16"},{"AVX-512VP2INTERSECT",p_vp2i,"avx512_vp2intersect"},{"AVX-VNNI (VEX)",p_avxvnni,"avx_vnni"},
 {"AMX (tilerelease)",p_amx,"amx_tile"},{"APX (REX2)",p_apx,"apx_f"},
};

static char cpuflags[8192];
static int has_flag(const char *f){ char pat[64]; snprintf(pat,sizeof pat," %s ",f); return strstr(cpuflags,pat)!=NULL; }

// ---------- timing ----------
static inline uint64_t now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000000000ull+t.tv_nsec; }
static double tsc_per_ns;
#define N (1<<10)           // 1024 dwords = 4 KiB per array: three arrays fit the 32 KiB L1d
#define REPS 80000
static int32_t POOL[32*N] __attribute__((aligned(4096)));
static int32_t *A,*B,*C;   // set in main: page offsets 0, 1344, 2688 bytes apart mod 4096
static uint8_t BA[N*4+64] __attribute__((aligned(64))), BB[N*4+64] __attribute__((aligned(64)));
static int32_t BIG[1<<24] __attribute__((aligned(64)));  // 64 MiB

#define BENCH(label, unit_count, body) do { \
  double best=1e30; for(int tr=0;tr<7;tr++){ uint64_t t0=__rdtsc(); for(int r=0;r<REPS;r++){ body; __asm__ volatile(""::"m"(C[0]):"memory"); } uint64_t t1=__rdtsc(); double c=(double)(t1-t0)/REPS/(unit_count); if(c<best)best=c; } \
  printf("| %-58s | %7.2f | %7.3f |\n", label, best, best/tsc_per_ns); } while(0)

#define A512 T("avx512f,avx512bw,avx512dq,avx512vl,avx512cd,avx512vbmi,avx512vbmi2,avx512bitalg,avx512vpopcntdq,bmi2,popcnt,gfni,vpclmulqdq,pclmul")

A512 static void k_add512(void){ for(int i=0;i<N;i+=16) _mm512_store_si512(C+i,_mm512_add_epi32(_mm512_load_si512(A+i),_mm512_load_si512(B+i))); }
A512 static void k_add256(void){ for(int i=0;i<N;i+=8) _mm256_store_si256((void*)(C+i),_mm256_add_epi32(_mm256_load_si256((void*)(A+i)),_mm256_load_si256((void*)(B+i)))); }
static void __attribute__((noinline,optimize("no-tree-vectorize"))) k_addscalar(void){ for(int i=0;i<N;i++) C[i]=A[i]+B[i]; }
A512 static void k_mul512(void){ for(int i=0;i<N;i+=16) _mm512_store_si512(C+i,_mm512_mullo_epi32(_mm512_load_si512(A+i),_mm512_load_si512(B+i))); }
A512 static void k_cmpblend(void){ for(int i=0;i<N;i+=16){ __m512i a=_mm512_load_si512(A+i),b=_mm512_load_si512(B+i); _mm512_store_si512(C+i,_mm512_mask_blend_epi32(_mm512_cmplt_epi32_mask(a,b),a,b)); } }
A512 static void k_compress_reg(void){ int32_t*o=C; for(int i=0;i<N;i+=16){ __m512i a=_mm512_load_si512(A+i); __mmask16 k=_mm512_test_epi32_mask(a,_mm512_set1_epi32(1)); _mm512_storeu_si512(o,_mm512_maskz_compress_epi32(k,a)); o+=_mm_popcnt_u32(k);} }
A512 static void k_compress_mem(void){ int32_t*o=C; for(int i=0;i<N;i+=16){ __m512i a=_mm512_load_si512(A+i); __mmask16 k=_mm512_test_epi32_mask(a,_mm512_set1_epi32(1)); _mm512_mask_compressstoreu_epi32(o,k,a); o+=_mm_popcnt_u32(k);} }
static void __attribute__((noinline,optimize("no-tree-vectorize"))) k_compress_scalar(void){ int32_t*o=C; for(int i=0;i<N;i++){ if(A[i]&1) *o++=A[i]; } }
static void __attribute__((noinline,optimize("no-tree-vectorize"))) k_compress_scalar_bl(void){ int32_t*o=C; for(int i=0;i<N;i++){ *o=A[i]; o+=A[i]&1; } }
A512 static void k_compressb_reg(void){ uint8_t*o=(uint8_t*)C; for(int i=0;i<N*4;i+=64){ __m512i a=_mm512_load_si512(BA+i); __mmask64 k=_mm512_test_epi8_mask(a,_mm512_set1_epi8(1)); _mm512_storeu_si512(o,_mm512_maskz_compress_epi8(k,a)); o+=_mm_popcnt_u64(k);} }
A512 static void k_compressb_mem(void){ uint8_t*o=(uint8_t*)C; for(int i=0;i<N*4;i+=64){ __m512i a=_mm512_load_si512(BA+i); __mmask64 k=_mm512_test_epi8_mask(a,_mm512_set1_epi8(1)); _mm512_mask_compressstoreu_epi8(o,k,a); o+=_mm_popcnt_u64(k);} }
A512 static void k_expand(void){ const int32_t*s=A; for(int i=0;i<N;i+=16){ __mmask16 k=(__mmask16)B[i]; _mm512_store_si512(C+i,_mm512_maskz_expandloadu_epi32(k,s)); s+=_mm_popcnt_u32(k);} }
A512 static void k_gather(void){ for(int i=0;i<N;i+=16) _mm512_store_si512(C+i,_mm512_i32gather_epi32(_mm512_load_si512(B+i),A,4)); }
static void __attribute__((noinline,optimize("no-tree-vectorize"))) k_gather_scalar(void){ for(int i=0;i<N;i++) C[i]=A[B[i]]; }
A512 static void k_scatter(void){ for(int i=0;i<N;i+=16) _mm512_i32scatter_epi32(C,_mm512_load_si512(B+i),_mm512_load_si512(A+i),4); }
static void __attribute__((noinline,optimize("no-tree-vectorize"))) k_scatter_scalar(void){ for(int i=0;i<N;i++) C[B[i]]=A[i]; }
A512 static void k_gather_big(void){ for(int i=0;i<N;i+=16) _mm512_store_si512(C+i,_mm512_i32gather_epi32(_mm512_slli_epi32(_mm512_load_si512(B+i),12),BIG,4)); }
A512 static void k_conflict(void){ for(int i=0;i<N;i+=16) _mm512_store_si512(C+i,_mm512_conflict_epi32(_mm512_load_si512(A+i))); }
A512 static void k_prefix(void){ __m512i carry=_mm512_setzero_si512(), z=_mm512_setzero_si512(); const __m512i last=_mm512_set1_epi32(15);
  for(int i=0;i<N;i+=16){ __m512i x=_mm512_load_si512(A+i);
    x=_mm512_add_epi32(x,_mm512_alignr_epi32(x,z,15)); x=_mm512_add_epi32(x,_mm512_alignr_epi32(x,z,14)); x=_mm512_add_epi32(x,_mm512_alignr_epi32(x,z,12)); x=_mm512_add_epi32(x,_mm512_alignr_epi32(x,z,8));
    x=_mm512_add_epi32(x,carry); carry=_mm512_permutexvar_epi32(last,x); _mm512_store_si512(C+i,x);} }
static void __attribute__((noinline,optimize("no-tree-vectorize"))) k_prefix_scalar(void){ uint32_t s=0; for(int i=0;i<N;i++){ s+=(uint32_t)A[i]; C[i]=(int32_t)s; } }
A512 static void k_prefix_xor_clmul(void){ uint64_t carry=0; uint64_t *a=(uint64_t*)A,*c=(uint64_t*)C; for(int i=0;i<N/2;i++){ __m128i v=_mm_clmulepi64_si128(_mm_cvtsi64_si128(a[i]),_mm_set1_epi8(-1),0); uint64_t r=_mm_cvtsi128_si64(v)^carry; c[i]=r; carry=(uint64_t)((int64_t)r>>63);} }
A512 static void k_permb(void){ __m512i idx=_mm512_load_si512(BB); for(int i=0;i<N*4;i+=64) _mm512_store_si512((uint8_t*)C+i,_mm512_permutexvar_epi8(idx,_mm512_load_si512(BA+i))); }
A512 static void k_pshufb(void){ __m512i idx=_mm512_load_si512(BB); for(int i=0;i<N*4;i+=64) _mm512_store_si512((uint8_t*)C+i,_mm512_shuffle_epi8(_mm512_load_si512(BA+i),idx)); }
A512 static void k_classify_b(void){ // nibble-table classification (simdjson style), 64 bytes/vector
  __m512i lo=_mm512_load_si512(BB), m=_mm512_set1_epi8(15); for(int i=0;i<N*4;i+=64){ __m512i x=_mm512_load_si512(BA+i);
    __m512i r=_mm512_and_si512(_mm512_shuffle_epi8(lo,_mm512_and_si512(x,m)),_mm512_shuffle_epi8(lo,_mm512_and_si512(_mm512_srli_epi16(x,4),m))); _mm512_store_si512((uint8_t*)C+i,r);} }
A512 static void k_classify_perm2b(void){ // one vpermi2b = full 7-bit table lookup, 64 bytes/vector
  __m512i t0=_mm512_load_si512(BB), t1=_mm512_load_si512(BB+64); for(int i=0;i<N*4;i+=64){ __m512i x=_mm512_load_si512(BA+i); _mm512_store_si512((uint8_t*)C+i,_mm512_permutex2var_epi8(t0,x,t1)); } }
A512 static void k_classify_d(void){ // classification done on widened dwords via gather from a 256-entry table: 16 bytes/vector
  for(int i=0;i<N;i+=16){ __m512i x=_mm512_cvtepu8_epi32(_mm_load_si128((void*)(BA+i))); _mm512_store_si512(C+i,_mm512_i32gather_epi32(x,BIG,4)); } }
A512 static void k_widen(void){ for(int i=0;i<N;i+=16) _mm512_store_si512(C+i,_mm512_cvtepu8_epi32(_mm_load_si128((void*)(BA+i)))); }
A512 static void k_narrow(void){ for(int i=0;i<N;i+=16) _mm_store_si128((void*)((uint8_t*)C+i),_mm512_cvtepi32_epi8(_mm512_load_si512(A+i))); }
A512 static void k_pext(void){ uint64_t *a=(uint64_t*)A,*c=(uint64_t*)C; for(int i=0;i<N/2;i++) c[i]=_pext_u64(a[i],0x5555aaaa0f0f3333ull); }
A512 static void k_popcntd(void){ for(int i=0;i<N;i+=16) _mm512_store_si512(C+i,_mm512_popcnt_epi32(_mm512_load_si512(A+i))); }
A512 static void k_kmov(void){ uint64_t acc=0; for(int i=0;i<N;i+=16){ __mmask16 k=_mm512_test_epi32_mask(_mm512_load_si512(A+i),_mm512_load_si512(B+i)); acc+=_mm_popcnt_u32(k);} C[0]=acc; }
A512 static void k_ternlog(void){ for(int i=0;i<N;i+=16) _mm512_store_si512(C+i,_mm512_ternarylogic_epi32(_mm512_load_si512(A+i),_mm512_load_si512(B+i),_mm512_load_si512(C+i),0x96)); }
A512 static void k_varshift(void){ for(int i=0;i<N;i+=16) _mm512_store_si512(C+i,_mm512_srlv_epi32(_mm512_load_si512(A+i),_mm512_and_si512(_mm512_load_si512(B+i),_mm512_set1_epi32(31)))); }
A512 static void k_minmax(void){ for(int i=0;i<N;i+=16) _mm512_store_si512(C+i,_mm512_max_epi32(_mm512_load_si512(A+i),_mm512_load_si512(B+i))); }
A512 static void k_maskstore(void){ for(int i=0;i<N;i+=16) _mm512_mask_storeu_epi32(C+i,0x0fff,_mm512_load_si512(A+i)); }
#define STREAMN (1<<24)
A512 static void k_stream512(void){ for(int i=0;i<STREAMN;i+=16) _mm512_store_si512(BIG+i,_mm512_add_epi32(_mm512_load_si512(BIG+i),_mm512_set1_epi32(1))); }
A512 static void k_stream256(void){ for(int i=0;i<STREAMN;i+=8) _mm256_store_si256((void*)(BIG+i),_mm256_add_epi32(_mm256_load_si256((void*)(BIG+i)),_mm256_set1_epi32(1))); }

static double spin_ns(void){ // fixed scalar workload, to detect frequency changes
  uint64_t t0=now_ns(); volatile uint64_t x=1; for(int i=0;i<50000000;i++) x=x*3+1; return (double)(now_ns()-t0); }
A512 static void heat512(void){ __m512i a=_mm512_set1_epi32(three),b=a; uint64_t t0=now_ns(); while(now_ns()-t0<300000000ull){ for(int i=0;i<100000;i++){ a=_mm512_mullo_epi32(a,b); b=_mm512_add_epi32(a,b);} } sink=_mm512_reduce_add_epi32(a); }

int main(void){
  setvbuf(stdout,0,_IONBF,0);
  FILE*f=fopen("/proc/cpuinfo","r"); char line[8192]; cpuflags[0]=0;
  while(f&&fgets(line,sizeof line,f)) if(!strncmp(line,"flags",5)){ char*c=strchr(line,':'); snprintf(cpuflags,sizeof cpuflags," %s ",c+2); char*nl=strchr(cpuflags,'\n'); if(nl){nl[0]=' ';} break; }
  unsigned a,b,c,d; char brand[49]={0};
  for(unsigned i=0;i<3;i++){ __get_cpuid(0x80000002+i,&a,&b,&c,&d); memcpy(brand+16*i,&a,4);memcpy(brand+16*i+4,&b,4);memcpy(brand+16*i+8,&c,4);memcpy(brand+16*i+12,&d,4);}
  printf("brand: %s\n",brand);
  __get_cpuid(1,&a,&b,&c,&d); printf("hypervisor bit: %u\n",(c>>31)&1);
  __get_cpuid_count(0xd,0,&a,&b,&c,&d); printf("XCR0 supported mask (cpuid.0xd): 0x%x ; ",a);
  { unsigned lo,hi; __asm__("xgetbv":"=a"(lo),"=d"(hi):"c"(0)); printf("XCR0 enabled: 0x%x (opmask/zmm_hi256/hi16_zmm = bits 5,6,7: %s)\n",lo,(lo&0xe0)==0xe0?"enabled":"NOT enabled"); }

  struct sigaction sa={0}; sa.sa_handler=on_sigill; sigemptyset(&sa.sa_mask); sigaction(SIGILL,&sa,0);
  printf("\n## execution probes\n| extension | CPUID/flags says | actually executes |\n|---|---|---|\n");
  for(unsigned i=0;i<sizeof probes/sizeof *probes;i++){
    int ok; if(sigsetjmp(jb,1)==0){ probes[i].f(); ok=1; } else ok=0;
    int fl=has_flag(probes[i].cpuflag);
    printf("| %s | %s | %s |%s\n",probes[i].name, fl?"yes":"no", ok?"yes":"SIGILL", fl!=ok?" **MISMATCH**":"");
  }

  // calibrate tsc
  { uint64_t n0=now_ns(),t0=__rdtsc(); while(now_ns()-n0<200000000ull); uint64_t t1=__rdtsc(),n1=now_ns(); tsc_per_ns=(double)(t1-t0)/(n1-n0); }
  printf("\nTSC: %.4f ticks/ns (%.0f MHz)\n",tsc_per_ns,tsc_per_ns*1000);
  double s0=spin_ns(); heat512(); double s1=spin_ns(); 
  printf("scalar spin before 512-bit heat: %.1f ms; right after 300ms of zmm mullo: %.1f ms (ratio %.3f)\n",s0/1e6,s1/1e6,s1/s0);

  A=POOL; B=POOL+N+2*1024+336; C=POOL+2*N+4*1024+672;
  srand(1); for(int i=0;i<N;i++){ A[i]=rand(); B[i]=rand()%N; } for(int i=0;i<N*4;i++){ BA[i]=rand()&127; BB[i]=rand()&63; }
  for(int i=0;i<(1<<24);i++) BIG[i]=i;
  printf("\n## kernel costs (L1-resident, 4 KiB arrays; best of 7)\n| kernel | TSC ticks/unit | ns/unit |\n|---|---|---|\n");
  BENCH("add i32, zmm (per 16-elt vector)",N/16,k_add512());
  BENCH("add i32, ymm (per 8-elt vector)",N/8,k_add256());
  BENCH("add i32, scalar (per element)",N,k_addscalar());
  { int32_t*sb=B,*sc=C; B=POOL+16*N+512; C=POOL+20*N+16; /* dst 64 bytes past a src mod 4096 */
    BENCH("add i32, zmm, dst 4K-ALIASED with src (+64B mod 4096)",N/16,k_add512()); B=sb; C=sc; }
  BENCH("mullo i32 zmm (per vector)",N/16,k_mul512());
  BENCH("cmp->k + blend zmm (per vector)",N/16,k_cmpblend());
  BENCH("min/max i32 zmm (per vector)",N/16,k_minmax());
  BENCH("vpternlogd zmm (per vector)",N/16,k_ternlog());
  BENCH("variable shift vpsrlvd zmm (per vector)",N/16,k_varshift());
  BENCH("vpopcntd zmm (per vector)",N/16,k_popcntd());
  BENCH("test->k, kmov->gpr, popcnt (per vector)",N/16,k_kmov());
  BENCH("masked store vmovdqu32 [m]{k} (per vector)",N/16,k_maskstore());
  BENCH("vpcompressd zmm{k}{z} reg + full store (per vector)",N/16,k_compress_reg());
  BENCH("vpcompressd [mem]{k} direct (per vector)",N/16,k_compress_mem());
  BENCH("compress scalar, branchy, 50% random (per element)",N,k_compress_scalar());
  BENCH("compress scalar, branchless (per element)",N,k_compress_scalar_bl());
  BENCH("vpcompressb zmm reg + full store (per 64-byte vector)",N*4/64,k_compressb_reg());
  BENCH("vpcompressb [mem]{k} direct (per 64-byte vector)",N*4/64,k_compressb_mem());
  BENCH("vpexpandd from mem (per vector)",N/16,k_expand());
  BENCH("vpgatherdd zmm, L1 table (per vector)",N/16,k_gather());
  BENCH("gather scalar (per element)",N,k_gather_scalar());
  BENCH("vpgatherdd zmm, 64 MiB table random (per vector)",N/16,k_gather_big());
  BENCH("vpscatterdd zmm, L1 (per vector)",N/16,k_scatter());
  BENCH("scatter scalar (per element)",N,k_scatter_scalar());
  BENCH("vpconflictd zmm (per vector)",N/16,k_conflict());
  BENCH("prefix-sum i32 in-register log-step + carry (per vector)",N/16,k_prefix());
  BENCH("prefix-sum scalar (per element)",N,k_prefix_scalar());
  BENCH("prefix-xor via pclmulqdq (per 64 bits)",N/2,k_prefix_xor_clmul());
  BENCH("pext r64 (per op)",N/2,k_pext());
  BENCH("vpermb zmm (per 64-byte vector)",N*4/64,k_permb());
  BENCH("vpshufb zmm (per 64-byte vector)",N*4/64,k_pshufb());
  BENCH("classify bytes: 2x vpshufb nibble tables (per 64 bytes)",N*4/64,k_classify_b());
  BENCH("classify bytes: vpermi2b 128-entry table (per 64 bytes)",N*4/64,k_classify_perm2b());
  BENCH("classify as dwords: widen + gather (per 16 bytes)",N/16,k_classify_d());
  BENCH("vpmovzxbd widen (per 16 elements)",N/16,k_widen());
  BENCH("vpmovdb narrow (per 16 elements)",N/16,k_narrow());
#undef REPS
#define REPS 3
  printf("\n## memory-bound (64 MiB array, read+add+write)\n| kernel | TSC ticks/unit | ns/unit |\n|---|---|---|\n");
  BENCH("zmm stream (per 64 bytes)",STREAMN/16,k_stream512());
  BENCH("ymm stream (per 64 bytes)",STREAMN/16,k_stream256());
  return 0;
}
