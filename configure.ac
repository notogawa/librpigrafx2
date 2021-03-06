#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([librpigrafx], [2.0], [ysugi@idein.jp])
AM_INIT_AUTOMAKE([foreign -W -Wall])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

# pkgconfig
AC_ARG_WITH(pkgconfigdir,
            AC_HELP_STRING([--with-pkgconfigdir=DIR],
                           [pkgconfig dir [default=LIBDIR/pkgconfig]]),
            [pkgconfigdir=${withval}],
            [pkgconfigdir=${libdir}/pkgconfig])
AC_MSG_NOTICE([pkgconfig directory is ${pkgconfigdir}])
AC_SUBST([pkgconfigdir])

# Checks for programs.
AC_PROG_CC
AM_PROG_AR

# Checks for libraries.
PKG_CHECK_MODULES([BCM_HOST], [bcm_host], , [AC_MSG_ERROR("missing -lbcm_host")])
AC_SUBST([BCM_HOST_CFLAGS])
AC_SUBST([BCM_HOST_LIBS])
PKG_CHECK_MODULES([MMAL], [mmal], , [AC_MSG_ERROR("missing -lmmal")])
AC_SUBST([MMAL_CFLAGS])
AC_SUBST([MMAL_LIBS])
AC_CHECK_LIB([qmkl], [mailbox_qpu_enable],
             [QMKL_LIBS=-lqmkl
              AC_SUBST(QMKL_LIBS)],
             [AC_MSG_ERROR("missing -lqmkl")])

# Checks for header files.
AC_CHECK_HEADERS([stdio.h stdint.h stdlib.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_UINT32_T

# Checks for library functions.
AC_FUNC_REALLOC

LT_INIT
AC_CONFIG_FILES([Makefile include/Makefile src/Makefile test/Makefile librpigrafx.pc])
AC_OUTPUT
