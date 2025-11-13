MODULE_big = test_large_files
OBJS = \
	$(WIN32RES) \
	test_large_files.o

EXTENSION = test_large_files
DATA = test_large_files--1.0.sql

REGRESS = test_large_files

NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_large_files
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
