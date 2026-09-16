#include "zle.h"

#include <string.h> /* memcpy, memset, memchr */

#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__BMI__) && defined(__BMI2__) && \
    ( defined(__GNUC__) || defined(__clang__) )
#include <immintrin.h>
#define ZLE_AVX512 1
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define ZLE_NEON 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ZLE_LIKELY(c)       __builtin_expect( !!(c), 1L )
#define ZLE_UNLIKELY(c)     __builtin_expect( !!(c), 0L )
#define ZLE_FN_NOINLINE     __attribute__((noinline))
#define ZLE_FN_ALWAYS_INLINE __attribute__((always_inline))
#else
#define ZLE_LIKELY(c)       (c)
#define ZLE_UNLIKELY(c)     (c)
#define ZLE_FN_NOINLINE
#define ZLE_FN_ALWAYS_INLINE
#endif

/* ZLE_SANITIZE is 1 when building with ASan or MSan */

#if defined(__has_feature)
# if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#  define ZLE_SANITIZE 1
# endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(ZLE_SANITIZE)
# define ZLE_SANITIZE 1
#endif
#ifndef ZLE_SANITIZE
# define ZLE_SANITIZE 0
#endif

typedef unsigned char uchar;

/* Wire format: a stream of tokens, each producing L literal bytes then
   Z zero bytes.

     L         field
     literals  L bytes verbatim
     Z         field, omitted in the last token

   A field is one byte if <0xFF, else 0xFF followed by leb128( v-0xFF ).

   The encoder breaks the literal run at every zero run of at least
   ZLE_MIN_Z bytes, or at least 2 bytes if it ends the input.

   The output is at most src_sz+src_sz/386+6 bytes.  A token costs its
   two field sizes minus Z, which is <=0 unless the L field escapes.  An
   escaped L field costs +1 at most once per 386 input bytes (L>=383,
   Z>=3).  The last token adds at most a 6 byte L field. */

#define ZLE_ESC       ((size_t)0xFF) /* field escape marker */
#define ZLE_MIN_Z     ((size_t)3)    /* shortest zero run worth a token */
#define ZLE_VARINT_SH (42)           /* max varint shift of a valid field */

/* zle_put_int writes field n at out and returns the cursor past it.  If
   narrow, n<0xFF. */

static inline ZLE_FN_ALWAYS_INLINE uchar *
zle_put_int( uchar * out,
             size_t  n,
             int     narrow ) {
  if( narrow || ZLE_LIKELY( n<ZLE_ESC ) ) {
    out[0] = (uchar)n;
    return out+1;
  }

  *out++ = (uchar)ZLE_ESC;
  n     -= ZLE_ESC;
  while( n>=0x80 ) {
    *out++ = (uchar)( n | 0x80 );
    n    >>= 7;
  }
  out[0] = (uchar)n;
  return out+1;
}

/* zle_get_int reads the varint following an escape marker at src.  Sets
   *n to the field value and returns the cursor past it.  Returns NULL
   if the varint is truncated or too long. */

static inline ZLE_FN_ALWAYS_INLINE uchar const *
zle_get_int( uchar const * src,
             uchar const * src_end,
             uint64_t *    n ) {
  uint64_t v = 0;
  int      k = 0;
  uchar    b;
  do {
    if( ZLE_UNLIKELY( ( src==src_end ) | ( k>ZLE_VARINT_SH ) ) ) return NULL;
    b  = *src++;
    v |= (uint64_t)( b & 0x7f )<<k;
    k += 7;
  } while( b & 0x80 );
  *n = v+ZLE_ESC;
  return src;
}

#ifdef ZLE_AVX512

/* The copy and zero helpers below are unrolled on purpose: clang turns
   loops here into memcpy/memset calls. */

/* zle_copy64 copies sz<=64 bytes. */

static inline void
zle_copy64( uchar       * ZLE_RESTRICT d,
            uchar const * ZLE_RESTRICT s,
            size_t                     sz ) {
  __mmask64 mm = _cvtu64_mask64( _bzhi_u64( ~(uint64_t)0, (unsigned)sz ) );
  _mm512_mask_storeu_epi8( d, mm, _mm512_maskz_loadu_epi8( mm, s ) );
}

/* zle_copy copies sz bytes without touching bytes past d+sz or s+sz.  If
   narrow, sz<=254. */

static inline ZLE_FN_ALWAYS_INLINE void
zle_copy( uchar       * ZLE_RESTRICT d,
          uchar const * ZLE_RESTRICT s,
          size_t                     sz,
          int                        narrow ) {
  size_t tail, mid_a, mid_b;

  if( ZLE_LIKELY( sz<=64 ) ) {
    zle_copy64( d, s, sz );
    return;
  }

  /* over 256 and the optimized memcpy is faster */
  if( !narrow && ZLE_UNLIKELY( sz>256 ) ) {
    memcpy( d, s, sz );
    return;
  }

  /* 65..256 bytes: four overlapping words */
  tail  = sz-64;
  mid_a = tail< 64 ? tail :  64;
  mid_b = tail<128 ? tail : 128;
  _mm512_storeu_si512( d,       _mm512_loadu_si512( s       ) );
  _mm512_storeu_si512( d+tail,  _mm512_loadu_si512( s+tail  ) );
  _mm512_storeu_si512( d+mid_a, _mm512_loadu_si512( s+mid_a ) );
  _mm512_storeu_si512( d+mid_b, _mm512_loadu_si512( s+mid_b ) );
}

/* zle_zero zeroes sz bytes without touching bytes past d+sz.  Unlike
   zle_copy, it tests word by word, as most decoded zero runs of 65..256
   bytes need only two stores. */

static inline void
zle_zero( uchar * d,
          size_t  sz ) {
  __m512i z = _mm512_setzero_si512();
  size_t  p;

  if( ZLE_LIKELY( sz<=64 ) ) {
    _mm512_mask_storeu_epi8( d, _cvtu64_mask64( _bzhi_u64( ~(uint64_t)0, (unsigned)sz ) ), z );
    return;
  }

  if( ZLE_LIKELY( sz<=256 ) ) {
    /* eager branching instead of overlapping writes - measured to be faster */
    _mm512_storeu_si512( d, z );
    if( ZLE_LIKELY( sz<=128 ) ) { _mm512_storeu_si512( d+sz-64, z ); return; }
    _mm512_storeu_si512( d+64, z );
    if( sz<=192 ) { _mm512_storeu_si512( d+sz-64, z ); return; }
    _mm512_storeu_si512( d+128, z );
    _mm512_storeu_si512( d+sz-64, z );
    return;
  }

  if( ZLE_UNLIKELY( sz>=512 ) ) { memset( d, 0, sz ); return; }
  p = 0;
  do {
    _mm512_storeu_si512( d+p, z );
    p += 64;
  } while( p+64<sz );
  _mm512_storeu_si512( d+sz-64, z );
}

/* The encoders find zero runs with a pair mask: bit i is set iff bytes
   i and i+1 are both zero.  Adding the lowest set bit to the mask
   carries through the run's bits and sets the bit of the run's last
   byte, so ctz gives both ends of the run without a scan. */

/* zle_compress_tiny encodes 1<=len<=64 bytes from one load. */

static size_t
zle_compress_tiny( uchar       * ZLE_RESTRICT dst,
                   uchar const * ZLE_RESTRICT src,
                   size_t                     len ) {
# if ZLE_SANITIZE
  /* sanitizers would flag the read past len */
  __m512i  v    = _mm512_maskz_loadu_epi8( _cvtu64_mask64( _bzhi_u64( ~(uint64_t)0, (unsigned)len ) ), src );
# else
  __m512i  v    = _mm512_loadu_si512( src ); /* may read up to align_up( len, 64 ) */
# endif
  uint64_t d    = (uint64_t)_mm512_testn_epi8_mask( v, v );
  uchar *  out  = dst;
  size_t   base = 0; /* start of pending literals */

  d = _bzhi_u64( d & ( d>>1 ), (unsigned)(len-1) ); /* drop pairs past len */

  /* The highest pair bit is len-2, so the carry stays in the word.
     d==0 runs once with a run [64,65) that is never emitted. */
  do {
    size_t zpos = (size_t)_tzcnt_u64( d );
    size_t zend, Z;
    d   += _blsi_u64( d );
    zend = (size_t)_tzcnt_u64( d )+1;
    Z    = zend-zpos;
    if( ZLE_LIKELY( ( Z>=ZLE_MIN_Z ) | ( zend==len ) ) ) {
      size_t L = zpos-base;
      out[0] = (uchar)L;
      zle_copy64( out+1, src+base, L );
      out[ L+1 ] = (uchar)Z;
      out += L+2;
      base = zend;
    }
    d = _blsr_u64( d ); /* clear the carry bit */
  } while( d );

  if( base<len ) { /* last token */
    size_t L = len-base;
    out[0] = (uchar)L;
    zle_copy64( out+1, src+base, L );
    out += L+1;
  }
  return (size_t)( out-dst );
}

/* zle_pairs returns the pair mask of the 64 byte word at p.  Reads 65
   bytes. */

static inline ZLE_FN_ALWAYS_INLINE uint64_t
zle_pairs( uchar const * p ) {
  __m512i v = _mm512_or_si512( _mm512_loadu_si512( p ), _mm512_loadu_si512( p+1 ) );
  return (uint64_t)_mm512_testn_epi8_mask( v, v );
}

/* zle_emit writes the token for literals [base,zpos) and zeros
   [zpos,zend).  Returns the cursor past it. */

static inline ZLE_FN_ALWAYS_INLINE uchar *
zle_emit( uchar *       out,
          uchar const * base,
          uchar const * zpos,
          uchar const * zend,
          int           narrow ) {
  size_t L = (size_t)( zpos-base );
  out = zle_put_int( out, L, narrow );
  zle_copy( out, base, L, narrow );
  return zle_put_int( out+L, (size_t)( zend-zpos ), narrow );
}

/* zle_zero_end returns the first non-zero byte in [src,end), or end if
   there is none.  Assumes src<end.  Reads nothing past end.

   Loops here and in zle_compress_core are bottom-tested on purpose:
   top-tested loops make clang spill a register in the literal copy. */

static inline ZLE_FN_ALWAYS_INLINE uchar const *
zle_zero_end( uchar const * src,
              uchar const * end,
              int           narrow ) {
  __m512i  v;
  uint64_t m;
  size_t   rem;

  if( src+64<end ) do {
    /* potential short run: test first 64 bytes */
    v = _mm512_loadu_si512( src );
    m = (uint64_t)_mm512_test_epi8_mask( v, v );
    if( m ) return src+_tzcnt_u64( m );
    src += 64;
    if( narrow ) continue;

    /* long run: skip 256 bytes at a time */
    while( src+256<end ) {
      __m512i v0 = _mm512_loadu_si512( src     );
      __m512i v1 = _mm512_loadu_si512( src+ 64 );
      __m512i v2 = _mm512_loadu_si512( src+128 );
      __m512i v3 = _mm512_loadu_si512( src+192 );
      __m512i t  = _mm512_or_si512( _mm512_or_si512( v0, v1 ), _mm512_or_si512( v2, v3 ) );
      if( ZLE_UNLIKELY( _mm512_test_epi8_mask( t, t ) ) ) {
        m = (uint64_t)_mm512_test_epi8_mask( v0, v0 ); if( m ) return src+    _tzcnt_u64( m );
        m = (uint64_t)_mm512_test_epi8_mask( v1, v1 ); if( m ) return src+ 64+_tzcnt_u64( m );
        m = (uint64_t)_mm512_test_epi8_mask( v2, v2 ); if( m ) return src+128+_tzcnt_u64( m );
        m = (uint64_t)_mm512_test_epi8_mask( v3, v3 );    return src+192+_tzcnt_u64( m );
      }
      src += 256;
    }
  } while( src+64<end );

  /* last 1..64 bytes, from the word ending at end */
  rem = (size_t)( end-src );
  v   = _mm512_loadu_si512( end-64 );
  m   = (uint64_t)_mm512_test_epi8_mask( v, v ) >> ( 64-rem );
  m   = _tzcnt_u64( m );
  return src+( m<rem ? m : rem );
}

/* zle_compress_core encodes n>64 bytes.  narrow is a compile time
   constant: 1 for n<255 (fields fit a byte, no 256 byte skips). */

static inline ZLE_FN_ALWAYS_INLINE size_t
zle_compress_core( uchar       * ZLE_RESTRICT dst,
                   uchar const * ZLE_RESTRICT src,
                   size_t                     n,
                   int                        narrow ) {
  uchar *       out  = dst;
  uchar const * end  = src+n;
  uchar const * base = src; /* start of pending literals */
  __m512i       v;
  uint64_t      d;
  size_t        rem;

  /* Walk the input one word at a time while a full pair mask
     (src+65<=end) fits. */
  do {
    d = zle_pairs( src );
    if( !d ) { /* not ZLE_UNLIKELY: an out of line block is slower here */
      /* Check a second word before skipping ahead.  Most accounts are
         too short for a skip to pay off. */
      src += 64;
      if( ZLE_UNLIKELY( src+64>=end ) ) break;
      d = zle_pairs( src );
      if( !d ) {
        src += 64;
        if( narrow ) continue;

        /* long literal run: skip 256 bytes at a time */
        while( src+256<end ) {
          __m512i v0 = _mm512_or_si512( _mm512_loadu_si512( src     ), _mm512_loadu_si512( src+  1 ) );
          __m512i v1 = _mm512_or_si512( _mm512_loadu_si512( src+ 64 ), _mm512_loadu_si512( src+ 65 ) );
          __m512i v2 = _mm512_or_si512( _mm512_loadu_si512( src+128 ), _mm512_loadu_si512( src+129 ) );
          __m512i v3 = _mm512_or_si512( _mm512_loadu_si512( src+192 ), _mm512_loadu_si512( src+193 ) );
          __m512i t  = _mm512_min_epu8( _mm512_min_epu8( v0, v1 ), _mm512_min_epu8( v2, v3 ) );
          if( ZLE_UNLIKELY( _mm512_testn_epi8_mask( t, t ) ) ) {
            d = (uint64_t)_mm512_testn_epi8_mask( v0, v0 ); if( d ) break;
            src += 64;
            d = (uint64_t)_mm512_testn_epi8_mask( v1, v1 ); if( d ) break;
            src += 64;
            d = (uint64_t)_mm512_testn_epi8_mask( v2, v2 ); if( d ) break;
            src += 64;
            d = (uint64_t)_mm512_testn_epi8_mask( v3, v3 ); /* nonzero */
            break;
          }
          src += 256;
        }
        if( !d ) continue;
      }
    }

    /* Emit every run in this word */
    for(;;) {
      uchar const * zpos = src+_tzcnt_u64( d );
      uchar const * zend;
      if( ZLE_UNLIKELY( __builtin_add_overflow( d, _blsi_u64( d ), &d ) ) ) {
        /* run continues into the next word */
        src += 64;
        zend = zle_zero_end( src, end, narrow );
        if( ( (size_t)( zend-zpos )>=ZLE_MIN_Z ) | ( zend==end ) ) {
          out  = zle_emit( out, base, zpos, zend, narrow );
          base = zend;
          if( zend==end ) return (size_t)( out-dst );
        }
        src = zend;
        break;
      }

      zend = src+_tzcnt_u64( d )+1; /* <end */
      if( (size_t)( zend-zpos )>=ZLE_MIN_Z ) {
        out  = zle_emit( out, base, zpos, zend, narrow );
        base = zend;
      }

      d = _blsr_u64( d ); /* clear the carry bit */
      if( !d ) { src += 64; break; }
    }
  } while( src+64<end );

  /* Last 1..64 bytes, from the word ending at end.  Its highest bit has
     no pair, so the carry stays in the word.  d==0 runs once with a run
     past end that is never emitted. */
  rem = (size_t)( end-src );
  v   = _mm512_loadu_si512( end-64 );
  d   = (uint64_t)_mm512_testn_epi8_mask( v, v ) >> ( 64-rem );
  d  &= d>>1;

  do {
    uchar const * zpos = src+_tzcnt_u64( d );
    uchar const * zend;
    d   += _blsi_u64( d );
    zend = src+_tzcnt_u64( d )+1;
    if( ( (size_t)( zend-zpos )>=ZLE_MIN_Z ) | ( zend==end ) ) {
      out  = zle_emit( out, base, zpos, zend, narrow );
      base = zend;
    }
    d = _blsr_u64( d );
  } while( d );

  if( base<end ) { /* last token */
    size_t L = (size_t)( end-base );
    out = zle_put_int( out, L, narrow );
    zle_copy( out, base, L, narrow );
    out += L;
  }
  return (size_t)( out-dst );
}

static size_t
zle_compress_small( uchar       * ZLE_RESTRICT dst,
                    uchar const * ZLE_RESTRICT src,
                    size_t                     n ) {
  return zle_compress_core( dst, src, n, 1 );
}

static size_t ZLE_FN_NOINLINE
zle_compress_big( uchar       * ZLE_RESTRICT dst,
                  uchar const * ZLE_RESTRICT src,
                  size_t                     n ) {
  return zle_compress_core( dst, src, n, 0 );
}

size_t
ZLE_compress( void       * ZLE_RESTRICT dst,
              void const * ZLE_RESTRICT src,
              size_t                    src_sz ) {
  uchar *       d = (uchar *)dst;
  uchar const * s = (uchar const *)src;
  if( ZLE_UNLIKELY( !src_sz      ) ) return 0;
  if( ZLE_LIKELY  ( src_sz<=64   ) ) return zle_compress_tiny ( d, s, src_sz );
  if( ZLE_LIKELY  ( src_sz< 255  ) ) return zle_compress_small( d, s, src_sz );
  return                                    zle_compress_big  ( d, s, src_sz );
}

#else /* !ZLE_AVX512 */

#ifdef ZLE_NEON

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
          size_t                     sz,
          int                        narrow ) {
  size_t k = 0;
  (void)narrow;
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
zle_zero( uchar * d,
          size_t  sz ) {
  uint64_t zero = 0;
  if( ZLE_UNLIKELY( sz>=512 ) ) { memset( d, 0, sz ); return; }
  if( ZLE_LIKELY( sz>=16 ) ) {
    uint8x16_t z = vdupq_n_u8( 0 );
    size_t     k = 0;
    do { vst1q_u8( d+k, z ); k += 16; } while( k+16<=sz );
    vst1q_u8( d+sz-16, z );
    return;
  }
  if     ( sz>=8 ) { memcpy( d, &zero, 8 ); memcpy( d+sz-8, &zero, 8 ); }
  else if( sz>=4 ) { memcpy( d, &zero, 4 ); memcpy( d+sz-4, &zero, 4 ); }
  else if( sz>=2 ) { memcpy( d, &zero, 2 ); memcpy( d+sz-2, &zero, 2 ); }
  else if( sz    ) d[0] = (uchar)0;
}

#else /* !ZLE_NEON */

static inline void
zle_copy( uchar       * ZLE_RESTRICT d,
          uchar const * ZLE_RESTRICT s,
          size_t                     sz,
          int                        narrow ) {
  (void)narrow;
  if( sz ) memcpy( d, s, sz );
}

static inline void
zle_zero( uchar * d,
          size_t  sz ) {
  if( sz ) memset( d, 0, sz );
}

#endif /* ZLE_NEON */

/* Portable encoder, same output as the AVX-512 one.  Never reads past
   src+src_sz. */

size_t
ZLE_compress( void       * ZLE_RESTRICT dst,
              void const * ZLE_RESTRICT src,
              size_t                    src_sz ) {
  uchar *       out  = (uchar *)dst;
  uchar const * p    = (uchar const *)src;
  uchar const * end  = p+src_sz;
  uchar const * base = p; /* start of pending literals */

  while( p<end ) {
    uchar const * zpos = (uchar const *)memchr( p, 0, (size_t)( end-p ) );
    uchar const * zend;
    size_t        Z;
    if( !zpos ) break;
    zend = zpos+1;
    while( zend<end && !*zend ) zend++;
    Z = (size_t)( zend-zpos );
    if( Z>=ZLE_MIN_Z || ( Z==2 && zend==end ) ) {
      size_t L = (size_t)( zpos-base );
      out = zle_put_int( out, L, 0 );
      zle_copy( out, base, L, 0 );
      out  = zle_put_int( out+L, Z, 0 );
      base = zend;
    }
    p = zend;
  }

  if( base<end ) { /* last token */
    size_t L = (size_t)( end-base );
    out = zle_put_int( out, L, 0 );
    zle_copy( out, base, L, 0 );
    out += L;
  }
  return (size_t)( out-(uchar *)dst );
}

#endif /* ZLE_AVX512 */

/* The decoder runs until src is exhausted; the last token is the one
   whose literals reach src_sz.  Lengths read from src are only ever
   compared against remaining space, never added to a pointer first. */

int64_t
ZLE_decompress( void       * ZLE_RESTRICT dst,
                size_t                    dst_max,
                void const * ZLE_RESTRICT src,
                size_t                    src_sz ) {
  uchar *       out     = (uchar *)dst;
  uchar *       dst_end = out+dst_max;
  uchar const * in      = (uchar const *)src;
  uchar const * in_end  = in+src_sz;

  if( ZLE_UNLIKELY( !src_sz ) ) return 0;

  for(;;) {
    uint64_t L, Z;

    /* read literal count L */
    L = *in++;
    if( ZLE_UNLIKELY( L==ZLE_ESC ) ) {
      in = zle_get_int( in, in_end, &L );
      if( ZLE_UNLIKELY( !in ) ) return ZLE_ERR_CORRUPT;
    }

    /* copy L literals from stream */
    if( ZLE_UNLIKELY( L>(uint64_t)( dst_end-out ) ) ) return ZLE_ERR_SPACE;
    if( ZLE_UNLIKELY( L>(uint64_t)( in_end-in   ) ) ) return ZLE_ERR_CORRUPT;
    zle_copy( out, in, (size_t)L, 0 );
    out += L;
    in  += L;
    if( in==in_end ) break; /* last token */

    /* read zero count Z */
    Z = *in++;
    if( ZLE_UNLIKELY( Z==ZLE_ESC ) ) {
      in = zle_get_int( in, in_end, &Z );
      if( ZLE_UNLIKELY( !in ) ) return ZLE_ERR_CORRUPT;
    }

    /* write Z zeroes */
    if( ZLE_UNLIKELY( Z>(uint64_t)( dst_end-out ) ) ) return ZLE_ERR_SPACE;
    zle_zero( out, (size_t)Z );
    out += Z;
    if( in==in_end ) break;
  }

  return (int64_t)( out-(uchar *)dst );
}

char const *
ZLE_strerror( int64_t res ) {
  switch( res ) {
  case ZLE_ERR_SPACE:   return "out of buffer space";
  case ZLE_ERR_CORRUPT: return "malformed compressed data";
  case 0:               return "empty";
  default:              return res>0 ? "ok" : "unknown";
  }
}
