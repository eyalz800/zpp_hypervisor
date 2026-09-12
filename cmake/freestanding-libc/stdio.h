/* Minimal freestanding stdio.h stub */
#ifndef _ZPP_FREESTANDING_STDIO_H
#define _ZPP_FREESTANDING_STDIO_H

#include <stddef.h>

#define EOF (-1)

#ifdef __cplusplus
extern "C" {
#endif

/* Required so that <cstdio>'s `using ::remove` resolves to a concrete
   C function and doesn't conflict with std::remove from <algorithm>. */
int remove(const char * __filename);
int rename(const char * __old, const char * __new);

#ifdef __cplusplus
}
#endif

#endif
