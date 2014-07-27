#include "wrap.h"

void*
wm(void* result)
{
    if (!result)
        err(1, "malloc() failed");
    return result;
}

void
wp(int result)
{
    if (result)
        errc(1, result, "pthread failed");
}

void
we(int result)
{
    if (result == -1)
        err(1, "function failed");
}
