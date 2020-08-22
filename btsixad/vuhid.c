#include "vuhid.h"

#include "device.h"
#include "host.h"
#include "wrap.h"

#include <assert.h>
#include <err.h>
#include <grp.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
#include <dev/usb/usb_ioctl.h>

#include <cuse.h>

#ifndef CUSE_ID_BTSIXAD
#define CUSE_ID_BTSIXAD(what) CUSE_MAKE_ID('6', 'A', what, 0)
#endif


static int
v_open(struct cuse_dev* dev, int fflags)
{
    struct device* d = cuse_dev_get_priv0(dev);
    if (!device_open(d))
        return CUSE_ERR_BUSY;
    return CUSE_ERR_NONE;
}

static int
v_close(struct cuse_dev* dev, int fflags)
{
    struct device* d = cuse_dev_get_priv0(dev);
    device_close(d);
    return CUSE_ERR_NONE;
}

static int
v_read(struct cuse_dev* dev, int fflags, void* peer_ptr, int len_)
{
    if (!(fflags & CUSE_FFLAG_READ))
        return CUSE_ERR_OTHER;
    struct device* d = cuse_dev_get_priv0(dev);
    size_t len = len_;
    if (len > DEVICE_MAX_REPORT_SIZE)
        len = DEVICE_MAX_REPORT_SIZE;
    unsigned char* buf = wm(malloc(len));
    int nonblock = fflags & CUSE_FFLAG_NONBLOCK;
    if (!device_read(d, nonblock, buf, &len))
        len = 0; // disconnected, act like EOF
    int r = cuse_copy_out(buf, peer_ptr, len);
    free(buf);
    if (!r && nonblock && !len)
        r = CUSE_ERR_WOULDBLOCK;
    return r ? r : len;
}

static int
v_write(struct cuse_dev* dev, int fflags, const void* peer_ptr, int len_)
{
    if (!(fflags & CUSE_FFLAG_WRITE))
        return CUSE_ERR_OTHER;
    struct device* d = cuse_dev_get_priv0(dev);
    size_t len = len_;
    if (len > DEVICE_MAX_REPORT_SIZE)
        len = DEVICE_MAX_REPORT_SIZE;
    unsigned char* buf = wm(malloc(len));
    int r = cuse_copy_in(peer_ptr, buf, len);
    if (!r && !device_write(d, buf, len))
        r = CUSE_ERR_INVALID;
    free(buf);
    return r ? r : len;
}

static int
bthid_result(int r)
{
    switch (r) {
    case -1: return CUSE_ERR_OTHER;      // disconnected
    case 0:  return CUSE_ERR_NONE;       // SUCCESSFUL
    case 1:  return CUSE_ERR_WOULDBLOCK; // NOT_READY
    default: return CUSE_ERR_INVALID;    // ERR_INVALID_REPORT_ID
    }
}

static int
v_ioctl(struct cuse_dev* dev, int fflags, unsigned long cmd,
            void* peer_data)
{
    struct device* d = cuse_dev_get_priv0(dev);
    struct usb_gen_descriptor m, *p = peer_data;
    int r = CUSE_ERR_INVALID;
    unsigned char* buf = NULL;
    switch (cmd) {
    case USB_GET_REPORT_ID: {
        int id = d->descr->id;
        r = cuse_copy_out(&id, peer_data, sizeof id);
        break;
    }
    case USB_GET_REPORT_DESC:
        if (r = cuse_copy_in(peer_data, &m, sizeof m))
            break;
        m.ugd_actlen = d->descr->report.size;
        if (r = cuse_copy_out(&m.ugd_actlen, &p->ugd_actlen,
                              sizeof m.ugd_actlen))
            break;
        if (m.ugd_data) {
            if (m.ugd_maxlen > d->descr->report.size)
                m.ugd_maxlen = d->descr->report.size;
            r = cuse_copy_out(d->descr->report.data, m.ugd_data, m.ugd_maxlen);
        }
        break;
    case USB_GET_REPORT:
    case USB_SET_REPORT: {
        if (!(fflags & (cmd == USB_GET_REPORT ? CUSE_FFLAG_READ
                                              : CUSE_FFLAG_WRITE)))
            return CUSE_ERR_OTHER;
        if (r = cuse_copy_in(peer_data, &m, sizeof m))
            break;
        int kind = m.ugd_report_type;
        if (!(kind >= 1 && kind <= 3))
            return CUSE_ERR_INVALID;
        size_t len = m.ugd_maxlen;
        if (len > DEVICE_MAX_REPORT_SIZE)
            len = DEVICE_MAX_REPORT_SIZE;
        buf = wm(malloc(len));
        if (cmd == USB_GET_REPORT) {
            if (d->descr->id && len)
                if (r = cuse_copy_in(m.ugd_data, buf, 1))
                    break;
            if (r = bthid_result(device_get_report(d, kind, buf, &len)))
                break;
            r = cuse_copy_out(buf, m.ugd_data, len);
        } else {
            if (r = cuse_copy_in(m.ugd_data, buf, len))
                break;
            r = bthid_result(device_set_report(d, kind, buf, len));
        }
        break;
    }
    }
    free(buf);
    return r;
}

static int
v_poll(struct cuse_dev* dev, int fflags, int events)
{
    struct device* d = cuse_dev_get_priv0(dev);
    int revents = 0;
    if (events & CUSE_POLL_READ) {
        size_t len = 1;
        if (!device_read(d, 1, NULL, &len) || len) // disconnected or ready
            revents |= CUSE_POLL_READ;
    }
    if (events & CUSE_POLL_WRITE)
        revents |= CUSE_POLL_WRITE;
    return revents;
}

static struct cuse_methods v_methods =
    { v_open, v_close, v_read, v_write, v_ioctl, v_poll };


static int initialized;

static const char name[] = "btsixa";
static uid_t user = 0;
static gid_t group = 0;
static int mode = 0644;

void
vuhid_init()
{
    // Reading is sufficient for using the controller,
    // writing allows setting LEDs and rumbling.
    struct group* gr = getgrnam("operator"); // like uhid
    if (gr)
        group = gr->gr_gid;

    int r = cuse_init();
    if (!r)
        initialized = 1;
    else {
        const char* error;
        switch (r) {
        case CUSE_ERR_NOT_LOADED:
            error = "kldload cuse";
            break;
        case CUSE_ERR_INVALID:
            error = "cuse not accessible";
            break;
        default:
            error = "cuse_init() failed";
        }
        if (dflag)
            syslog(LOG_WARNING, "%s, won't create %s device", error, name);
        else
            errx(1, "%s", error);
    }
}

static void*
worker_run(void* _)
{
    sigset_t mask;
    we(sigemptyset(&mask));
    we(sigaddset(&mask, SIGHUP)); // delivered when peer wants to close the file
    wp(pthread_sigmask(SIG_BLOCK, &mask, NULL));
    // Won't get a signal until we start processing, no need for main thread to
    // wait for this to be in place.

    for (;;)
        if (cuse_wait_and_process())
            errx(1, "cuse_wait_and_process() failed");
}

void
vuhid_start()
{
    if (!initialized)
        return;

    for (int i = 0; i < 4; i++) {
        pthread_t worker;
        wp(pthread_create(&worker, NULL, worker_run, NULL));
    }
}

void
vuhid_allocate_unit(struct device* d)
{
    if (!initialized ||
            cuse_alloc_unit_number_by_id(&d->unit, CUSE_ID_BTSIXAD(0)))
        d->unit = -1;
}

void
vuhid_open(struct device* d)
{
    // We can't create a uhid device directly because the system crashes if a
    // real device with the same unit number is connected. Instead we create a
    // unique device and use a devfs rule to automatically symlink it to uhid.

    assert(!d->dev);
    if (d->unit < 0)
        return;
    d->dev = cuse_dev_create(&v_methods, d, NULL,
                             user, group, mode, "%s%d", name, d->unit);
    if (!d->dev)
        errx(1, "cuse_dev_create() failed");
    char buf[1000];
    syslog(LOG_NOTICE, "%s%d: %s at %s",
           name, d->unit, d->model, bt_ntoa(&d->bdaddr, buf));
}

void
vuhid_close(struct device* d)
{
    if (!d->dev)
        return;
    cuse_dev_destroy(d->dev);
    cuse_free_unit_number_by_id(d->unit, CUSE_ID_BTSIXAD(0));
    syslog(LOG_NOTICE, "%s%d detached", name, d->unit);
    d->unit = -1;
    d->dev = NULL;
}

void
vuhid_wakeup()
{
    if (initialized)
        cuse_poll_wakeup();
}

int
vuhid_cancelled()
{
    return initialized && !cuse_got_peer_signal();
}
