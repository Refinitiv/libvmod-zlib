#!/bin/sh

warn() {
	echo "WARNING: $@" 1>&2
}

case `uname -s` in
Darwin)
	LIBTOOLIZE=glibtoolize
	;;
FreeBSD)
	LIBTOOLIZE=libtoolize
	;;
Linux)
	LIBTOOLIZE=libtoolize
	;;
SunOS)
	LIBTOOLIZE=libtoolize
	;;
*)
	warn "unrecognized platform:" `uname -s`
	LIBTOOLIZE=libtoolize
esac

automake_version=`automake --version | tr ' ' '\n' | egrep '^[0-9]\.[0-9a-z.-]+'`
if [ -z "$automake_version" ] ; then
	warn "unable to determine automake version"
else
	case $automake_version in
		0.*|1.[0-8]|1.[0-8][.-]*)
			warn "automake ($automake_version) detected; 1.9 or newer recommended"
			;;
		*)
			;;
	esac
fi

# check for varnishapi.m4 in custom paths
AC_MACROS=""

dataroot=$(pkg-config --variable=datarootdir varnishapi 2>/dev/null)
if [ ! -z "$dataroot" ] ; then
    AC_MACROS=" -I ${dataroot}/aclocal ${AC_MACROS}"
fi

if [ ! -z "$VARNISHSRC" ]; then
    AC_MACROS=" -I ${VARNISHSRC} ${AC_MACROS}"
fi

if [ -z "${AC_MACROS}" ]; then
	cat >&2 <<'EOF'
Package varnishapi was not found in the pkg-config search path,
and environment variable VARNISHSRC is not found.
Perhaps you should add the directory containing `varnishapi.pc'
to the PKG_CONFIG_PATH environment variable, or set VARNISHSRC.
EOF
	exit 1
fi
set -ex
aclocal -I m4 ${AC_MACROS}
$LIBTOOLIZE --copy --force
autoheader
automake --add-missing --copy --foreign
autoconf
