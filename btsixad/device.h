#ifndef BTSIXAD_DEVICE_H
#define BTSIXAD_DEVICE_H

#define L2CAP_SOCKET_CHECKED
#include <bluetooth.h>

// Protocol limit is 0xffff
#define DEVICE_MAX_REPORT_SIZE 1024

struct descr {
    struct {
        unsigned char* data;
        size_t size;
    } report;
    int id;// first or 0 for ioctl - a flag for whether to include IDs
};

struct device {
    // initialized by server:
    bdaddr_t bdaddr;
    int ctrl, intr;
    // private, zero-initialized:
    int sixaxis;
    const char* model;
    struct descr* descr;
    struct cuse_dev* dev;
    int unit;
    int state; // 0 - closed, 1 - open, -1 - disconnected
    int timeout_running;
    int d_printed;
    struct {
        unsigned char* data;
        size_t size;
    } intr_report;
    struct {
        int type; // 1 - GET_REPORT, 2 - SET_REPORT
        int kind;
        int result; // -1 initially
        int cancelled;
        unsigned char* data;
        size_t size;
    } ctrl_query;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

void device_run(struct device* d);
void device_disconnect(struct device* d);

int device_open(struct device* d);
void device_close(struct device* d);
int device_read(struct device* d, int nonblock,
                unsigned char* data, size_t* size);
int device_write(struct device* d,
                 unsigned char* data, size_t size);
int device_get_report(struct device* d, int kind,
                      unsigned char* data, size_t* size);
int device_set_report(struct device* d, int kind,
                      unsigned char* data, size_t size);

#endif
