# src/test/modules/test_deletemark/Makefile

MODULE_big = test_deletemark
OBJS = \
	$(WIN32RES) \
	test_deletemark.o
PGFILEDESC = "test_deletemark - synthetic exerciser for nbtree delete-marking"

EXTENSION = test_deletemark
DATA = test_deletemark--1.0.sql

REGRESS = test_deletemark
TAP_TESTS = 1

# The SQL test uses amcheck to verify structural soundness of an index that
# contains a delete-marked entry, so amcheck must be present in the temporary
# install used by "make check".
EXTRA_INSTALL = contrib/amcheck

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_deletemark
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
