/* Minimal freestanding wchar.h stub for libc++ #include_next */
#ifndef _ZPP_FREESTANDING_WCHAR_H
#define _ZPP_FREESTANDING_WCHAR_H

#include <stddef.h>

#ifndef WEOF
#define WEOF ((wchar_t) - 1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* mbstate_t is required by libc++ char_traits / iosfwd */
typedef struct
{
    unsigned __state;
    unsigned __value;
} mbstate_t;

/* Wide character functions — C signatures expected by libc++ wchar.h.
   libc++ provides const-correct C++ overloads on top of these. */
const wchar_t * wcschr(const wchar_t * __s, wchar_t __c);
const wchar_t * wcspbrk(const wchar_t * __s1, const wchar_t * __s2);
const wchar_t * wcsrchr(const wchar_t * __s, wchar_t __c);
const wchar_t * wcsstr(const wchar_t * __s1, const wchar_t * __s2);
const wchar_t * wmemchr(const wchar_t * __s, wchar_t __c, size_t __n);

size_t wcslen(const wchar_t * __s);
int wmemcmp(const wchar_t * __s1, const wchar_t * __s2, size_t __n);
wchar_t * wmemcpy(wchar_t * __dest, const wchar_t * __src, size_t __n);
wchar_t * wmemmove(wchar_t * __dest, const wchar_t * __src, size_t __n);
wchar_t * wmemset(wchar_t * __dest, wchar_t __c, size_t __n);
int wcscmp(const wchar_t * __s1, const wchar_t * __s2);
size_t mbrtowc(wchar_t * __pwc,
               const char * __s,
               size_t __n,
               mbstate_t * __ps);
size_t mbrlen(const char * __s, size_t __n, mbstate_t * __ps);

#ifdef __cplusplus
}
#endif

#endif
