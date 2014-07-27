#include "device.h"

#include "host.h"
#include "sixaxis.h"
#include "vuhid.h"
#include "wrap.h"

#include <assert.h>
#include <bluetooth.h>
#include <err.h>
#include <pthread.h>
#include <sdp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <dev/usb/usbhid.h>


static void
query_sdp(struct device* d)
{
    bdaddr_t l;
    bdaddr_copy(&l, &bdaddr);
    void* xs = sdp_open(&l, &d->bdaddr);
    if (!xs)
        errx(1, "sdp_open() failed");
    if (sdp_error(xs))
        errc(1, sdp_error(xs), "sdp_open() failed");

    unsigned char v[4][3]; // max size is uint16
    sdp_attr_t attrs[4];
    for (int i = 0; i < 4; i++) {
        attrs[i].flags = SDP_ATTR_INVALID;
        attrs[i].vlen = sizeof v[i];
        attrs[i].value = (void*)&v[i];
    }

    uint16_t serv = SDP_SERVICE_CLASS_PNP_INFORMATION;
    uint32_t ranges[2] = {
        SDP_ATTR_RANGE(0x0201, 0x0203),
        SDP_ATTR_RANGE(0x0205, 0x0205)
    };
    if (sdp_search(xs, 1, &serv, 2, ranges, 4, attrs))
        errc(1, sdp_error(xs), "sdp_search()");
    if (sdp_close(xs))
        errx(1, "sdp_close()");

    uint16_t id[5] = {}; // vendor, product, release, _, source
    for (int i = 0; i < 4; i++)
        if (attrs[i].flags == SDP_ATTR_OK &&
                (attrs[i].attr >= 0x0201 && attrs[i].attr <= 0x0205) &&
                attrs[i].vlen == 3 && v[i][0] == 0x09) // uint16
            id[attrs[i].attr-0x0201] = v[i][1] << 8 | v[i][2];

    d->sixaxis = id[4] == 2 /* USB */ && id[0] == 0x054c && id[1] == 0x0268;
    d->model = d->sixaxis ? "Sixaxis gamepad" : "unknown device";

    syslog(LOG_DEBUG, "connection is from %s: "
           "vendor 0x%04x (by 0x%04x), product 0x%04x, release 0x%04x",
           d->model, id[0], id[4], id[1], id[2]);
}


static void
reflect_state(struct device* d, int opened)
{
    if (d->sixaxis) {
        if (d->unit >= 0)
            // uhid1 is LED 1
            sixaxis_leds(d, 1 << d->unit % 4, !opened);
        else
            sixaxis_leds(d, 0xf, 1);
        sixaxis_operational(d, opened || dflag > 2);
    }
}

int
device_open(struct device* d)
{
    int r = 0;
    wp(pthread_mutex_lock(&d->mutex));
    if (d->state == 0) {
        d->state = 1;
        d->timeout_running = 0;
        d->intr_report.size = 0;
        r = 1;
        wp(pthread_cond_broadcast(&d->cond));
    }
    wp(pthread_mutex_unlock(&d->mutex));
    if (r)
        reflect_state(d, 1);
    return r;
}

void
device_close(struct device* d)
{
    reflect_state(d, 0);
    wp(pthread_mutex_lock(&d->mutex));
    if (d->state == 1)
        d->state = 0;
    wp(pthread_cond_broadcast(&d->cond));
    wp(pthread_mutex_unlock(&d->mutex));
}

void
device_disconnect(struct device* d)
{
    wp(pthread_mutex_lock(&d->mutex));
    d->state = -1;
    wp(pthread_cond_broadcast(&d->cond));
    wp(pthread_mutex_unlock(&d->mutex));

    // close() would introduce a race condition on the file descriptor number
    shutdown(d->intr, SHUT_RDWR);
    shutdown(d->ctrl, SHUT_RDWR);
}


static void
print_message(struct device* d, int send, int ctrl, unsigned char message,
              unsigned char* data, size_t size)
{
    // NOTE: this doesn't prevent overlapping output for different devices
    wp(pthread_mutex_lock(&d->mutex));
    if (ctrl || dflag > 1 || !(d->d_printed & 1 << send)) {
        printf("%s %s message 0x%02x and %zd bytes",
               (send ? "sending" : "received"),
               (ctrl ? "control" : "interrupt"), message, size);
        if (size)
            printf(": 0x");
        for (size_t i = 0; i < size; i++)
            printf("%02x", data[i]);
        printf("\n");
    }
    if (!(ctrl || dflag > 1 || (d->d_printed & 1 << send))) {
        printf("(use -dd to print subsequent interrupt messages)\n");
        d->d_printed |= 1 << send;
    }
    wp(pthread_mutex_unlock(&d->mutex));
}

static int
send_message(struct device* d, int ctrl, unsigned char message,
             unsigned char* data, size_t size)
{
    if (dflag)
        print_message(d, 1, ctrl, message, data, size);

    struct iovec iov[2] = { { &message, 1 }, { data, size } };
    ssize_t w = WR(writev(ctrl ? d->ctrl : d->intr, iov, 2));
    if (!w || w-1 < size) {
        device_disconnect(d);
        return 0;
    }
    return 1;
}

static int
recv_message(struct device* d, int ctrl, unsigned char* message,
             unsigned char* data, size_t* size)
{
    struct iovec iov[2] = { { message, 1 }, { data, *size } };
    ssize_t r = WR(readv(ctrl ? d->ctrl : d->intr, iov, 2));
    if (!r--) {
        device_disconnect(d);
        return 0;
    }
    *size = r;
    if (dflag)
        print_message(d, 0, ctrl, *message, data, *size);
    return 1;
}


static const clockid_t timed_clock = CLOCK_MONOTONIC;
static const long nsec = 1000000000L;

static void
timed_wait(struct device* d)
{
    struct timespec abstime;
    we(clock_gettime(timed_clock, &abstime));
    abstime.tv_nsec += nsec / 10;
    abstime.tv_sec += abstime.tv_nsec / nsec;
    abstime.tv_nsec %= nsec;
    int r = pthread_cond_timedwait(&d->cond, &d->mutex, &abstime);
    if (r != ETIMEDOUT)
        wp(r);
}

int
device_read(struct device* d, int nonblock, unsigned char* buf, size_t* size)
{
    int r = 0;
    wp(pthread_mutex_lock(&d->mutex));
    while (!d->intr_report.size && !nonblock) {
        if (d->state == -1 || vuhid_cancelled())
            goto unlock_done;
        timed_wait(d);
    }
    if (*size > d->intr_report.size)
        *size = d->intr_report.size;
    if (buf) { // buf=NULL used to poll
        memcpy(buf, d->intr_report.data, *size);
        d->intr_report.size = 0;
    }
    r = 1;
unlock_done:
    wp(pthread_mutex_unlock(&d->mutex));
    return r;
}

int
device_write(struct device* d, unsigned char* data, size_t size)
{
    return send_message(d, 0, 0xa2, data, size);
}

int
device_get_report(struct device* d, int kind, unsigned char* data, size_t* size)
{
    int r = -1;
    wp(pthread_mutex_lock(&d->mutex));
    while (d->ctrl_query.type) {
        if (d->state == -1 || vuhid_cancelled())
            goto unlock_done;
        timed_wait(d);
    }
    d->ctrl_query.type = 1;
    d->ctrl_query.kind = kind;
    d->ctrl_query.result = -1;
    d->ctrl_query.cancelled = 0;
    d->ctrl_query.data = data;
    d->ctrl_query.size = *size;
    int id = d->descr->id && *size ? data[0] : 0; // before data is clobbered
    wp(pthread_mutex_unlock(&d->mutex));

    // Sixaxis seems to require report size in the message
    assert(*size <= DEVICE_MAX_REPORT_SIZE && DEVICE_MAX_REPORT_SIZE <= 0xffff);
    unsigned char buf[] = { id, *size & 0xff, *size >> 8 };

    assert(kind >= 1 && kind <= 3);
    int sent = send_message(d, 1, 0x48 + kind,
                            buf+!d->descr->id, sizeof buf-!d->descr->id);

    wp(pthread_mutex_lock(&d->mutex));
    while (sent && d->ctrl_query.result == -1) {
        if (d->state == -1 || vuhid_cancelled()) {
            // ctrl_run is responsible for discarding the result
            d->ctrl_query.cancelled = 1;
            goto unlock_done;
        }
        timed_wait(d);
    }
    *size = d->ctrl_query.size;
    r = d->ctrl_query.result;
    d->ctrl_query.type = 0;
    wp(pthread_cond_broadcast(&d->cond));
unlock_done:
    wp(pthread_mutex_unlock(&d->mutex));
    return r;
}

int
device_set_report(struct device* d, int kind, unsigned char* data, size_t size)
{
    int r = -1;
    wp(pthread_mutex_lock(&d->mutex));
    while (d->ctrl_query.type) {
        if (d->state == -1 || vuhid_cancelled())
            goto unlock_done;
        timed_wait(d);
    }
    d->ctrl_query.type = 2;
    d->ctrl_query.result = -1;
    wp(pthread_mutex_unlock(&d->mutex));

    assert(kind >= 1 && kind <= 3);
    int sent = send_message(d, 1, 0x50 + kind, data, size);

    wp(pthread_mutex_lock(&d->mutex));
    while (sent && d->ctrl_query.result == -1) {
        if (d->state == -1 || vuhid_cancelled()) {
            // ctrl_run is responsible for discarding the result
            d->ctrl_query.cancelled = 1;
            goto unlock_done;
        }
        wp(pthread_cond_wait(&d->cond, &d->mutex));
    }
    r = d->ctrl_query.result;
    d->ctrl_query.type = 0;
    wp(pthread_cond_broadcast(&d->cond));
unlock_done:
    wp(pthread_mutex_unlock(&d->mutex));
    return r;
}


static void*
ctrl_run(void* d_void)
{
    struct device* d = d_void;
    unsigned char* buf = wm(malloc(DEVICE_MAX_REPORT_SIZE));
    for (;;) {
        int unexpected = 0;
        unsigned char message;
        size_t size = DEVICE_MAX_REPORT_SIZE;
        if (!recv_message(d, 1, &message, buf, &size))
            break;
        switch (message >> 4) {
        case 0: // HANDSHAKE in response to GET_REPORT or SET_REPORT
            wp(pthread_mutex_lock(&d->mutex));
            if (d->ctrl_query.type && d->ctrl_query.result == -1) {
                if (d->ctrl_query.cancelled)
                    d->ctrl_query.cancelled = d->ctrl_query.type = 0;
                else {
                    d->ctrl_query.result = message & 0xf;
                    d->ctrl_query.size = 0;
                }
                wp(pthread_cond_broadcast(&d->cond));
            } else
                unexpected = 1;
            wp(pthread_mutex_unlock(&d->mutex));
            break;
        case 10: // DATA in response to GET_REPORT
            wp(pthread_mutex_lock(&d->mutex));
            if (d->ctrl_query.type == 1 && d->ctrl_query.result == -1) {
                if (d->ctrl_query.cancelled)
                    d->ctrl_query.cancelled = d->ctrl_query.type = 0;
                else {
                    if (d->sixaxis)
                        sixaxis_fixup(d, d->ctrl_query.kind, buf, size);
                    d->ctrl_query.result = 0;
                    if (d->ctrl_query.size > size)
                        d->ctrl_query.size = size;
                    memcpy(d->ctrl_query.data, buf, d->ctrl_query.size);
                }
                wp(pthread_cond_broadcast(&d->cond));
            } else
                unexpected = 1;
            wp(pthread_mutex_unlock(&d->mutex));
            break;
        case 1: // HID_CONTROL
            switch (message & 0xf) {
            case 5: // VIRTUAL_CABLE_UNPLUG
                // TODO: should erase persistent configuration information
                syslog(LOG_DEBUG, "virtual cable unplug by device");
                device_disconnect(d);
                break;
            default:
                ; // shall ignore other operations
            }
            break;
        default:
            // should not get other messages without requests
            unexpected = 1;
        }
        if (unexpected) {
            syslog(LOG_DEBUG, "unexpected control message, disconnecting");
            break;
        }
    }
    free(buf);
    device_disconnect(d);
    return NULL;
}


static void*
intr_run(void* d_void)
{
    struct device* d = d_void;
    unsigned char* buf = wm(malloc(DEVICE_MAX_REPORT_SIZE));
    for (;;) {
        unsigned char message;
        size_t size = DEVICE_MAX_REPORT_SIZE;
        if (!recv_message(d, 0, &message, buf, &size))
            break;
        if (message == 0xa1) {
            if (d->sixaxis)
                sixaxis_fixup(d, UHID_INPUT_REPORT, buf, size);
            wp(pthread_mutex_lock(&d->mutex));
            if (d->state == 1) {
                // Only add to queue if file is open.
                // Buffering only one report is really enough: some users like
                // GLFW don't care about transitions, only the current state,
                // and we don't want a situation where a slow user will only
                // read stale reports from the back of the queue.
                memcpy(d->intr_report.data, buf, size);
                d->intr_report.size = size;
                wp(pthread_cond_signal(&d->cond));
            }
            wp(pthread_mutex_unlock(&d->mutex));
        } else {
            syslog(LOG_DEBUG, "unexpected interrupt message, disconnecting");
            break;
        }
    }
    free(buf);
    device_disconnect(d);
    return NULL;
}


void
device_run(struct device* d)
{
    assert(d->ctrl >= 0 && d->intr >= 0);

    query_sdp(d);
    if (!d->sixaxis)
        return;
    d->descr = &sixaxis_descr;
    d->intr_report.data = wm(malloc(DEVICE_MAX_REPORT_SIZE));

    pthread_condattr_t condattr;
    wp(pthread_mutex_init(&d->mutex, NULL));
    wp(pthread_condattr_init(&condattr));
    wp(pthread_condattr_setclock(&condattr, timed_clock));
    wp(pthread_cond_init(&d->cond, &condattr));
    wp(pthread_condattr_destroy(&condattr));

    pthread_t ctrl_thread, intr_thread;
    wp(pthread_create(&ctrl_thread, NULL, ctrl_run, d));
    wp(pthread_create(&intr_thread, NULL, intr_run, d));

    vuhid_allocate_unit(d);
    // Send our control messages before user can access device.
    reflect_state(d, 0);
    vuhid_open(d);

    wp(pthread_mutex_lock(&d->mutex));
    struct timespec until;
    int timed_out = 0;
    while (d->state != -1 && !timed_out) {
        if (timeout && !d->timeout_running && d->state == 0) {
            d->timeout_running = 1;
            we(clock_gettime(timed_clock, &until));
            until.tv_sec += timeout;
        }
        if (d->timeout_running) {
            int r = pthread_cond_timedwait(&d->cond, &d->mutex, &until);
            if (r == ETIMEDOUT)
                timed_out = 1;
            else
                wp(r);
        } else
            wp(pthread_cond_wait(&d->cond, &d->mutex));
    }
    wp(pthread_mutex_unlock(&d->mutex));

    vuhid_close(d);

    if (timed_out)
        if (d->sixaxis)
            sixaxis_operational(d, -1);

    wp(pthread_join(intr_thread, NULL));
    wp(pthread_join(ctrl_thread, NULL));

    wp(pthread_cond_destroy(&d->cond));
    wp(pthread_mutex_destroy(&d->mutex));

    free(d->intr_report.data);
}
