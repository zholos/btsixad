#ifndef BTSIXAD_SIXAXIS_H
#define BTSIXAD_SIXAXIS_H

#include "device.h"

extern struct descr sixaxis_descr;

void sixaxis_operational(struct device* d, int operational);
void sixaxis_leds(struct device* d, int bitmap, int blink);
void sixaxis_fixup(struct device* d, int kind,
                   unsigned char* data, size_t size);

#endif
