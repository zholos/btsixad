#include "host.h"

#include "device.h"
#include "vuhid.h"
#include "wrap.h"

#include <bluetooth.h>
#include <err.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>


int dflag;
bdaddr_t bdaddr;
int timeout;

struct session {
    LIST_ENTRY(session) next;
    struct device d;
};
LIST_HEAD(, session) sessions;
static pthread_mutex_t mutex;


static void*
session_run(void* s_void)
{
    struct session* s = s_void;
    device_run(&s->d);
    WR(close(s->d.intr));
    WR(close(s->d.ctrl));
    // TODO: is it possible and useful to wait for L2CAP close acknowledged?

    wp(pthread_mutex_lock(&mutex));
    syslog(LOG_DEBUG, "connection from %s closed",
           bt_ntoa(&s->d.bdaddr, NULL));
    LIST_REMOVE(s, next);
    wp(pthread_mutex_unlock(&mutex));
    free(s);
    return NULL;
}


static int lfd[2];

static void
listen_init(int ctrl)
{
    lfd[ctrl] = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BLUETOOTH_PROTO_L2CAP);
    if (lfd[ctrl] == -1)
        err(1, "socket() failed");

    struct sockaddr_l2cap sa;
    sa.l2cap_len = sizeof sa;
    sa.l2cap_family = AF_BLUETOOTH;
    sa.l2cap_psm = ctrl ? 0x11 // HID_Control
                        : 0x13; // HID_Interrupt
    bdaddr_copy(&sa.l2cap_bdaddr, &bdaddr);

    if (bind(lfd[ctrl], (struct sockaddr*)&sa, sizeof sa) == -1)
        err(1, "bind() failed");
    if (listen(lfd[ctrl], 10) == -1)
        err(1, "listen() failed");
}

static void*
listen_run(void* ctrl_void)
{
    int ctrl = !!ctrl_void;
    for (;;) {
        struct sockaddr_l2cap sa;
        socklen_t len = sizeof sa;
        int cfd;
        do
            cfd = accept(lfd[ctrl], (struct sockaddr*)&sa, &len);
        while (cfd == -1 && (errno == EINTR || errno == ECONNABORTED));
        if (cfd == -1)
            err(1, "accept() failed");
        int flag = 1;
        we(setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof flag));

        wp(pthread_mutex_lock(&mutex));
        syslog(LOG_DEBUG, "connection from %s on %s channel",
               bt_ntoa(&sa.l2cap_bdaddr, NULL), ctrl ? "control" : "interrupt");
        struct session* s;
        LIST_FOREACH(s, &sessions, next)
            if (bdaddr_same(&s->d.bdaddr, &sa.l2cap_bdaddr))
                break;
        if (s && (ctrl ? s->d.ctrl : s->d.intr) != -1)
            WR(close(cfd));
        else {
            if (!s) {
                s = wm(calloc(1, sizeof *s));
                wp(pthread_mutex_init(&s->d.mutex, NULL));
                bdaddr_copy(&s->d.bdaddr, &sa.l2cap_bdaddr);
                s->d.intr = s->d.ctrl = -1;
                LIST_INSERT_HEAD(&sessions, s, next);
            }
            *(ctrl ? &s->d.ctrl : &s->d.intr) = cfd;
            if (s->d.ctrl != -1 && s->d.intr != -1) {
                pthread_t thread;
                wp(pthread_create(&thread, NULL, session_run, s));
                wp(pthread_detach(thread));
            }
        }
        wp(pthread_mutex_unlock(&mutex));
    }
}


int
main(int argc, char* argv[])
{
    bdaddr_copy(&bdaddr, NG_HCI_BDADDR_ANY);

    int ch;
    while ((ch = getopt(argc, argv, "a:dt:")) != -1)
        switch (ch) {
        case 'a':
            if (!bt_aton(optarg, &bdaddr))
                goto usage;
            break;
        case 'd':
            dflag++;
            break;
        case 't': {
            char* end;
            timeout = strtol(optarg, &end, 10);
            if (end == optarg || *end || timeout < 0)
                goto usage;
            break;
        }
        default:
            goto usage;
        }
    argc -= optind;
    argv += optind;
    if (argc)
    usage:
        errx(1, "usage: btsixad [-a bdaddr] [-d] [-t timeout]");

    openlog("btsixad", LOG_PERROR, LOG_USER);

    vuhid_init();

    listen_init(1);
    listen_init(0);

    LIST_INIT(&sessions);
    wp(pthread_mutex_init(&mutex, NULL));

    if (!dflag)
        if (daemon(0, 0) == -1)
            err(1, "daemon() failed");

    vuhid_start();

    pthread_t ctrl_thread, intr_thread;
    wp(pthread_create(&ctrl_thread, NULL, listen_run, ""));
    wp(pthread_create(&intr_thread, NULL, listen_run, NULL));

    wp(pthread_join(ctrl_thread, NULL));
    wp(pthread_join(intr_thread, NULL));
    return 0;
}
