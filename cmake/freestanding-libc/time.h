/* Minimal freestanding time.h stub */
#ifndef _ZPP_FREESTANDING_TIME_H
#define _ZPP_FREESTANDING_TIME_H

typedef long time_t;

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

#endif
