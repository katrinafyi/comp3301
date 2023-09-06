PROG=	vkeyadm
SRCS=	vkeyadm.c \
	sshbuf.c sshbuf-getput-basic.c sshbuf-io.c sshbuf-misc.c \
	atomicio.c misc.c log.c match.c xmalloc.c addrmatch.c addr.c \
	fatal.c ssherr.c
MAN=

.PATH:		${.CURDIR}/../../usr.bin/ssh
CFLAGS+=	-I${.CURDIR}/../../usr.bin/ssh

CFLAGS+=	-W -Wall -Wstrict-prototypes -Wno-unused-parameter -Wunused-variable -Werror


.include <bsd.prog.mk>
