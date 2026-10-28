# src/test/modules/test_wait_hook/Makefile

MODULE_big = test_wait_hook
OBJS = \
	$(WIN32RES) \
	test_wait_hook.o
PGFILEDESC = "test_wait_hook - test code for the wait event hook contract"

EXTENSION = test_wait_hook
DATA = test_wait_hook--1.0.sql

REGRESS = test_wait_hook

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_wait_hook
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
