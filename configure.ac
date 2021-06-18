AC_PREREQ([2.69])
AC_INIT([fc-char], [1.0], [josh@thibodaux.net])
AC_CONFIG_SRCDIR([fc-char.c])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE

# Checks for programs.
AC_PROG_CC

PKG_CHECK_MODULES([DEPS], [fontconfig xft xmu])

# Checks for libraries.
# FIXME: Replace `main' with a function in `-lX11':
AC_CHECK_LIB([X11], [main])
# FIXME: Replace `main' with a function in `-lXft':
AC_CHECK_LIB([Xft], [main])
# FIXME: Replace `main' with a function in `-lfontconfig':
AC_CHECK_LIB([fontconfig], [main])
# FIXME: Replace `main' with a function in `-lm':
AC_CHECK_LIB([m], [main])
# FIXME: Replace `main' with a function in `-luninameslist':
AC_CHECK_LIB([uninameslist], [main])
AC_CHECK_LIB([Xmu], [main])
AC_CHECK_LIB([Xext], [main])

# Checks for header files.
AC_CHECK_HEADERS([locale.h stdint.h stdlib.h string.h fontconfig/fontconfig.h X11/Xft/Xft.h X11/Xatom.h X11/Xmu/Atoms.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SIZE_T
AC_TYPE_UINT32_T

# Checks for library functions.
AC_CHECK_FUNCS([floor setlocale sqrt strerror strtol])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
