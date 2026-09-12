/* Minimal freestanding string.h stub for libc++ #include_next */
#ifndef _ZPP_FREESTANDING_STRING_H
#define _ZPP_FREESTANDING_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void * memcpy(void * dest, const void * src, size_t count);
void * memmove(void * dest, const void * src, size_t count);
void * memset(void * dest, int value, size_t count);
int memcmp(const void * lhs, const void * rhs, size_t count);
size_t strlen(const char * str);

#ifdef __cplusplus
}
#endif

#endif
