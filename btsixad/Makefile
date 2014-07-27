PROG=btsixad
SRCS=host.c device.c sixaxis.c vuhid.c wrap.c
MAN=btsixad.8

CFLAGS+= -pthread -I${LOCALBASE}/include
CFLAGS+= -Wno-parentheses
LDFLAGS+= -pthread -L${LOCALBASE}/lib
LDADD+= -lbluetooth -lsdp -lcuse4bsd

SCRIPTS=rc.sh
SCRIPTSNAME_rc.sh=btsixad
SCRIPTSDIR_rc.sh=${PREFIX}/etc/rc.d

FILES=devd.conf
FILESNAME_devd.conf=btsixad.conf
FILESDIR_devd.conf=${PREFIX}/etc/devd

CLEANFILES+= rc.sh devd.conf
rc.sh devd.conf: ${@:S/$/.in/}
	sed 's|%%PREFIX%%|${PREFIX}|g' $> >$@

PREFIX?=/usr/local
LOCALBASE?=/usr/local
BINDIR=${PREFIX}/sbin
MANDIR=${PREFIX}/man/man

.include <bsd.prog.mk>
