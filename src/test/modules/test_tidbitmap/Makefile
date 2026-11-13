# src/test/modules/test_tidbitmap/Makefile

MODULE_big = test_tidbitmap
OBJS = \
	$(WIN32RES) \
	test_tidbitmap.o
PGFILEDESC = "test_tidbitmap - test code for src/backend/nodes/tidbitmap.c"

EXTENSION = test_tidbitmap
DATA = test_tidbitmap--1.0.sql

REGRESS = test_tidbitmap

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_tidbitmap
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
