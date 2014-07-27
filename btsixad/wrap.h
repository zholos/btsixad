#ifndef BTSIXAD_WRAP_H
#define BTSIXAD_WRAP_H

#include <err.h>
#include <errno.h>
#include <sys/types.h>

#define WR(f) ({ \
    ssize_t r; \
    do r = (f); while (r == -1 && errno == EINTR); \
    if (r == -1) if (errno == EPIPE) r = 0; else err(1, #f); \
    r; })

void* wm(void* result);
void wp(int result);
void we(int result);

#endif
