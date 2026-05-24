# src/test/modules/test_scram/Makefile

MODULE_big = test_scram
OBJS = \
	$(WIN32RES) \
	test_scram.o
PGFILEDESC = "test_scram - test SCRAM helpers in src/common"

EXTENSION = test_scram
DATA = test_scram--1.0.sql

REGRESS = test_scram

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_scram
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
