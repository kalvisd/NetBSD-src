#	$NetBSD: Makefile,v 1.1 2008/04/30 21:15:33 garbled Exp $

.include <bsd.hostinit.mk>

HOSTPROGNAME=	${_TOOL_PREFIX}vax-mopcopy
HOST_SRCDIR=	usr.sbin/mopd/mopcopy

LIBMOPCOPYOBJ!= cd ${.CURDIR}/../libmopcopy && ${PRINTOBJDIR}

LDADD+=		-L${LIBMOPCOPYOBJ} -lmopcopy

# VAX binaries are now ELF, so no need to support a.out format
HOST_CPPFLAGS+=	-DNOAOUT

.include "${.CURDIR}/../Makefile.nbincludes"
.include "${.CURDIR}/../Makefile.host"
