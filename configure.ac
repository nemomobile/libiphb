# Package name and version
AC_INIT(libiphb, 0.61.30)

AM_INIT_AUTOMAKE

AC_PREREQ([2.61])
AC_CONFIG_SRCDIR([iphbd/libiphb.c])

# NOTE: Must the LT version must be changed when code is released:
#  - If interfaces changed/added/removed: lt_current++, lt_rev=0
#  - If interfaces added: lt_age++
#  - If Interfaces removed: lt_age=0
#  - If only code changes (interface untouched): lt_rev++
iphb_lt_current=0
iphb_lt_rev=0
iphb_lt_age=0
AC_SUBST([IPHB_LT_VERSION],[$iphb_lt_current:$iphb_lt_rev:$iphb_lt_age])

#
# Compiler and linker flags
#
AC_DEFINE_UNQUOTED([PRG_VERSION],[$VERSION],[Version for libiphb])
# FIXME: DSME_LOG_ENABLE should be proper&working configure option
#AC_DEFINE([DSME_LOG_ENABLE], [1])

AC_SUBST(C_GENFLAGS, ["-pthread -g -std=c99 -Wall -Wwrite-strings -Wmissing-prototypes -Werror"])
AC_SUBST(C_OPTFLAGS, ["-O2 -s"])
AC_SUBST(C_DBGFLAGS, ["-g -DDEBUG -DDSME_LOG_ENABLE"])

# Checks for programs.
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET
AC_PROG_LIBTOOL
PKG_PROG_PKG_CONFIG

# Specify dir for local macros
AC_CONFIG_MACRO_DIR([m4])

# Enable per-target compiler options
AM_PROG_CC_C_O()

# Pkgconf checks
PKG_CHECK_MODULES(GLIB, glib-2.0)

# Check libs (that are not yet checked)
AC_CHECK_LIB([dsme], [dsmesock_init], [AC_MSG_NOTICE([got libdsme])], AC_MSG_FAILURE([libdsme required!]), [$GLIB_LIBS])

# Check headers
AC_CHECK_HEADERS([arpa/inet.h stdlib.h string.h sys/ioctl.h sys/socket.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_PID_T
AC_TYPE_SIZE_T

# Check common library functions
AC_FUNC_MALLOC
AC_CHECK_FUNCS([inet_ntoa memset select socket strerror])

# Create files
AC_CONFIG_FILES([Makefile
                 iphbd/Makefile
                 iphbd/libiphb-uninstalled.pc
                 iphbd/libiphb.pc])

AC_OUTPUT
