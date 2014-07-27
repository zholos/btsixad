#ifndef BTSIXAD_VUHID_H
#define BTSIXAD_VUHID_H

#include "device.h"

void vuhid_init();
void vuhid_start();
void vuhid_allocate_unit(struct device* device);
void vuhid_open(struct device* device);
void vuhid_close(struct device* device);
void vuhid_wakeup();
int vuhid_cancelled();

#endif
