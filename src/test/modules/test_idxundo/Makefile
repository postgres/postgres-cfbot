# src/test/modules/test_idxundo/Makefile

MODULE_big = test_idxundo
OBJS = \
	$(WIN32RES) \
	test_idxundo.o
PGFILEDESC = "test_idxundo - exercise index UNDO"

EXTENSION = test_idxundo
DATA = test_idxundo--1.0.sql

REGRESS = test_idxundo
# The end-to-end tests verify that ROLLBACK reversed the index entries by
# reading them back through the index, and check the result with amcheck.
EXTRA_INSTALL = contrib/amcheck contrib/pageinspect src/test/modules/injection_points
export enable_injection_points
REGRESS_OPTS = --load-extension=amcheck --load-extension=pageinspect
REGRESS_OPTS += --temp-config $(top_srcdir)/src/test/modules/test_idxundo/test_idxundo.conf
# Disables the logical revert worker, which needs a server of its own.
NO_INSTALLCHECK = 1

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_idxundo
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
