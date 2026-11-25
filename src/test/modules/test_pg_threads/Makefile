# src/test/modules/test_pg_threads/Makefile

PGFILEDESC = "test_pg_threads - test pg_threads.h"
PGAPPICON = win32

PROGRAM = test_pg_threads
OBJS = $(WIN32RES) test_pg_threads.o

override CFLAGS += $(PTHREAD_CFLAGS)
PG_LIBS += $(PTHREAD_LIBS)

NO_INSTALL = 1
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_pg_threads
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
