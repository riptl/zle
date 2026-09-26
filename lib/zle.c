#include "zle.h"

#include <string.h> /* memcpy, memset */

#if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VBMI2__) && \
    defined(__BMI__) && defined(__BMI2__) && ( defined(__GNUC__) || defined(__clang__) )
#include <immintrin.h>
#define ZLE_AVX512 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ZLE_LIKELY(c)   __builtin_expect( !!(c), 1L )
#define ZLE_UNLIKELY(c) __builtin_expect( !!(c), 0L )
#else
#define ZLE_LIKELY(c)   (c)
#define ZLE_UNLIKELY(c) (c)
#endif

/* ZLE_SANITIZE is 1 when building with ASan or MSan.  The fast paths
   below read a few bytes past their buffers, within the page, which a
   sanitizer cannot tell from a real overrun, so they step aside for the
   exact ones. */

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

/* WIRE FORMAT

   The input is cut into blocks of 64 bytes; the last block holds the
   remaining 1..64.  Blocks are grouped into chunks, and a chunk keeps
   its control bytes and its literal bytes in two runs rather than
   interleaved, so that neither side has to walk a variable length field
   to find where the next one starts:

     blob   := chunk+                       (empty input -> empty blob)
     chunk  := hdr[2] ctl[ctl_sz] lit[...]
     hdr    := u16le: ctl_sz<<7 | (rem-1)<<1 | last
     ctl    := group+                       (covers this chunk's blocks)
     lit    := the literal bytes of this chunk's blocks, in order

   rem is the size of the blob's very last block (1..64) and last is set
   on the chunk that holds it.  A chunk takes blocks until its control
   field would not hold another span of them (ctl_sz reaches ZLE_CTL, and
   a span adds at most 288) or its ZLE_SPAN byte literal run is full, so
   ctl_sz<=488 and the three fields fit one u16.

   A group describes one block, or a run of trivial blocks.  Its first
   byte c selects the form:

     c&0x40==0  position list: c and the bytes that follow are byte
                positions inside the block, bit 7 set on the last one.
                Positions are strictly increasing and alternate: the
                first starts a zero run, the second ends it, and so on.
                The block's literals are the bytes outside those runs,
                read from lit in order.  At most 4 positions (below).
     c==0x7F    mask mode: the next 8 bytes are a little endian bit mask
                of the block's literal bytes.
     0x40..0x7E (c-0x3F) blocks, 1..63, that are all literal.
     0xC0..0xFF (c-0xBF) blocks, 1..64, that are all zero.

   A zero run is coded only if it is at least 2 bytes long and lies
   entirely inside its block; single zeros, and the 1 byte tails of runs
   that straddle a block boundary, stay literals.  The coder switches to
   mask mode once a block has ZLE_MASK_MIN transitions, so a position
   list is never longer than 4 bytes and a group never longer than 9.
   The mask costs 9 bytes against a list's 5 to 8 -- 0.2% of the output
   -- and buys a group whose length neither side has to find by scanning.

   The block is the unit of work on both sides: the encoder advances the
   input by 64 bytes per block and the decoder advances the output by 64
   bytes per block, while the other cursor advances by a popcount of the
   block's mask, never by a length read out of the stream.  That is
   where the speed comes from: the dense shapes carry no serial chain
   through the data at all.

   This is incompatible with 1.x and 2.x streams. */

#define ZLE_SPAN     ((size_t)2048) /* literal bytes a chunk can hold */
#define ZLE_CTL      ((size_t) 200) /* control bytes at which a chunk stops taking spans */
#define ZLE_MASK_MIN ((size_t)   5) /* transitions at which mask mode wins */

#if defined(__GNUC__) || defined(__clang__)
#define zle_ctz( x )    ((size_t)__builtin_ctzll( x ))
#define zle_popcnt( x ) ((size_t)__builtin_popcountll( x ))
#else
static inline size_t
zle_ctz( uint64_t x ) {
  size_t n = 0;
  for( ; !( x & 1 ); x >>= 1 ) n++;
  return n;
}

static inline size_t
zle_popcnt( uint64_t x ) {
  size_t n = 0;
  for( ; x; x &= x-1 ) n++;
  return n;
}
#endif

static inline size_t
zle_min( size_t a,
         size_t b ) {
  return a<b ? a : b;
}

#ifdef ZLE_AVX512

/* zle_iota is the byte index of every lane, the source the encoder
   compresses a transition mask down into a position list from. */

static uchar const zle_iota[ 64 ] __attribute__((aligned(64))) = {
   0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,
  16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
  32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,
  48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
};

static inline uint64_t
zle_load8( uchar const * p ) {
  uint64_t v;
  memcpy( &v, p, 8 );
  return v;
}

static inline void
zle_store8( uchar *  p,
            uint64_t v ) {
  memcpy( p, &v, 8 );
}

/* x86 is little endian, so the header's u16 goes out as one store. */

static inline void
zle_store2( uchar *  p,
            uint64_t v ) {
  uint16_t h = (uint16_t)v;
  memcpy( p, &h, 2 );
}

size_t
ZLE_compress( void       * ZLE_RESTRICT dst,
              void const * ZLE_RESTRICT src,
              size_t                    src_sz ) {

  /* The chunk's literals are built here and copied out at the chunk's
     end: ZLE_SPAN of span, 64 for the block stores that write a whole
     block's worth regardless of the mask, and 64 for the short last
     block, which the span does not budget for. */

  uchar lit_buf[ ZLE_SPAN+128 ] __attribute__((aligned(64)));

  uchar const * s;
  uchar const * s_end;
  uchar const * blk_end;  /* the span's last block boundary */
  uchar *       out;      /* control cursor */
  uchar *       ctl;      /* this chunk's control start */
  uchar *       lit;      /* literal scratch cursor */
  __m512i       iota;
  __m512i       v;        /* the block's bytes */
  uint64_t      z, r2, b, T, keep;
  size_t        t, span, hdr_last;

  if( ZLE_UNLIKELY( !src_sz ) ) return 0;

  s        = (uchar const *)src;
  s_end    = s + src_sz;
  out      = (uchar *)dst;
  iota     = _mm512_load_si512( zle_iota );
  hdr_last = ( ( ( src_sz-1 ) & 63 )<<1 ) | 1; /* (rem-1)<<1 | last */

chunk:
  out += 2;                                     /* the chunk header */
  ctl  = out;
  lit  = lit_buf;
  span = zle_min( (size_t)( s_end-s ) & ~(size_t)63, ZLE_SPAN );
  goto span_short;

span_next:                                      /* a later span of the same chunk */
  {
    size_t avail = (size_t)( s_end-s ) & ~(size_t)63;    /* whole blocks left */
    size_t room  = (size_t)( lit_buf+ZLE_SPAN - lit );   /* literal scratch still free */
    span = avail<=room ? avail : ( room & ~(size_t)63 );
  }

span_short:
  blk_end = s + span;
  if( ZLE_UNLIKELY( s>=blk_end ) ) goto tail;   /* no full block left */

blk:                                            /* a full block at s */
  v  = _mm512_loadu_si512( s );
  z  = (uint64_t)_mm512_testn_epi8_mask( v, v );/* zero bytes */
  r2 = z & (z>>1);                              /* 2 zeros start here */
  if( ZLE_UNLIKELY( !r2 ) ) goto alllit;

blk_r2:                                         /* r2 nonzero, v the block */
  b = r2 | (r2<<1);                             /* byte is in a coded run */
  if( ZLE_UNLIKELY( !~b ) ) goto allzero;
  T    = b ^ (b<<1);                            /* transitions */
  keep = ~b;
  t    = zle_popcnt( T );
  if( t>=ZLE_MASK_MIN ) {                       /* mask mode */
    out[0] = (uchar)0x7F;
    zle_store8( out+1, keep );
    out += 9;
  } else if( t==1 ) {
    /* One zero run, reaching the end of the block: one control byte,
       and the literals are the block's own prefix, so neither needs a
       compress. */
    size_t p0 = zle_ctz( T );
    *out++ = (uchar)( p0 | 0x80 );
    _mm512_storeu_si512( lit, v );
    lit += p0;
    goto blk_next;
  } else {
    _mm512_mask_compressstoreu_epi8( out, (__mmask64)T, iota );           /* the positions */
    out[ t-1 ] = (uchar)( ( 63-(size_t)_lzcnt_u64( T ) ) | 0x80 );        /* terminate the list */
    out += t;
  }
  _mm512_mask_compressstoreu_epi8( lit, (__mmask64)keep, v );
  lit += zle_popcnt( keep );

blk_next:
  s += 64;
  if( ZLE_LIKELY( s<blk_end ) ) goto blk;

blkdone:                                        /* the chunk's span is done */
  if( s>=s_end                         ) goto chunk_end;
  if( (size_t)( s_end-s )<64           ) goto tail;       /* a short last block, same chunk */
  if( (size_t)( out-ctl )>=ZLE_CTL     ) goto chunk_end;  /* a span adds at most 288 bytes */
  if( lit>=lit_buf+ZLE_SPAN-63         ) goto chunk_end;  /* the literal scratch is full */
  goto span_next;

  /* A block with no coded zero run.  Runs of them share one group, and
     the loop's test doubles as the next block's, so falling out of it
     re-enters the block body with the mask already in hand. */

alllit:
  {
    size_t cnt = 0;
    for(;;) {
      _mm512_storeu_si512( lit, v );
      lit += 64;
      s   += 64;
      cnt++;
      if( s>=blk_end ) { *out++ = (uchar)( 63+cnt ); goto blkdone; }
      v  = _mm512_loadu_si512( s );
      z  = (uint64_t)_mm512_testn_epi8_mask( v, v );
      r2 = z & (z>>1);
      if( r2 ) break;
    }
    *out++ = (uchar)( 63+cnt );
  }
  goto blk_r2;

  /* An all zero block.  Scan 256 bytes at a time for the end of the
     run, out to the 64 blocks one group can code, regardless of where
     the chunk's span ends -- the run costs the chunk no literal
     scratch. */

allzero:
  {
    uchar const * run = s;
    uchar const * lim = s + zle_min( (size_t)( s_end-s ) & ~(size_t)63, 64*64 );
    s += 64;
    while( s+256<=lim ) {
      __m512i o = _mm512_loadu_si512( s );
      o = _mm512_or_si512( o, _mm512_loadu_si512( s+ 64 ) );
      o = _mm512_or_si512( o, _mm512_loadu_si512( s+128 ) );
      o = _mm512_or_si512( o, _mm512_loadu_si512( s+192 ) );
      if( _mm512_test_epi8_mask( o, o ) ) break;
      s += 256;
    }
    while( s<lim ) {
      __m512i w = _mm512_loadu_si512( s );
      if( _mm512_test_epi8_mask( w, w ) ) break;
      s += 64;
    }
    *out++ = (uchar)( 191 + (size_t)( s-run )/64 );  /* blocks in the run */
  }
  if( s<blk_end ) goto blk;
  goto blkdone;

  /* The last block, 1..63 bytes.  All zero is tested first: an account
     that ends in zeros is the common case.  The load reads up to
     align_up( src_sz, 64 ). */

tail:
  {
    size_t    bs   = (size_t)( s_end-s );
    uint64_t  full = _bzhi_u64( ~(uint64_t)0, (unsigned)bs );
    uint64_t  m;

#   if ZLE_SANITIZE
    v = _mm512_maskz_loadu_epi8( (__mmask64)full, s ); /* sanitizers would flag the read past bs */
#   else
    v = _mm512_loadu_si512( s );
#   endif
    m = (uint64_t)_mm512_test_epi8_mask( v, v );  /* nonzero bytes */
    s = s_end;

    if( !( m & full ) ) { *out++ = (uchar)0xC0; goto chunk_end; }  /* all zero, any size */

    z  = (~m) & full;
    r2 = z & (z>>1);
    if( !r2 ) {                                 /* no coded zero run */
      *out++ = (uchar)0x40;
      _mm512_mask_compressstoreu_epi8( lit, (__mmask64)full, v ); /* the whole block */
      lit += bs;
      goto chunk_end;
    }

    b    = ( r2 | (r2<<1) ) & full;
    T    = ( b ^ (b<<1) ) & full;
    keep = (~b) & full;
    t    = zle_popcnt( T );
    if( t>=ZLE_MASK_MIN ) {
      out[0] = (uchar)0x7F;
      zle_store8( out+1, keep );
      out += 9;
    } else {
      _mm512_mask_compressstoreu_epi8( out, (__mmask64)T, iota );
      out[ t-1 ] = (uchar)( ( 63-(size_t)_lzcnt_u64( T ) ) | 0x80 );
      out += t;
    }
    _mm512_mask_compressstoreu_epi8( lit, (__mmask64)keep, v );
    lit += zle_popcnt( keep );
  }

chunk_end:
  {
    size_t   ctl_sz = (size_t)( out-ctl );
    size_t   lit_sz = (size_t)( lit-lit_buf );
    uint64_t h      = (uint64_t)( ( ctl_sz<<7 ) | ( s>=s_end ? hdr_last : 126 ) ); /* 126: rem 64, last 0 */
    size_t   i;

    zle_store2( ctl-2, h );

#   if ZLE_SANITIZE
    for( i=0; i+64<=lit_sz; i+=64 ) _mm512_storeu_si512( out+i, _mm512_load_si512( lit_buf+i ) );
    if( i<lit_sz ) {
      __mmask64 mm = (__mmask64)_bzhi_u64( ~(uint64_t)0, (unsigned)( lit_sz-i ) );
      _mm512_mask_storeu_epi8( out+i, mm, _mm512_maskz_loadu_epi8( mm, lit_buf+i ) );
    }
#   else
    if( lit_sz<=64 ) {
      if( lit_sz ) _mm512_storeu_si512( out, _mm512_load_si512( lit_buf ) ); /* one block covers most chunks */
    } else {
      for( i=0; i<lit_sz; i+=64 )
        _mm512_storeu_si512( out+i, _mm512_load_si512( lit_buf+i ) );
    }
#   endif
    out += lit_sz;
  }
  if( s<s_end ) goto chunk;

  return (size_t)( out-(uchar *)dst );
}

/* A group is emitted with sloppy 64 byte stores when it is not the last
   group of its chunk and both cursors have a block of slack.  Otherwise
   it goes through the mask path, which expands the block into one
   register and stores exactly its bytes, so nothing is ever written past
   dst+dst_max or read past a page the input does not reach. */

int64_t
ZLE_decompress( void       * ZLE_RESTRICT dst,
                size_t                    dst_max,
                void const * ZLE_RESTRICT src,
                size_t                    src_sz ) {

  /* A chunk whose control bytes end within 8 of the blob is run over a
     padded copy: ctl_sz's 511 max, rounded up by a block because the
     copy stores 64 at a time, plus the 8 the group loop reads past its
     last control byte. */

  uchar ctl_buf[ 584 ];

  uchar       * const out_beg  = (uchar *)dst;
  uchar       * const out_end  = out_beg + dst_max;
  uchar const * const blob_end = (uchar const *)src + src_sz;
  __m512i       const zero     = _mm512_setzero_si512();

  uchar *       out;
  uchar const * p;
  uchar const * ctl;
  uchar const * ctl_end;
  uchar const * lit;
  uchar const * nxt;
  uintptr_t     lit_lim;  /* the literal cursor's sloppy read limit */
  size_t        bs_last;  /* the size of this chunk's last block */
  uint64_t      w;        /* the 8 control bytes at ctl */
  uint64_t      keep;
  size_t        c, bs, nlit, cnt, tm1;

  if( ZLE_UNLIKELY( !src_sz ) ) return 0;

  out = out_beg;
  p   = (uchar const *)src;

  /* The sloppy paths read up to 128 bytes past the literal cursor.
     When the blob's last byte is at least that far from the end of its
     page, those reads touch no page the blob does not already touch,
     and the cursor only has to stay inside the blob. */

# if ZLE_SANITIZE
  lit_lim = 0; /* every path takes its exact form */
# else
  lit_lim = (uintptr_t)blob_end - ( ( ( (uintptr_t)( blob_end-1 ) ) & 4095 )>=3968 ? 128 : 0 );
# endif

chunk:                                          /* p < blob_end */
  ctl = p+2;
  if( ZLE_UNLIKELY( ctl>blob_end ) ) goto ecorrupt;
  {
    size_t h      = (size_t)p[0] | ( (size_t)p[1]<<8 );
    size_t ctl_sz = h>>7;
    if( ZLE_UNLIKELY( !ctl_sz ) ) goto ecorrupt;
    lit = ctl + ctl_sz;
    if( ZLE_UNLIKELY( lit>blob_end ) ) goto ecorrupt;
    ctl_end = lit;
    bs_last = ( h & 1 ) ? ( ( ( h>>1 ) & 63 )+1 ) : 64; /* the blob's last block */
  }
  /* the 8 byte control reads must fit */
  if( ZLE_UNLIKELY( (uintptr_t)ctl_end-120 > lit_lim ) ) goto ctlcopy;
  w = zle_load8( ctl );

grp:                                            /* ctl < ctl_end, w the control bytes at ctl */
  c = (size_t)( w & 0xFF );
  if( c & 0x40 ) goto special;

  /* position list */
  tm1 = (size_t)_tzcnt_u64( w & 0x8080808080808080ULL )>>3;  /* t-1, 8 if the list has no terminator */
  w  &= 0x3F3F3F3F3F3F3F3FULL;                    /* a corrupt stream can set bit 6 */
  nxt = ctl + tm1 + 1;
  if( ZLE_UNLIKELY( nxt>=ctl_end                 ) ) goto pos_exact; /* the chunk's last group */
  if( ZLE_UNLIKELY( (size_t)( out_end-out )<128  ) ) goto pos_exact; /* output slack */
  if( ZLE_UNLIKELY( (uintptr_t)lit>lit_lim       ) ) goto pos_exact; /* literal slack */
  ctl = nxt;

  /* Dispatched by hand rather than by a switch: a jump table here is an
     indirect branch on the control stream, which predicts badly.  Two
     zero runs is the common case, so it goes first. */

  if( ZLE_LIKELY( tm1==2 ) ) goto t3;
  if( tm1<2 ) { if( tm1 ) goto t2; goto t1; }
  if( tm1==3 ) goto t4;
  bs = 64;                                      /* only a corrupt stream */
  goto pos_keep;

t3: { /* two zero runs, the second reaching the end of the block */
    size_t  p0 = (size_t)( w & 0xFF ), p1 = (size_t)( (w>>8) & 0xFF ), p2 = (size_t)( (w>>16) & 0xFF );
    __m512i v0 = _mm512_loadu_si512( lit );
    __m512i v1 = _mm512_loadu_si512( lit+p0 );
    w = zle_load8( ctl );                       /* the next group, early */
    _mm512_storeu_si512( out,    v0   );
    _mm512_storeu_si512( out+p0, zero );
    _mm512_storeu_si512( out+p1, v1   );
    _mm512_storeu_si512( out+p2, zero );
    lit += p0 + p2 - p1;
    out += 64;
    goto grp;
  }

t1: { /* one zero run reaching the end of the block */
    size_t p0 = (size_t)( w & 0xFF );
    w = zle_load8( ctl );
    _mm512_storeu_si512( out,    _mm512_loadu_si512( lit ) );
    _mm512_storeu_si512( out+p0, zero );
    lit += p0;
    out += 64;
    goto grp;
  }

t2: { /* one zero run inside the block */
    size_t  p0 = (size_t)( w & 0xFF ), p1 = (size_t)( (w>>8) & 0xFF );
    __m512i v0 = _mm512_loadu_si512( lit );
    __m512i v1 = _mm512_loadu_si512( lit+p0 );
    w = zle_load8( ctl );
    _mm512_storeu_si512( out,    v0   );
    _mm512_storeu_si512( out+p0, zero );
    _mm512_storeu_si512( out+p1, v1   );
    lit += p0 + 64 - p1;
    out += 64;
    goto grp;
  }

t4: { /* two zero runs inside the block */
    size_t  p0 = (size_t)( w & 0xFF ), p1 = (size_t)( (w>>8) & 0xFF );
    size_t  p2 = (size_t)( (w>>16) & 0xFF ), p3 = (size_t)( (w>>24) & 0xFF );
    __m512i v0 = _mm512_loadu_si512( lit );
    __m512i v1 = _mm512_loadu_si512( lit+p0 );
    size_t  k;
    w = zle_load8( ctl );
    _mm512_storeu_si512( out,    v0   );
    _mm512_storeu_si512( out+p0, zero );
    _mm512_storeu_si512( out+p1, v1   );
    _mm512_storeu_si512( out+p2, zero );
    k = ( p0 + p2 - p1 ) & 63;                  /* a corrupt stream can wrap it */
    _mm512_storeu_si512( out+p3, _mm512_loadu_si512( lit+k ) );
    lit += k + 64 - p3;
    out += 64;
    goto grp;
  }

  /* A position list emitted exactly, when the sloppy paths have no
     room.  The literal mask is the xor of ( ~0 << p ) over the list,
     complemented.  Five or more positions: only a corrupt stream gets
     here, since the coder switches to mask mode at five transitions. */

pos_exact:                                      /* nxt = control after the group */
  bs  = nxt<ctl_end ? 64 : bs_last;             /* the chunk's last group */
  ctl = nxt;

pos_keep:
  {
    size_t   n = zle_min( tm1+1, 8 );
    uint64_t m = 0;
    size_t   i;
    for( i=0; i<n; i++ ) m ^= ~(uint64_t)0<<( ( w>>(8*i) ) & 63 );
    keep = ~m;
  }
  goto emit;

  /* Special groups.  One all zero block and one all literal block are
     half of all groups, so they get their own short paths. */

special:
  if( c==0xC0 ) goto z1;
  if( c> 0xC0 ) goto zrun;
  if( c==0x40 ) goto l1;
  if( c==0x7F ) goto mask;

lrun:
  cnt = ( c-0x3F )<<6;                          /* literal bytes */
  nxt = ctl+1;
  ctl = nxt;
  if( ZLE_UNLIKELY( nxt>=ctl_end ) ) cnt += bs_last-64; /* the blob's last block may be short */
  if( ZLE_UNLIKELY( cnt>(size_t)( out_end-out  ) ) ) goto espace;
  if( ZLE_UNLIKELY( cnt>(size_t)( blob_end-lit ) ) ) goto ecorrupt;
  {
    size_t whole = cnt & ~(size_t)63;
    size_t i     = 0;
    for( ; i+256<=whole; i+=256 ) {
      __m512i v0 = _mm512_loadu_si512( lit+i     );
      __m512i v1 = _mm512_loadu_si512( lit+i+ 64 );
      __m512i v2 = _mm512_loadu_si512( lit+i+128 );
      __m512i v3 = _mm512_loadu_si512( lit+i+192 );
      _mm512_storeu_si512( out+i,     v0 );
      _mm512_storeu_si512( out+i+ 64, v1 );
      _mm512_storeu_si512( out+i+128, v2 );
      _mm512_storeu_si512( out+i+192, v3 );
    }
    for( ; i<whole; i+=64 ) _mm512_storeu_si512( out+i, _mm512_loadu_si512( lit+i ) );
    if( cnt & 63 ) {
      __mmask64 mm = (__mmask64)_bzhi_u64( ~(uint64_t)0, (unsigned)( cnt & 63 ) );
      _mm512_mask_storeu_epi8( out+whole, mm, _mm512_maskz_loadu_epi8( mm, lit+whole ) );
    }
    lit += cnt;
    out += cnt;
  }
  goto grp_next;

zrun:
  cnt = ( c-0xBF )<<6;                          /* zero bytes */
  nxt = ctl+1;
  ctl = nxt;
  if( ZLE_UNLIKELY( nxt>=ctl_end ) ) cnt += bs_last-64;
  if( ZLE_UNLIKELY( cnt>(size_t)( out_end-out ) ) ) goto espace;
  {
    size_t whole = cnt & ~(size_t)63;
    size_t i     = 0;
    for( ; i+256<=whole; i+=256 ) {
      _mm512_storeu_si512( out+i,     zero );
      _mm512_storeu_si512( out+i+ 64, zero );
      _mm512_storeu_si512( out+i+128, zero );
      _mm512_storeu_si512( out+i+192, zero );
    }
    for( ; i<whole; i+=64 ) _mm512_storeu_si512( out+i, zero );
    if( cnt & 63 )
      _mm512_mask_storeu_epi8( out+whole, (__mmask64)_bzhi_u64( ~(uint64_t)0, (unsigned)( cnt & 63 ) ), zero );
    out += cnt;
  }
  goto grp_next;

z1:                                             /* one all zero block */
  nxt = ctl+1;
  if( ZLE_UNLIKELY( nxt>=ctl_end ) ) goto z1_last;
  if( ZLE_UNLIKELY( (size_t)( out_end-out )<128 ) ) goto zrun;
  ctl = nxt;
  w   = zle_load8( ctl );
  _mm512_storeu_si512( out, zero );
  out += 64;
  goto grp;

l1:                                             /* one all literal block */
  nxt = ctl+1;
  if( ZLE_UNLIKELY( nxt>=ctl_end ) ) goto l1_last;
  if( ZLE_UNLIKELY( (size_t)( out_end-out )<128 ) ) goto lrun;
  if( ZLE_UNLIKELY( (uintptr_t)lit>lit_lim      ) ) goto lrun;
  ctl = nxt;
  w   = zle_load8( ctl );
  _mm512_storeu_si512( out, _mm512_loadu_si512( lit ) );
  lit += 64;
  out += 64;
  goto grp;

  /* The same two as the chunk's last group: the block may be short, so
     they store exactly bs_last bytes. */

z1_last:
  ctl = nxt;
  if( ZLE_UNLIKELY( bs_last>(size_t)( out_end-out ) ) ) goto espace;
  _mm512_mask_storeu_epi8( out, (__mmask64)_bzhi_u64( ~(uint64_t)0, (unsigned)bs_last ), zero );
  out += bs_last;
  goto chunk_next;

l1_last:
  ctl = nxt;
  if( ZLE_UNLIKELY( bs_last>(size_t)( out_end-out  ) ) ) goto espace;
  if( ZLE_UNLIKELY( bs_last>(size_t)( blob_end-lit ) ) ) goto ecorrupt;
  {
    __mmask64 mm = (__mmask64)_bzhi_u64( ~(uint64_t)0, (unsigned)bs_last );
    _mm512_mask_storeu_epi8( out, mm, _mm512_maskz_loadu_epi8( mm, lit ) );
  }
  lit += bs_last;
  out += bs_last;
  goto chunk_next;

mask:                                           /* the block's literal mask is in the stream */
  nxt = ctl+9;
  if( ZLE_UNLIKELY( nxt>ctl_end ) ) goto ecorrupt;
  keep = zle_load8( ctl+1 );
  ctl  = nxt;
  if( ZLE_UNLIKELY( nxt==ctl_end ) ) { bs = bs_last; goto emit; } /* the chunk's last group */
  bs = 64;
  if( ZLE_UNLIKELY( (size_t)( out_end-out )<128 ) ) goto emit;
  if( ZLE_UNLIKELY( (uintptr_t)lit>lit_lim      ) ) goto emit;
  nlit = zle_popcnt( keep );
  w    = zle_load8( ctl );
  _mm512_storeu_si512( out, _mm512_maskz_expandloadu_epi8( (__mmask64)keep, lit ) );
  lit += nlit;
  out += 64;
  goto grp;

  /* Emit one block: keep the literal mask, bs the block size.  A full
     block needs no mask on its store and no clip on its keep, which is
     the common case. */

emit:
  if( ZLE_LIKELY( bs==64 ) ) {
    if( ZLE_UNLIKELY( (size_t)( out_end-out )<64 ) ) goto espace;
    nlit = zle_popcnt( keep );
    if( ZLE_UNLIKELY( nlit>(size_t)( blob_end-lit ) ) ) goto ecorrupt;
    _mm512_storeu_si512( out, _mm512_maskz_expandloadu_epi8( (__mmask64)keep, lit ) );
    lit += nlit;
    out += 64;
  } else {                                      /* the blob's last block */
    uint64_t full;
    if( ZLE_UNLIKELY( bs>(size_t)( out_end-out ) ) ) goto espace;
    full  = _bzhi_u64( ~(uint64_t)0, (unsigned)bs );
    keep &= full;
    nlit  = zle_popcnt( keep );
    if( ZLE_UNLIKELY( nlit>(size_t)( blob_end-lit ) ) ) goto ecorrupt;
    _mm512_mask_storeu_epi8( out, (__mmask64)full,
                             _mm512_maskz_expandloadu_epi8( (__mmask64)keep, lit ) );
    lit += nlit;
    out += bs;
  }

grp_next:
  if( ZLE_UNLIKELY( ctl>=ctl_end ) ) goto chunk_next;
  w = zle_load8( ctl );
  goto grp;

chunk_next:
  p = lit;
  if( p<blob_end ) goto chunk;
  return (int64_t)( out-out_beg );

  /* The control bytes end within 8 of the blob: run the group loop over
     a padded copy instead. */

ctlcopy:
  {
    size_t ctl_sz = (size_t)( ctl_end-ctl );
    size_t i      = 0;
    for( ; ctl_sz-i>=64; i+=64 )
      _mm512_storeu_si512( ctl_buf+i, _mm512_loadu_si512( ctl+i ) );
    _mm512_storeu_si512( ctl_buf+i,
                         _mm512_maskz_loadu_epi8( (__mmask64)_bzhi_u64( ~(uint64_t)0, (unsigned)( ctl_sz-i ) ),
                                                  ctl+i ) );
    memset( ctl_buf+ctl_sz, 0, 8 );             /* the reads past the last control byte */
    ctl     = ctl_buf;
    ctl_end = ctl_buf + ctl_sz;
    w       = zle_load8( ctl );
  }
  goto grp;

espace:
  return ZLE_ERR_SPACE;
ecorrupt:
  return ZLE_ERR_CORRUPT;
}

#else /* !ZLE_AVX512 */

/* zle_lits appends the bytes of the block at s selected by keep to out,
   in order, and returns the cursor past them.  zle_expand is its
   inverse: it scatters the literals at lit into the block at out, whose
   other bytes out already holds, and returns the cursor past the
   literals it read. */

static inline uchar *
zle_lits( uchar       * ZLE_RESTRICT out,
          uchar const * ZLE_RESTRICT s,
          uint64_t                   keep ) {
  size_t base = 0;
  while( keep ) {
    size_t p = zle_ctz( keep );
    size_t k;
    keep >>= p;
    base  += p;
    k      = ~keep ? zle_ctz( ~keep ) : 64-base;
    memcpy( out, s+base, k );
    out   += k;
    base  += k;
    keep   = k>=64 ? 0 : keep>>k;
  }
  return out;
}

static inline uchar const *
zle_expand( uchar       * ZLE_RESTRICT out,
            uchar const * ZLE_RESTRICT lit,
            uint64_t                   keep ) {
  size_t base = 0;
  while( keep ) {
    size_t p = zle_ctz( keep );
    size_t k;
    keep >>= p;
    base  += p;
    k      = ~keep ? zle_ctz( ~keep ) : 64-base;
    memcpy( out+base, lit, k );
    lit   += k;
    base  += k;
    keep   = k>=64 ? 0 : keep>>k;
  }
  return lit;
}

/* ZLE_LE is 1 when a 64 bit load can be read as bytes in index order,
   which is what zle_zeros' word trick assumes. */

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
# define ZLE_LE ( __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__ )
#elif defined(_M_X64) || defined(_M_IX86) || defined(_M_ARM64)
# define ZLE_LE 1
#else
# define ZLE_LE 0
#endif

/* zle_zeros returns the zero byte mask of the sz<=64 bytes at s: bit i
   is set iff s[i]==0.  Reads nothing past s+sz.

   The word at a time path marks every zero byte's high bit -- the add
   carries into it for every other byte, and stays inside its byte, so
   neighbours cannot borrow into the answer -- and then gathers those 8
   high bits into 8 adjacent ones with a multiply. */

static inline uint64_t
zle_zeros( uchar const * s,
           size_t        sz ) {
  uint64_t z = 0;
  size_t   i = 0;
#if ZLE_LE
  for( ; i+8<=sz; i+=8 ) {
    uint64_t w, t;
    memcpy( &w, s+i, 8 );
    t  = ~( ( ( w & 0x7F7F7F7F7F7F7F7FULL ) + 0x7F7F7F7F7F7F7F7FULL ) | w | 0x7F7F7F7F7F7F7F7FULL );
    z |= ( ( t * 0x0002040810204081ULL )>>56 )<<i;
  }
#endif
  for( ; i<sz; i++ ) z |= (uint64_t)( !s[i] )<<i;
  return z;
}

/* zle_zero_blk returns 1 if the 64 bytes at s are all zero. */

static inline int
zle_zero_blk( uchar const * s ) {
  uint64_t o = 0;
  size_t   i;
  for( i=0; i<64; i+=8 ) {
    uint64_t w;
    memcpy( &w, s+i, 8 );
    o |= w;
  }
  return !o;
}

/* zle_group writes the group of one block whose transition mask is T and
   whose literal mask is keep, and returns the cursor past it. */

static inline uchar *
zle_group( uchar *  out,
           uint64_t T,
           uint64_t keep ) {
  size_t t = zle_popcnt( T );
  size_t i;
  if( t>=ZLE_MASK_MIN ) {                       /* mask mode */
    *out++ = (uchar)0x7F;
    for( i=0; i<8; i++ ) *out++ = (uchar)( keep>>(8*i) );
    return out;
  }
  for( i=0; i<t; i++ ) {                        /* position list */
    size_t p = zle_ctz( T );
    T &= T-1;
    *out++ = (uchar)( p | ( i==t-1 ? 0x80 : 0 ) );
  }
  return out;
}

/* Portable encoder, byte for byte the same output as the AVX-512 one.
   Never reads past src+src_sz. */

size_t
ZLE_compress( void       * ZLE_RESTRICT dst,
              void const * ZLE_RESTRICT src,
              size_t                    src_sz ) {

  uchar lit_buf[ ZLE_SPAN+128 ];

  uchar       *       out = (uchar *)dst;
  uchar const * const s   = (uchar const *)src;
  size_t              pos = 0;
  size_t              rem;

  if( ZLE_UNLIKELY( !src_sz ) ) return 0;
  rem = ( ( src_sz-1 ) & 63 )+1;                /* the blob's last block */

  while( pos<src_sz ) {                         /* one chunk */
    uchar * hdr = out;
    uchar * ctl = out+2;
    uchar * lit = lit_buf;
    size_t  ctl_sz, lit_sz, last;
    uint64_t h;

    out = ctl;

    for(;;) {                                   /* one span of the chunk */
      size_t span = pos + zle_min( ( ZLE_SPAN - (size_t)( lit-lit_buf ) ) & ~(size_t)63,
                                   ( src_sz-pos ) & ~(size_t)63 );

      while( pos<span ) {                       /* full blocks */
        uint64_t z  = zle_zeros( s+pos, 64 );
        uint64_t r2 = z & (z>>1);               /* 2 zeros start here */
        uint64_t b, T, keep;

        if( !r2 ) {                             /* a run of all literal blocks */
          size_t cnt = 0;
          do {
            memcpy( lit, s+pos, 64 );
            lit += 64;
            pos += 64;
            cnt++;
            if( pos>=span ) break;
            z  = zle_zeros( s+pos, 64 );
            r2 = z & (z>>1);
          } while( !r2 );
          *out++ = (uchar)( 0x3F+cnt );
          continue;
        }

        b = r2 | (r2<<1);                       /* byte is in a coded run */
        if( !~b ) {                             /* a run of all zero blocks */
          size_t zlim = pos + zle_min( 64*64, ( src_sz-pos ) & ~(size_t)63 );
          size_t cnt  = 0;
          do {
            pos += 64;
            cnt++;
          } while( pos<zlim && zle_zero_blk( s+pos ) );
          *out++ = (uchar)( 0xBF+cnt );
          continue;
        }

        T    = b ^ (b<<1);                      /* transitions */
        keep = ~b;
        out  = zle_group( out, T, keep );
        lit  = zle_lits( lit, s+pos, keep );
        pos += 64;
      }

      if( pos<src_sz && src_sz-pos<64 ) {       /* the short last block, in this chunk */
        size_t   bs   = src_sz-pos;
        uint64_t full = ( (uint64_t)1<<bs )-1;
        uint64_t z    = zle_zeros( s+pos, bs );
        uint64_t r2   = z & (z>>1);
        uint64_t b, T, keep;

        if( z==full ) *out++ = (uchar)0xC0;     /* all zero, any size */
        else if( !r2 ) {                        /* no coded zero run */
          *out++ = (uchar)0x40;
          memcpy( lit, s+pos, bs );
          lit += bs;
        } else {
          b    = ( r2 | (r2<<1) ) & full;
          T    = ( b ^ (b<<1) ) & full;
          keep = (~b) & full;
          out  = zle_group( out, T, keep );
          lit  = zle_lits( lit, s+pos, keep );
        }
        pos = src_sz;
        break;
      }

      if( pos>=src_sz                                   ) break;
      if( (size_t)( out-ctl )>=ZLE_CTL                  ) break;  /* a span adds at most 288 bytes */
      if( (size_t)( lit-lit_buf )+64>ZLE_SPAN           ) break;  /* the literal scratch is full */
    }

    ctl_sz = (size_t)( out-ctl );
    lit_sz = (size_t)( lit-lit_buf );
    last   = pos>=src_sz;
    h      = (uint64_t)( ( ctl_sz<<7 ) | ( ( ( last ? rem : 64 )-1 )<<1 ) | last );
    hdr[0] = (uchar)h;
    hdr[1] = (uchar)(h>>8);
    memcpy( out, lit_buf, lit_sz );
    out += lit_sz;
  }

  return (size_t)( out-(uchar *)dst );
}

/* Portable decoder.  Reads nothing past src+src_sz and writes nothing
   past dst+dst_max, whatever the input. */

int64_t
ZLE_decompress( void       * ZLE_RESTRICT dst,
                size_t                    dst_max,
                void const * ZLE_RESTRICT src,
                size_t                    src_sz ) {
  uchar       *       out      = (uchar *)dst;
  uchar       * const out_end  = out + dst_max;
  uchar const *       p        = (uchar const *)src;
  uchar const * const blob_end = p + src_sz;

  if( ZLE_UNLIKELY( !src_sz ) ) return 0;

  while( p<blob_end ) {                         /* one chunk */
    uchar const * ctl;
    uchar const * ctl_end;
    uchar const * lit;
    size_t        h, ctl_sz, bs_last;

    if( ZLE_UNLIKELY( (size_t)( blob_end-p )<2 ) ) return ZLE_ERR_CORRUPT;
    h       = (size_t)p[0] | ( (size_t)p[1]<<8 );
    ctl     = p+2;
    ctl_sz  = h>>7;
    if( ZLE_UNLIKELY( !ctl_sz || ctl_sz>(size_t)( blob_end-ctl ) ) ) return ZLE_ERR_CORRUPT;
    ctl_end = ctl + ctl_sz;
    lit     = ctl_end;
    bs_last = ( h & 1 ) ? ( ( ( h>>1 ) & 63 )+1 ) : 64;

    while( ctl<ctl_end ) {                      /* one group */
      size_t   c = (size_t)*ctl;
      uint64_t keep;
      size_t   bs, nlit;

      if( !( c & 0x40 ) ) {                     /* position list */
        uint64_t m = 0;
        size_t   k = 0;
        for(;;) {
          size_t v;
          if( ZLE_UNLIKELY( ctl+k>=ctl_end ) ) return ZLE_ERR_CORRUPT;
          v  = (size_t)ctl[k];
          m ^= ~(uint64_t)0<<( v & 63 );
          k++;
          if( v & 0x80 ) break;
          if( ZLE_UNLIKELY( k>8 ) ) return ZLE_ERR_CORRUPT;
        }
        ctl += k;
        keep = ~m;
      } else if( c==0x7F ) {                    /* mask mode */
        size_t i;
        if( ZLE_UNLIKELY( (size_t)( ctl_end-ctl )<9 ) ) return ZLE_ERR_CORRUPT;
        keep = 0;
        for( i=0; i<8; i++ ) keep |= (uint64_t)ctl[1+i]<<(8*i);
        ctl += 9;
      } else {                                  /* a run of trivial blocks */
        int    zero = c>=0xC0;
        size_t cnt  = ( zero ? c-0xBF : c-0x3F )<<6;
        ctl++;
        if( ctl>=ctl_end ) cnt -= 64-bs_last;   /* the blob's last block may be short */
        if( ZLE_UNLIKELY( cnt>(size_t)( out_end-out ) ) ) return ZLE_ERR_SPACE;
        if( zero ) memset( out, 0, cnt );
        else {
          if( ZLE_UNLIKELY( cnt>(size_t)( blob_end-lit ) ) ) return ZLE_ERR_CORRUPT;
          memcpy( out, lit, cnt );
          lit += cnt;
        }
        out += cnt;
        continue;
      }

      /* one block, keep its literal mask */

      bs = ctl<ctl_end ? 64 : bs_last;
      if( bs<64 ) keep &= ( (uint64_t)1<<bs )-1;
      nlit = zle_popcnt( keep );
      if( ZLE_UNLIKELY( bs  >(size_t)( out_end-out   ) ) ) return ZLE_ERR_SPACE;
      if( ZLE_UNLIKELY( nlit>(size_t)( blob_end-lit  ) ) ) return ZLE_ERR_CORRUPT;
      memset( out, 0, bs );
      lit  = zle_expand( out, lit, keep );
      out += bs;
    }

    p = lit;
  }

  return (int64_t)( out-(uchar *)dst );
}

#endif /* ZLE_AVX512 */

char const *
ZLE_strerror( int64_t res ) {
  switch( res ) {
  case ZLE_ERR_SPACE:   return "out of buffer space";
  case ZLE_ERR_CORRUPT: return "malformed compressed data";
  case 0:               return "empty";
  default:              return res>0 ? "ok" : "unknown";
  }
}
