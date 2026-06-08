# src/test/modules/test_logicalmsg_hooks/Makefile

MODULE_big = test_logicalmsg_hooks
OBJS = \
	$(WIN32RES) \
	test_logicalmsg_hooks.o
PGFILEDESC = "test_logicalmsg_hooks - test logical replication message hooks"

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_logicalmsg_hooks
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
