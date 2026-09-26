# src/test/modules/test_conflict_typeout/Makefile

MODULES = test_conflict_typeout

EXTENSION = test_conflict_typeout
DATA = test_conflict_typeout--1.0.sql
PGFILEDESC = "test_conflict_typeout - demonstrates the conflict log table's output-function size gap"

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_conflict_typeout
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
