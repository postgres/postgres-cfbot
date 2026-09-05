# src/test/modules/test_walwriter/Makefile

MODULE_big = test_walwriter
OBJS = \
	$(WIN32RES) \
	test_walwriter.o
PGFILEDESC = "test_walwriter - test facilities for the WAL writer"

EXTENSION = test_walwriter
DATA = test_walwriter--1.0.sql

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_walwriter
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
