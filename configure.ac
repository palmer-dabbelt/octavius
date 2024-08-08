AC_INIT([impl], [0.1], [palmer@dabbelt.com])

AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])

AC_PROG_CC

AC_LANG(C++)
AC_PROG_CXX

AC_CONFIG_FILES([Makefile])

AC_CHECK_LIB([avformat], [avformat_open_input])
AC_CHECK_LIB([avcodec], [avcodec_find_decoder])
AC_CHECK_LIB([avutil], [av_image_alloc])
AC_CHECK_LIB([swscale], [sws_getCachedContext])

PKG_CHECK_MODULES(OPENCV, [opencv4])

AC_CHECK_LIB([mp4v2], [MP4Modify])

AC_OUTPUT
