#ifndef ZLE_H_20260830
#define ZLE_H_20260830

#include <stddef.h> /* size_t */
#include <stdint.h> /* int64_t */

#define ZLELIB_API

#if defined(__GNUC__) || defined(__clang__)
#define ZLE_RESTRICT __restrict
#elif defined(_MSC_VER)
#define ZLE_RESTRICT __restrict
#elif !defined(__cplusplus) && defined(__STDC_VERSION__) && __STDC_VERSION__>=199901L
#define ZLE_RESTRICT restrict
#else
#define ZLE_RESTRICT
#endif

#define ZLE_OVERHEAD (8)
#define ZLE_COMPRESSBOUND( in_sz ) (ZLE_OVERHEAD + (in_sz))

#define ZLE_MAX_SZ (((size_t)1<<49)+14)

#define ZLE_ERR_SPACE   ((int64_t)-1)
#define ZLE_ERR_CORRUPT ((int64_t)-2)

#if defined (__cplusplus)
extern "C" {
#endif

ZLELIB_API size_t
ZLE_compress( void       * ZLE_RESTRICT dst,
              void const * ZLE_RESTRICT src,
              size_t                    src_sz );

ZLELIB_API int64_t
ZLE_decompress( void       * ZLE_RESTRICT dst,
                size_t                    dst_max,
                void const * ZLE_RESTRICT src,
                size_t                    src_sz );

ZLELIB_API char const *
ZLE_strerror( int64_t res );

#if defined (__cplusplus)
}
#endif

#endif /* ZLE_H_20260830 */
