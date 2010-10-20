# Package name and version
AC_INIT(libiphb, 0.6.25)

AM_INIT_AUTOMAKE

AC_PREREQ([2.61])
AC_CONFIG_SRCDIR([iphbd/libiphb.c])

#
# CONFIG OPTIONS
#

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
PKG_CHECK_MODULES(DBUS, dbus-1)
PKG_CHECK_MODULES(DBUSGLIB, dbus-glib-1)

# Check libs (that are not yet checked)
# Whitespaces in 'action-if-found' fields in order to not (auto)update LIBS variable
AC_CHECK_LIB([rt], [clock_nanosleep], [AC_MSG_NOTICE([got librt])], AC_MSG_FAILURE([librt required!]))
AC_CHECK_LIB([dsme], [dsmesock_init], [AC_MSG_NOTICE([got libdsme])], AC_MSG_FAILURE([libdsme required!]), [$GLIB_LIBS])

# Check headers
AC_CHECK_HEADERS([arpa/inet.h fcntl.h inttypes.h limits.h stdint.h stdlib.h \
                  string.h strings.h sys/ioctl.h sys/socket.h sys/time.h    \
                  syslog.h unistd.h utmpx.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_TYPE_UID_T
AC_C_INLINE
AC_TYPE_INT16_T
AC_TYPE_INT32_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT8_T

# Check common library functions
AC_FUNC_ALLOCA
AC_FUNC_FORK
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_FUNC_STRTOD
AC_CHECK_FUNCS([atexit clock_gettime dup2 gettimeofday inet_ntoa memmove        \
                memset realpath select setenv socket strcasecmp strdup strerror \
                strndup strpbrk strrchr strstr strtoul uname utmpxname])

# Create files
AC_CONFIG_FILES([Makefile
                 iphbd/Makefile
                 iphbd/libiphb-uninstalled.pc
                 iphbd/libiphb.pc])

AC_OUTPUT
