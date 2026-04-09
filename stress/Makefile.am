AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror

AM_CPPFLAGS  = -D_GNU_SOURCE
AM_CPPFLAGS += -I$(top_srcdir)/include
AM_CPPFLAGS += -I$(top_srcdir)/drivers

# Stress / micro-benchmark binaries. Never installed.
noinst_PROGRAMS = tapdisk-vbd-stress

tapdisk_vbd_stress_SOURCES = tapdisk-vbd-stress.c
tapdisk_vbd_stress_LDADD   = $(top_builddir)/drivers/libtapdisk.la
