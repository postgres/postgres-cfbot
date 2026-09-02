# src/test/modules/test_ready_for_query/Makefile

MODULE_big = test_ready_for_query
OBJS = \
	$(WIN32RES) \
	test_ready_for_query.o
PGFILEDESC = "test_ready_for_query - test code for ReadyForQuery hook"

EXTENSION = test_ready_for_query
DATA = test_ready_for_query--1.0.sql
REGRESS = test_ready_for_query
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_ready_for_query
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
