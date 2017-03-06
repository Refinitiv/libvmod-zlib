AM_CFLAGS = $(VARNISHAPI_CFLAGS) -Wall -Werror \
	-I$(VARNISHSRC)

AM_LDFLAGS = $(VARNISHAPI_LIBS) $(VMOD_LDFLAGS)

vmod_LTLIBRARIES = libvmod_zlib.la

libvmod_zlib_la_SOURCES = \
	vmod_zlib.c

nodist_libvmod_zlib_la_SOURCES = \
	vcc_zlib_if.c \
	vcc_zlib_if.h

dist_man_MANS = vmod_zlib.3

@BUILD_VMOD_ZLIB@

AM_TESTS_ENVIRONMENT = \
	PATH="$(VMOD_TEST_PATH)" \
	LD_LIBRARY_PATH="$(VARNISH_LIBRARY_PATH)"
TEST_EXTENSIONS = .vtc
VTC_LOG_COMPILER = varnishtest -v
AM_VTC_LOG_FLAGS = -Dvmod_zlib="$(VMOD_ZLIB)"

TESTS = @VMOD_TESTS@

EXTRA_DIST = \
	vmod_zlib.vcc \
	$(VMOD_TESTS)
