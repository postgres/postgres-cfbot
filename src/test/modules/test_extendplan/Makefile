# src/test/modules/test_extendplan/Makefile

MODULE_big = test_extendplan
OBJS = \
	$(WIN32RES) \
	test_extendplan.o
PGFILEDESC = "test_extendplan - test module for planner extension state"

EXTENSION = test_extendplan
DATA = test_extendplan--1.0.sql

REGRESS = test_extendplan

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_extendplan
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
