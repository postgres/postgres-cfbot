# src/test/modules/test_extensible/Makefile

MODULE_big = test_extensible
OBJS = \
	$(WIN32RES) \
	test_extensible.o
PGFILEDESC = "test_extensible - test module for extensible node and custom scan registration"

EXTENSION = test_extensible
DATA = test_extensible--1.0.sql

REGRESS_OPTS = --temp-config $(top_srcdir)/src/test/modules/test_extensible/test_extensible.conf
REGRESS = test_extensible
# Disabled because these tests require "shared_preload_libraries=test_extensible",
# which typical installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_extensible
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
