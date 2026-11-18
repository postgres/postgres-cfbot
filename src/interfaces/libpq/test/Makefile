# src/interfaces/libpq/test/Makefile

PGFILEDESC = "libpq test program"
PGAPPICON = win32

subdir = src/interfaces/libpq/test
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(PORTNAME), win32)
LDFLAGS += -lws2_32
endif

override CPPFLAGS := -I$(libpq_srcdir) -I$(top_builddir)/src/port -I$(top_srcdir)/src/port $(CPPFLAGS)
LDFLAGS_INTERNAL += $(libpq_pgport)

PROGS = libpq_testclient libpq_trace_regress libpq_uri_regress

all: $(PROGS)

$(PROGS): $(WIN32RES)

# libpq_trace_regress.c includes fe-trace.c
libpq_trace_regress.o: $(libpq_srcdir)/fe-trace.c

clean distclean:
	rm -f $(PROGS) *.o
