# src/test/modules/test_wait_totals/Makefile

MODULE_big = test_wait_totals
OBJS = \
	$(WIN32RES) \
	test_wait_totals.o
PGFILEDESC = "test_wait_totals - producer for the pg_wait_event_tracing totals tests"

EXTENSION = test_wait_totals
DATA = test_wait_totals--1.0.sql

REGRESS = test_wait_totals

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_wait_totals
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
