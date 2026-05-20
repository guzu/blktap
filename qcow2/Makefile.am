
SUBDIRS = lib

AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += $(if $(GCOV),-fprofile-dir=/tmp/coverage/blktap/qcow2 -fprofile-arcs -ftest-coverage)

AM_CPPFLAGS  = -D_GNU_SOURCE
AM_CPPFLAGS += -I$(top_srcdir)/include
AM_CPPFLAGS += $(GLIB_CFLAGS)

bin_PROGRAMS = qemu-img

# Force ld to use DT_NEEDED entries of libqcow2.so to resolve transitive
# symbols (glib, gnutls, ...). Required with libtool >= 2.5 which no longer
# propagates dependency_libs to the link line. Harmless on older toolchains
# where this is already the default.
qemu_img_LDFLAGS = -Wl,--copy-dt-needed-entries
qemu_img_LDADD  = lib/libqcow2.la
qemu_img_LDADD += $(GLIB_LIBS)

qemu_img_SOURCES = qemu-img.c
qemu_img_SOURCES += qemu-progress.c

clean-local:
	-rm -rf *.gc??
