# src/test/modules/dummy_index_am/Makefile

MODULES = dummy_index_am

EXTENSION = dummy_index_am
DATA = dummy_index_am--1.0.sql
PGFILEDESC = "dummy_index_am - index access method template"

REGRESS = reloptions notify_delete
ISOLATION = notify_speculative notify_repack
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/dummy_index_am
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
