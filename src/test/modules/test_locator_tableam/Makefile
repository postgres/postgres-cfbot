# src/test/modules/test_locator_tableam/Makefile

MODULES = test_locator_tableam

EXTENSION = test_locator_tableam
DATA = test_locator_tableam--1.0.sql
PGFILEDESC = "test_locator_tableam - test table AMs for the locator contract"

REGRESS = noretain width_check max_offset bitmap_hooks

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_locator_tableam
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
