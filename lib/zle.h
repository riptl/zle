#ifndef ZLE_H_20260831
#define ZLE_H_20260831

#include <stddef.h> /* size_t */
#include <stdint.h> /* int64_t */

#if defined(__GNUC__) || defined(__clang__)
#define ZLELIB_API __attribute__((visibility("default")))
#else
#define ZLELIB_API
#endif

#define ZLE_VERSION_MAJOR   2
#define ZLE_VERSION_MINOR   0
#define ZLE_VERSION_RELEASE 0
#define ZLE_VERSION_NUMBER  (ZLE_VERSION_MAJOR*100*100 + ZLE_VERSION_MINOR*100 + ZLE_VERSION_RELEASE)

#define ZLE_LIB_VERSION  ZLE_VERSION_MAJOR.ZLE_VERSION_MINOR.ZLE_VERSION_RELEASE
#define ZLE_QUOTE(str)   #str
#define ZLE_EXPAND_AND_QUOTE(str) ZLE_QUOTE(str)
#define ZLE_VERSION_STRING ZLE_EXPAND_AND_QUOTE(ZLE_LIB_VERSION)


#if defined(__GNUC__) || defined(__clang__)
#define ZLE_RESTRICT __restrict
#elif defined(_MSC_VER)
#define ZLE_RESTRICT __restrict
#elif !defined(__cplusplus) && defined(__STDC_VERSION__) && __STDC_VERSION__>=199901L
#define ZLE_RESTRICT restrict
#else
#define ZLE_RESTRICT
#endif

/* ZLE_COMPRESSBOUND is the largest output of ZLE_compress for in_sz
   input bytes. */

#define ZLE_COMPRESSBOUND( in_sz ) ( (in_sz) + (in_sz)/64 + 128 )

/* ZLE_MAX_SZ is the largest input ZLE_compress accepts. */

#define ZLE_MAX_SZ ((size_t)0xFFFFFFF0)

#define ZLE_ERR_SPACE   ((int64_t)-1)
#define ZLE_ERR_CORRUPT ((int64_t)-2)

#if defined (__cplusplus)
extern "C" {
#endif

/* ZLE_compress compresses src_sz bytes at src.  Writes up to
   ZLE_COMPRESSBOUND( src_sz ) bytes to dst.  Returns the number of bytes
   written.  src_sz<=ZLE_MAX_SZ.

   On x86 with AVX-512, the readable span starting at src must cover
   src_sz rounded up to a multiple of 64 bytes.  The padding does not
   affect the output. */

ZLELIB_API size_t
ZLE_compress( void       * ZLE_RESTRICT dst,
              void const * ZLE_RESTRICT src,
              size_t                    src_sz );

/* ZLE_decompress decompresses src_sz bytes at src.  Writes up to
   dst_max bytes to dst.  Returns the number of bytes decompressed (>=0)
   on success, or a negative ZLE_ERR_* code on failure.  Safe given
   arbitrary input: never reads past src+src_sz or writes past
   dst+dst_max. */

ZLELIB_API int64_t
ZLE_decompress( void       * ZLE_RESTRICT dst,
                size_t                    dst_max,
                void const * ZLE_RESTRICT src,
                size_t                    src_sz );

/* ZLE_strerror returns a static string describing a return code of
   ZLE_decompress. */

ZLELIB_API char const *
ZLE_strerror( int64_t res );

#if defined (__cplusplus)
}
#endif

#endif /* ZLE_H_20260831 */
