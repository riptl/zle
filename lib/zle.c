#include "zle.h"

#include <string.h> /* memcpy, memset */

#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__BMI2__)
#include <immintrin.h>
#define ZLE_AVX512 1
#define ZLE_SIMD   1
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define ZLE_NEON 1
#define ZLE_SIMD 1
#else
#define ZLE_SIMD 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ZLE_LIKELY(c)    __builtin_expect( !!(c), 1L )
#define ZLE_UNLIKELY(c)  __builtin_expect( !!(c), 0L )
#define ZLE_FN_NOINLINE  __attribute__((noinline))
#define ZLE_CTZ64( x )   ((size_t)__builtin_ctzll( (unsigned long long)(x) ))
#else
#define ZLE_LIKELY(c)    (c)
#define ZLE_UNLIKELY(c)  (c)
#define ZLE_FN_NOINLINE
static inline size_t
ZLE_CTZ64( uint64_t x ) {
  size_t k = 0;
  while( !(x & 1) ) { x >>= 1; k++; }
  return k;
}
#endif

typedef unsigned char uchar;

#define ZLE_MIN_Z ((size_t)3)

#define ZLE_ONES (~(uint64_t)0)

static inline size_t
zle_vput( uchar *  p,
          uint64_t v ) {
  size_t k = 0;
  do {
    p[k] = (uchar)( v & 0x7f );
    v >>= 7;
    if( v ) p[k] = (uchar)( p[k] | 0x80 );
    k++;
  } while( v );
  return k;
}

static inline int
zle_vget( uchar const * s,
          size_t        slen,
          size_t      * pi,
          uint64_t    * out ) {
  uint64_t v  = 0;
  size_t   i  = *pi;
  int      sh = 0;
  for(;;) {
    if( ZLE_UNLIKELY( (i>=slen) | (sh>42) ) ) return 0;
    uchar b = s[i++];
    v |= (uint64_t)( b & 0x7f )<<sh;
    if( !(b & 0x80) ) break;
    sh += 7;
  }
  *pi  = i;
  *out = v;
  return 1;
}

static inline size_t
zle_vlen( uint64_t v ) {
  size_t k = 1;
  while( v>=128 ) { v >>= 7; k++; }
  return k;
}

static inline size_t
zle_ext( size_t v ) {
  return v>=15 ? zle_vlen( (uint64_t)v-15 ) : 0;
}

static inline size_t
zle_frame_sz( size_t L,
              size_t Z ) {
  return 1 + zle_ext( L ) + L + zle_ext( Z );
}

#define ZLE_ZERO_THRESH ((size_t)512)

#ifdef ZLE_AVX512

static inline uint64_t
zle_zmask_raw( uchar const * p ) {
  return (uint64_t)_mm512_cmpeq_epi8_mask(
      _mm512_loadu_si512( (__m512i const *)p ), _mm512_setzero_si512() );
}

#define ZLE_NZ256_SPAN ((size_t)256)

static inline int
zle_nz_256( uchar const * p ) {
  __m512i v = _mm512_or_si512( _mm512_or_si512( _mm512_loadu_si512( (__m512i const *)( p     ) ),
                                                _mm512_loadu_si512( (__m512i const *)( p+ 64 ) ) ),
                               _mm512_or_si512( _mm512_loadu_si512( (__m512i const *)( p+128 ) ),
                                                _mm512_loadu_si512( (__m512i const *)( p+192 ) ) ) );
  return !!_mm512_test_epi8_mask( v, v );
}

#define ZLE_SHR( lo, hi, k ) \
  _mm512_alignr_epi8( _mm512_alignr_epi32( (hi), (lo), 4 ), (lo), (k) )

_Static_assert( ZLE_MIN_Z==3, "ZLE_OR3V assumes ZLE_MIN_Z==3" );

#define ZLE_OR3V( lo, hi ) \
  _mm512_ternarylogic_epi32( (lo), ZLE_SHR( (lo), (hi), 1 ), ZLE_SHR( (lo), (hi), 2 ), 0xFE )

#define ZLE_CAND256_SPAN ((size_t)320)

static inline int
zle_cand_256( uchar const * p ) {
  __m512i v0 = _mm512_loadu_si512( (__m512i const *)( p     ) );
  __m512i v1 = _mm512_loadu_si512( (__m512i const *)( p+ 64 ) );
  __m512i v2 = _mm512_loadu_si512( (__m512i const *)( p+128 ) );
  __m512i v3 = _mm512_loadu_si512( (__m512i const *)( p+192 ) );
  __m512i v4 = _mm512_loadu_si512( (__m512i const *)( p+256 ) );
  __m512i t  = _mm512_min_epu8( _mm512_min_epu8( ZLE_OR3V( v0, v1 ), ZLE_OR3V( v1, v2 ) ),
                                _mm512_min_epu8( ZLE_OR3V( v2, v3 ), ZLE_OR3V( v3, v4 ) ) );
  return !!_mm512_testn_epi8_mask( t, t );
}

static inline void
zle_copy( uchar       * ZLE_RESTRICT d,
          uchar const * ZLE_RESTRICT s,
          size_t                     sz ) {
  size_t k = 0;
  for( ; k+64<=sz; k+=64 ) {
    _mm512_storeu_si512( (__m512i *)( d+k ), _mm512_loadu_si512( (__m512i const *)( s+k ) ) );
  }
  if( k<sz ) {
    __mmask64 m = _cvtu64_mask64( ZLE_ONES>>( 64-( sz-k ) ) );
    _mm512_mask_storeu_epi8( (void *)( d+k ), m, _mm512_maskz_loadu_epi8( m, s+k ) );
  }
}

static inline void
zle_copy64( uchar       * ZLE_RESTRICT d,
            uchar const * ZLE_RESTRICT s,
            size_t                     sz ) {
  __mmask64 m = _cvtu64_mask64( _bzhi_u64( ZLE_ONES, (unsigned)sz ) );
  _mm512_mask_storeu_epi8( (void *)d, m, _mm512_maskz_loadu_epi8( m, s ) );
}

static inline void
zle_zero( uchar * d,
          size_t  sz ) {
  __m512i z = _mm512_setzero_si512();
  if( ZLE_LIKELY( sz<=64 ) ) {
    _mm512_mask_storeu_epi8( (void *)d, _cvtu64_mask64( _bzhi_u64( ZLE_ONES, (unsigned)sz ) ), z );
    return;
  }
  if( ZLE_LIKELY( sz<ZLE_ZERO_THRESH ) ) {
    size_t k = 0;
    do {
      _mm512_storeu_si512( (__m512i *)( d+k ), z ); k += 64;
    } while( k+64<=sz );
    _mm512_storeu_si512( (__m512i *)( d+sz-64 ), z );
    return;
  }
  memset( d, 0, sz );
}

#elif defined(ZLE_NEON)

static inline uint64_t
zle_movemask( uint8x16_t c0,
              uint8x16_t c1,
              uint8x16_t c2,
              uint8x16_t c3 ) {
  static uchar const bit[16] __attribute__((aligned(16))) =
    { 1,2,4,8,16,32,64,128, 1,2,4,8,16,32,64,128 };
  uint8x16_t b   = vld1q_u8( bit );
  uint8x16_t t0  = vandq_u8( c0, b );
  uint8x16_t t1  = vandq_u8( c1, b );
  uint8x16_t t2  = vandq_u8( c2, b );
  uint8x16_t t3  = vandq_u8( c3, b );
  uint8x16_t p01 = vpaddq_u8( t0,  t1  );
  uint8x16_t p23 = vpaddq_u8( t2,  t3  );
  uint8x16_t q   = vpaddq_u8( p01, p23 );
  uint8x16_t r   = vpaddq_u8( q,   q   );
  return vgetq_lane_u64( vreinterpretq_u64_u8( r ), 0 );
}

static inline uint64_t
zle_zmask_raw( uchar const * p ) {
  return zle_movemask( vceqzq_u8( vld1q_u8( p    ) ),
                       vceqzq_u8( vld1q_u8( p+16 ) ),
                       vceqzq_u8( vld1q_u8( p+32 ) ),
                       vceqzq_u8( vld1q_u8( p+48 ) ) );
}

#define ZLE_NZ256_SPAN ((size_t)256)

static inline int
zle_nz_256( uchar const * p ) {
  uint8x16_t a0 = vld1q_u8( p    );
  uint8x16_t a1 = vld1q_u8( p+16 );
  uint8x16_t a2 = vld1q_u8( p+32 );
  uint8x16_t a3 = vld1q_u8( p+48 );
  for( size_t k=64; k<256; k+=64 ) {
    a0 = vorrq_u8( a0, vld1q_u8( p+k    ) );
    a1 = vorrq_u8( a1, vld1q_u8( p+k+16 ) );
    a2 = vorrq_u8( a2, vld1q_u8( p+k+32 ) );
    a3 = vorrq_u8( a3, vld1q_u8( p+k+48 ) );
  }
  uint8x16_t v = vorrq_u8( vorrq_u8( a0, a1 ), vorrq_u8( a2, a3 ) );
  return vmaxvq_u8( v )!=0;
}

_Static_assert( ZLE_MIN_Z==3, "zle_run assumes ZLE_MIN_Z==3" );

static inline uint8x16_t
zle_run( uchar const * p ) {
  return vorrq_u8( vorrq_u8( vld1q_u8( p ), vld1q_u8( p+1 ) ), vld1q_u8( p+2 ) );
}

#define ZLE_CAND256_SPAN ((size_t)( 256+ZLE_MIN_Z-1 ))

static inline int
zle_cand_256( uchar const * p ) {
  uint8x16_t a0 = zle_run( p    );
  uint8x16_t a1 = zle_run( p+16 );
  uint8x16_t a2 = zle_run( p+32 );
  uint8x16_t a3 = zle_run( p+48 );
  for( size_t k=64; k<256; k+=64 ) {
    a0 = vminq_u8( a0, zle_run( p+k    ) );
    a1 = vminq_u8( a1, zle_run( p+k+16 ) );
    a2 = vminq_u8( a2, zle_run( p+k+32 ) );
    a3 = vminq_u8( a3, zle_run( p+k+48 ) );
  }
  uint8x16_t t = vminq_u8( vminq_u8( a0, a1 ), vminq_u8( a2, a3 ) );
  return vminvq_u8( t )==0;
}

#define ZLE_MOV( type ) do {                           \
    type _a, _b;                                       \
    memcpy( &_a, s,                 sizeof(type) );    \
    memcpy( &_b, s+sz-sizeof(type), sizeof(type) );    \
    memcpy( d,                 &_a, sizeof(type) );    \
    memcpy( d+sz-sizeof(type), &_b, sizeof(type) );    \
  } while(0)

static inline void
zle_rest( uchar       * ZLE_RESTRICT d,
          uchar const * ZLE_RESTRICT s,
          size_t                     k,
          size_t                     sz ) {
  for( ; k+16<=sz; k+=16 ) vst1q_u8( d+k, vld1q_u8( s+k ) );
  d += k; s += k; sz -= k;
  if     ( sz>=8 ) ZLE_MOV( uint64_t );
  else if( sz>=4 ) ZLE_MOV( uint32_t );
  else if( sz>=2 ) ZLE_MOV( uint16_t );
  else if( sz    ) d[0] = s[0];
}

#undef ZLE_MOV

static inline void
zle_copy( uchar       * ZLE_RESTRICT d,
          uchar const * ZLE_RESTRICT s,
          size_t                     sz ) {
  size_t k = 0;
  for( ; k+64<=sz; k+=64 ) {
    uint8x16_t v0 = vld1q_u8( s+k    );
    uint8x16_t v1 = vld1q_u8( s+k+16 );
    uint8x16_t v2 = vld1q_u8( s+k+32 );
    uint8x16_t v3 = vld1q_u8( s+k+48 );
    vst1q_u8( d+k,    v0 );
    vst1q_u8( d+k+16, v1 );
    vst1q_u8( d+k+32, v2 );
    vst1q_u8( d+k+48, v3 );
  }
  zle_rest( d, s, k, sz );
}

static inline void
zle_copy64( uchar       * ZLE_RESTRICT d,
            uchar const * ZLE_RESTRICT s,
            size_t                     sz ) {
  zle_rest( d, s, 0, sz );
}

static inline void
zle_zero( uchar * d,
          size_t  sz ) {
  if( ZLE_UNLIKELY( sz>=ZLE_ZERO_THRESH ) ) { memset( d, 0, sz ); return; }
  if( ZLE_LIKELY( sz>=16 ) ) {
    uint8x16_t z = vdupq_n_u8( 0 );
    size_t k = 0;
    do { vst1q_u8( d+k, z ); k += 16; } while( k+16<=sz );
    vst1q_u8( d+sz-16, z );
    return;
  }
  uint64_t zero = 0;
  if     ( sz>=8 ) { memcpy( d, &zero, 8 ); memcpy( d+sz-8, &zero, 8 ); }
  else if( sz>=4 ) { memcpy( d, &zero, 4 ); memcpy( d+sz-4, &zero, 4 ); }
  else if( sz>=2 ) { memcpy( d, &zero, 2 ); memcpy( d+sz-2, &zero, 2 ); }
  else if( sz    ) d[0] = (uchar)0;
}

#else

static inline void
zle_copy( uchar       * ZLE_RESTRICT d,
          uchar const * ZLE_RESTRICT s,
          size_t                     sz ) {
  if( sz ) memcpy( d, s, sz );
}

static inline void
zle_zero( uchar * d,
          size_t  sz ) {
  if( sz ) memset( d, 0, sz );
}

#endif

static uchar *
zle_emit( uchar *                    o,
          uchar const * ZLE_RESTRICT lit,
          size_t                     L,
          size_t                     Z ) {
  size_t ln = L<15 ? L : 15;
  size_t zn = Z<15 ? Z : 15;
  *o++ = (uchar)( (ln<<4) | zn );
  if( L>=15 ) o += zle_vput( o, (uint64_t)L-15 );
  zle_copy( o, lit, L );
  o += L;
  if( Z>=15 ) o += zle_vput( o, (uint64_t)Z-15 );
  return o;
}

#if ZLE_SIMD

static inline uint64_t
zle_zmask( uchar const * s,
           size_t        w,
           size_t        n ) {
  size_t   off = w<<6;
  uint64_t m   = zle_zmask_raw( s+off );
  if( ZLE_LIKELY( off+64<=n ) ) return m;
  return m & ( ZLE_ONES>>( 64-( n-off ) ) );
}

_Static_assert( ZLE_MIN_Z==3, "zle_cand assumes ZLE_MIN_Z==3" );

static inline uint64_t
zle_cand( uint64_t m,
          uint64_t nz ) {
  return m & ( ( m>>1 ) | ( nz<<63 ) ) & ( ( m>>2 ) | ( nz*( (uint64_t)3<<62 ) ) );
}

static inline size_t
zle_spill( uchar const * s,
           size_t        n,
           size_t        words,
           size_t      * pw,
           uint64_t    * pm,
           size_t        len ) {
  size_t w = *pw+1;
  for(;;) {
    if( w>=words ) { *pw = w; *pm = 0; return len; }
    uint64_t t = zle_zmask( s, w, n );
    if( ZLE_UNLIKELY( t!=ZLE_ONES ) ) {
      size_t ext = ZLE_CTZ64( ~t );
      *pw = w;
      *pm = t & ~( ( (uint64_t)1<<ext )-1 );
      return len+ext;
    }
    w++; len += 64;
    while( ( w<<6 )+ZLE_NZ256_SPAN<=n ) {
      if( ZLE_UNLIKELY( zle_nz_256( s+( w<<6 ) ) ) ) break;
      w += 4; len += 256;
    }
  }
}

static inline uchar *
zle_vput_small( uchar *  o,
                uint64_t v ) {
  uint64_t big = (uint64_t)( v>=128 );
  o[0] = (uchar)( ( v & 0x7f ) | ( big<<7 ) );
  o[1] = (uchar)( v>>7 );
  return o+1+big;
}

static inline uchar *
zle_emit_small( uchar *                    o,
                uchar const * ZLE_RESTRICT lit,
                size_t                     L,
                size_t                     Z ) {
  size_t ln = L<15 ? L : 15;
  size_t zn = Z<15 ? Z : 15;
  *o++ = (uchar)( (ln<<4) | zn );
  if( L>=15 ) o = zle_vput_small( o, (uint64_t)L-15 );
  if( ZLE_LIKELY( L<=64 ) ) zle_copy64( o, lit, L );
  else                      zle_copy  ( o, lit, L );
  o += L;
  if( Z>=15 ) o = zle_vput_small( o, (uint64_t)Z-15 );
  return o;
}

#define ZLE_EXT( m ) do {                                              \
    size_t _e = ~(m) ? ZLE_CTZ64( ~(m) ) : (size_t)64;                 \
    len  += _e;                                                        \
    (m)   = _e<64 ? ( (m) & ~( ( (uint64_t)1<<_e )-1 ) ) : (uint64_t)0; \
    full  = _e==64;                                                    \
  } while(0)

#define ZLE_WALK( m, wb, EXT ) do {                                    \
    while( m ) {                                                       \
      size_t   b   = ZLE_CTZ64( m );                                   \
      size_t   zs  = (size_t)(wb)+b;                                   \
      uint64_t hi  = (m)>>b;                                           \
      size_t   len;                                                    \
      int      full = 0;                                               \
      if( ~hi ) {                                                      \
        len = ZLE_CTZ64( ~hi );                                        \
        (m) &= ~( ( ( (uint64_t)1<<len )-1 )<<b );                     \
      } else {                                                         \
        len = 64-b;                                                    \
        (m) = 0;                                                       \
      }                                                                \
      if( b+len==64 ) { EXT; }                                         \
      if( len>=ZLE_MIN_Z ) {                                           \
        o  = zle_emit_small( o, src+fs, zs-fs, len );                  \
        fs = zs+len;                                                   \
      }                                                                \
      (void)full;                                                      \
    }                                                                  \
  } while(0)

static inline size_t
zle_fini( uchar       * ZLE_RESTRICT dst,
          uchar       *              o,
          uchar const * ZLE_RESTRICT src,
          size_t                     n,
          size_t                     fs ) {
  if( fs<n ) o = zle_emit_small( o, src+fs, n-fs, 0 );
  size_t sz = (size_t)( o-dst );
  if( ZLE_UNLIKELY( sz>=1+zle_ext( n )+n ) ) return (size_t)( zle_emit( dst, src, n, 0 )-dst );
  return sz;
}

static size_t
zle_w1( uchar       * ZLE_RESTRICT dst,
        uchar const * ZLE_RESTRICT src,
        size_t                     n ) {
  uint64_t m0 = zle_zmask( src, 0, n );
  uchar *  o  = dst;
  size_t   fs = 0;
  ZLE_WALK( m0, 0, (void)0 );
  return zle_fini( dst, o, src, n, fs );
}

static size_t
zle_w2( uchar       * ZLE_RESTRICT dst,
        uchar const * ZLE_RESTRICT src,
        size_t                     n ) {
  uint64_t m0 = zle_zmask( src, 0, n );
  uint64_t m1 = zle_zmask( src, 1, n );
  uchar *  o  = dst;
  size_t   fs = 0;
  ZLE_WALK( m0,  0, ZLE_EXT( m1 ) );
  ZLE_WALK( m1, 64, (void)0 );
  return zle_fini( dst, o, src, n, fs );
}

static size_t
zle_w3( uchar       * ZLE_RESTRICT dst,
        uchar const * ZLE_RESTRICT src,
        size_t                     n ) {
  uint64_t m0 = zle_zmask( src, 0, n );
  uint64_t m1 = zle_zmask( src, 1, n );
  uint64_t m2 = zle_zmask( src, 2, n );
  uchar *  o  = dst;
  size_t   fs = 0;
  ZLE_WALK( m0,   0, { ZLE_EXT( m1 ); if( full ) ZLE_EXT( m2 ); } );
  ZLE_WALK( m1,  64, ZLE_EXT( m2 ) );
  ZLE_WALK( m2, 128, (void)0 );
  return zle_fini( dst, o, src, n, fs );
}

static size_t
zle_w4( uchar       * ZLE_RESTRICT dst,
        uchar const * ZLE_RESTRICT src,
        size_t                     n ) {
  uint64_t m0 = zle_zmask( src, 0, n );
  uint64_t m1 = zle_zmask( src, 1, n );
  uint64_t m2 = zle_zmask( src, 2, n );
  uint64_t m3 = zle_zmask( src, 3, n );
  uchar *  o  = dst;
  size_t   fs = 0;
  ZLE_WALK( m0,   0, { ZLE_EXT( m1 ); if( full ) { ZLE_EXT( m2 ); if( full ) ZLE_EXT( m3 ); } } );
  ZLE_WALK( m1,  64, { ZLE_EXT( m2 ); if( full ) ZLE_EXT( m3 ); } );
  ZLE_WALK( m2, 128, ZLE_EXT( m3 ) );
  ZLE_WALK( m3, 192, (void)0 );
  return zle_fini( dst, o, src, n, fs );
}

#undef ZLE_WALK
#undef ZLE_EXT

static size_t ZLE_FN_NOINLINE
zle_big( uchar       * ZLE_RESTRICT dst,
         uchar const * ZLE_RESTRICT src,
         size_t                     n ) {

  size_t    esc_sz = 1 + zle_ext( n ) + n;
  uintptr_t lim    = (uintptr_t)dst + esc_sz;
  size_t    words  = ( n+63 )>>6;

  uchar *  o  = dst;
  size_t   fs = 0;
  size_t   w  = 0;
  uint64_t m  = zle_zmask( src, 0, n );
  uint64_t d  = zle_cand( m, 1 );

  for(;;) {

    if( !d ) {

      w++;
      for(;;) {
        if( ZLE_UNLIKELY( w>=words ) ) goto trailing;
        m = zle_zmask( src, w, n );
        d = zle_cand( m, 1 );
        if( d ) break;
        w++;
        while( ( w<<6 )+ZLE_CAND256_SPAN<=n ) {
          if( ZLE_UNLIKELY( zle_cand_256( src+( w<<6 ) ) ) ) break;
          w += 4;
        }
      }
    }

    uint64_t lo = d & ( ~d+1 );
    size_t   b  = ZLE_CTZ64( d );
    size_t   zs = ( w<<6 )+b;
    uint64_t x  = d + lo;
    size_t   len;
    if( ZLE_LIKELY( x ) ) {
      len = ZLE_CTZ64( x )-b+( ZLE_MIN_Z-1 );
      d   = x & ( x-1 );
    } else {
      len = zle_spill( src, n, words, &w, &m, 64-b );
      d   = zle_cand( m, 1 );
    }
    if( ZLE_LIKELY( len>=ZLE_MIN_Z ) ) {
      size_t L = zs-fs;
      if( ZLE_UNLIKELY( (uintptr_t)o+L+15>=lim ) &&
          (uintptr_t)o+zle_frame_sz( L, len )>=lim ) goto escape;
      o  = zle_emit( o, src+fs, L, len );
      fs = zs+len;
    }
  }

trailing:
  if( fs<n ) {
    size_t L = n-fs;
    if( ZLE_UNLIKELY( (uintptr_t)o+L+15>=lim ) &&
        (uintptr_t)o+zle_frame_sz( L, 0 )>=lim ) goto escape;
    o = zle_emit( o, src+fs, L, 0 );
  }
  return (size_t)( o-dst );

escape:
  return (size_t)( zle_emit( dst, src, n, 0 )-dst );
}

size_t
ZLE_compress( void       * ZLE_RESTRICT dst,
              void const * ZLE_RESTRICT src,
              size_t                    src_sz ) {
  uchar *       d = (uchar *)dst;
  uchar const * s = (uchar const *)src;
  if( ZLE_UNLIKELY( !src_sz       ) ) return 0;
  if( ZLE_LIKELY  ( src_sz<= 64   ) ) return zle_w1 ( d, s, src_sz );
  if( ZLE_LIKELY  ( src_sz<=128   ) ) return zle_w2 ( d, s, src_sz );
  if( ZLE_LIKELY  ( src_sz<=192   ) ) return zle_w3 ( d, s, src_sz );
  if( ZLE_LIKELY  ( src_sz<=256   ) ) return zle_w4 ( d, s, src_sz );
  return                                     zle_big( d, s, src_sz );
}

#else /* !ZLE_SIMD */

size_t
ZLE_compress( void       * ZLE_RESTRICT dst,
              void const * ZLE_RESTRICT src,
              size_t                    src_sz ) {
  uchar *       d = (uchar *)dst;
  uchar const * s = (uchar const *)src;
  size_t        n = src_sz;

  if( ZLE_UNLIKELY( !n ) ) return 0;

  size_t esc_sz = 1 + zle_ext( n ) + n;

  uchar * o   = d;
  size_t  pos = 0;
  size_t  fs  = 0;
  while( pos<n ) {
    size_t zs = pos;
    while( (zs<n) &&  s[zs] ) zs++;
    if( zs==n ) break;
    size_t ze = zs;
    while( (ze<n) && !s[ze] ) ze++;
    if( ze-zs>=ZLE_MIN_Z ) {
      size_t L = zs-fs;
      size_t Z = ze-zs;
      if( ZLE_UNLIKELY( (size_t)( o-d )+zle_frame_sz( L, Z )>=esc_sz ) ) goto escape;
      o  = zle_emit( o, s+fs, L, Z );
      fs = ze;
    }
    pos = ze;
  }
  if( fs<n ) {
    size_t L = n-fs;
    if( ZLE_UNLIKELY( (size_t)( o-d )+zle_frame_sz( L, 0 )>=esc_sz ) ) goto escape;
    o = zle_emit( o, s+fs, L, 0 );
  }
  return (size_t)( o-d );

escape:
  return (size_t)( zle_emit( d, s, n, 0 )-d );
}

#endif /* ZLE_SIMD */

int64_t
ZLE_decompress( void       * ZLE_RESTRICT dst,
                size_t                    dst_max,
                void const * ZLE_RESTRICT src,
                size_t                    src_sz ) {
  uchar *       d = (uchar *)dst;
  uchar const * s = (uchar const *)src;
  size_t i = 0;
  size_t o = 0;
  while( i<src_sz ) {
    uchar t = s[i++];
    if( ZLE_UNLIKELY( !t ) ) return ZLE_ERR_CORRUPT;
    uint64_t L = (uint64_t)( t>>4 );
    uint64_t Z = (uint64_t)( t&15 );
    uint64_t e;
    if( L==15 ) {
      if( ZLE_UNLIKELY( !zle_vget( s, src_sz, &i, &e ) ) ) return ZLE_ERR_CORRUPT;
      L += e;
    }
    if( ZLE_UNLIKELY( L>(uint64_t)( src_sz-i ) ) ) return ZLE_ERR_CORRUPT;
    if( ZLE_UNLIKELY( L>(uint64_t)( dst_max-o ) ) ) return ZLE_ERR_SPACE;
    zle_copy( d+o, s+i, (size_t)L );
    i += (size_t)L;
    o += (size_t)L;
    if( Z==15 ) {
      if( ZLE_UNLIKELY( !zle_vget( s, src_sz, &i, &e ) ) ) return ZLE_ERR_CORRUPT;
      Z += e;
    }
    if( ZLE_UNLIKELY( Z>(uint64_t)( dst_max-o ) ) ) return ZLE_ERR_SPACE;
    zle_zero( d+o, (size_t)Z );
    o += (size_t)Z;
  }
  return (int64_t)o;
}

char const *
ZLE_strerror( int64_t res ) {
  switch( res ) {
  case -1: return "out of buffer space";
  case -2: return "malformed compressed data";
  case  0: return "empty";
  default:
    if( ZLE_UNLIKELY( res>0 ) ) return "ok";
    return "unknown";
  }
}
