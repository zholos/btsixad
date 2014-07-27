#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <usbhid.h>
#include <sys/ioctl.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usbhid.h>

#define W(f) ({ int r = (f); if (r == -1) err(1, #f); r; })

int
main(int argc, char* argv[]) {
    if (argc != 2)
        errx(1, "usage: test uhid0");

    char* path = argv[1];
    if (!strchr(path, '/'))
        asprintf(&path, "/dev/%s", path);

    int fd = W(open(path, O_RDWR));

    int ioctl_id;
    if (ioctl(fd, USB_GET_REPORT_ID, &ioctl_id) == -1)
        err(1, "ioctl() failed");
    printf("report id: %d\n", ioctl_id);

    report_desc_t rd = hid_get_report_desc(fd);
    hid_data_t s;
    hid_item_t h;
    for (s = hid_start_parse(rd, 1 << hid_input, -1); hid_get_item(s, &h);) {
        switch (h.kind) {
        case hid_collection:
            break;
        case hid_endcollection:
            break;
        case hid_input:
            break;
        }
    }
    hid_end_parse(s);
    printf("input report size: %d\n", hid_report_size(rd, hid_input, -1));
    printf("output report size: %d\n", hid_report_size(rd, hid_output, -1));
    printf("feature report size: %d\n", hid_report_size(rd, hid_feature, -1));

    // light rumble
    unsigned char rumble[49] = { // size must be so for write
        0x01,
        0x00, 0x10, 0x01, 0, 0
    };
    // This comes before the other tests. It used to fail because it ran
    // concurrently with the driver setting up LEDs.
    if (write(fd, rumble, sizeof rumble) != sizeof rumble)
        err(1, "write() failed");
    printf("rumbled\n");

    printf("enumerating reports...\n");
    for (int kind = 0; kind < 3; kind++) {
        int size = hid_report_size(rd, kind, -1);
        for (int id = 0; id < 256; id++) {
            unsigned char buf[256];
            for (int len = 1; len < sizeof buf && len < size + 10; len++) {
                memset(buf, 0, len);
                buf[0] = id;
                if (hid_get_report(fd, kind, buf, len) == 0) {
                    if (kind == hid_input && id == 1 && len < size ||
                            kind == hid_feature &&
                                id >= 0xee && id < 0xf0 && len < 21)
                        continue;
                    if (len == 2)
                        break; // some reports return junk data of any size
                    printf("%s report 0x%02x has %d bytes: 0x",
                           kind==0 ? "input" : kind==1 ? "output" : "feature",
                           id, len);
                    for (int j = 0; j < len; j++)
                        printf("%02x", buf[j]);
                    printf("\n");
                    break;
                }
            }
        }
    }

    // blink pattern with different duty cycles, will diverge over time
    unsigned char leds[36] = {
        0x01,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0x1e,
        0xff, 0x20, 0x10, 0x40, 0x08,
        0xff, 0x20, 0x20, 0x40, 0x08,
        0xff, 0x20, 0x30, 0x40, 0x08,
        0xff, 0x20, 0x40, 0x40, 0x08
    };

    if (hid_set_report(fd, hid_output, leds, sizeof leds) == -1)
        err(1, "hid_set_report() failed");
    printf("set LEDs\n");

    struct pollfd pfd[1] = { fd, POLLIN | POLLOUT | POLLERR };
    int r;
    do
        r = poll(pfd, 1, 0);
    while (r == -1 && errno == EINTR);
    if (!(pfd[0].revents & POLLIN))
        printf("input report not available, following will block\n");

    unsigned char buf[49];
    struct timespec t0, t1;
    double t;
    int n;
    printf("measuring report rate...\n");
    for (n = -1; n < 500; n++) {
        if (!n)
            if (clock_gettime(CLOCK_MONOTONIC, &t0) == -1)
                err(1, "clock_gettime() failed");
        if (read(fd, buf, sizeof buf) != sizeof buf)
            err(1, "read() failed");
    }
    if (clock_gettime(CLOCK_MONOTONIC, &t1) == -1)
        err(1, "clock_gettime() failed");
    t = ((t1.tv_sec-t0.tv_sec) + 1e-9*(t1.tv_nsec-t0.tv_nsec)) / n;
    printf("%.2lf Hz / %.3lf ms\n", 1/t, 1000*t);

    printf("measuring response time...\n");
    if (clock_gettime(CLOCK_MONOTONIC, &t0) == -1)
        err(1, "clock_gettime() failed");
    for (n = 0; n < 500; n++) {
        buf[0] = 1;
        if (hid_get_report(fd, hid_input, buf, sizeof buf) == -1)
            err(1, "hid_get_report() failed");
    }
    if (clock_gettime(CLOCK_MONOTONIC, &t1) == -1)
        err(1, "clock_gettime() failed");
    t = ((t1.tv_sec-t0.tv_sec) + 1e-9*(t1.tv_nsec-t0.tv_nsec)) / n;
    printf("%.3lf ms\n", 1000*t);

    return 0;
}
