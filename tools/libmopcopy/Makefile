#	$NetBSD: Makefile,v 1.3 2011/04/17 12:52:26 tron Exp $

HOSTLIB=	mopcopy
LIBMOPCOPY_DIR=	${NETBSDSRCDIR}/usr.sbin/mopd/common

.include <bsd.hostinit.mk>

# for now
SRCS=		cmp.c  \
		get.c \
		nma.c \
		mopdef.c \
		file.c \
		log.c

.ifndef NOCOMPATLIB
COMPATLIB_NO_LIB= yes # only the include files, not the library
.-include       "${TOOLDIR}/share/compat/defs.mk"
.endif

BUILD_OSTYPE!=  uname -s

# Disable use of pre-compiled headers on Darwin.
.if ${BUILD_OSTYPE} == "Darwin"
CPPFLAGS+=	-no-cpp-precomp
.endif

HOST_CPPFLAGS+=	-DHAVE_NBTOOL_CONFIG_H=1

# VAX binaries are now ELF, so no need to support a.out format
HOST_CPPFLAGS+=	-DNOAOUT

.PATH:	${LIBMOPCOPY_DIR}

realinstall: install.host
install.host: ${TOOLDIR}/lib/libmopcopy.a
${TOOLDIR}/lib/libmopcopy.a:: libmopcopy.a
	${_MKTARGET_INSTALL}
	mkdir -p ${TOOLDIR}/lib
	${HOST_INSTALL_FILE} -m ${BINMODE} libmopcopy.a ${.TARGET}

.include "${.CURDIR}/../Makefile.nbincludes"
.include <bsd.hostlib.mk>
