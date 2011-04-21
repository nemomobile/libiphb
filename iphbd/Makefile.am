#
# Generic options
#
AM_CFLAGS = -Wall -Wmissing-prototypes -std=c99 -Os -g -fPIC
AM_CPPFLAGS = -D_GNU_SOURCE
AM_LDFLAGS = -g -Wl,--as-needed

ACLOCAL_AMFLAGS = -I m4

#
# Additional dirs
#
iphbdincludedir = @includedir@/iphbd

#
# Build targets
#
bin_PROGRAMS = hbtest hbtest2
lib_LTLIBRARIES = libiphb.la

hbtest_SOURCES = hbtest.c
hbtest_LDADD = libiphb.la


hbtest2_SOURCES = hbtest2.c
hbtest2_LDADD = libiphb.la

libiphb_la_SOURCES = libiphb.c
libiphb_la_LDFLAGS = $(AM_LDFLAGS) -shared -version-info $(IPHB_LT_VERSION)

iphbdinclude_HEADERS = libiphb.h \
		       iphb_internal.h

pkgconfigdir = ${libdir}/pkgconfig
pkgconfig_DATA = libiphb.pc
