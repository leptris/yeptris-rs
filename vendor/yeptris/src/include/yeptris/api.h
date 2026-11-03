/* api.h — export control for libyeptris.
 *
 * The library is built with hidden visibility; exactly the YEPTRIS_API-marked
 * surface is exported. Define YEPTRIS_BUILDING when compiling libyeptris
 * itself; define YEPTRIS_SHARED when consuming the shared library on Windows.
 */
#ifndef YEPTRIS_API_H
/* YEPTRIS_CT_ASSERT — the compile-time layout gate, C99/MSVC-C-mode/
 * C++ portable (typedef-array; libleptris's same migration): the old
 * C11 _Static_assert was the ONE C11 feature pinning the standard. */
#define YEPTRIS_CT_ASSERT_(cond, line) typedef char yeptris_ct_assert_##line[(cond) ? 1 : -1]
#define YEPTRIS_CT_ASSERT(cond) YEPTRIS_CT_ASSERT_(cond, __LINE__)

#define YEPTRIS_API_H

#if defined(YEPTRIS_BUILDING) && defined(YEPTRIS_SHARED)
#error "define YEPTRIS_BUILDING (building) or YEPTRIS_SHARED (consuming), not both"
#endif

#if defined(YEPTRIS_BUILDING)
#if defined(_WIN32) || defined(__CYGWIN__)
#define YEPTRIS_API __declspec(dllexport)
#else
#define YEPTRIS_API __attribute__((visibility("default")))
#endif
#elif defined(YEPTRIS_SHARED) && (defined(_WIN32) || defined(__CYGWIN__))
#define YEPTRIS_API __declspec(dllimport)
#else
#define YEPTRIS_API
#endif

#endif /* YEPTRIS_API_H */
