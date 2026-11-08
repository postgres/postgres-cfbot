# src/test/modules/test_upper_paths_hook/Makefile

MODULE_big = test_upper_paths_hook
OBJS = \
	$(WIN32RES) \
	test_upper_paths_hook.o
PGFILEDESC = "test_upper_paths_hook - test create_upper_paths_hook"

REGRESS = test_upper_paths_hook

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_upper_paths_hook
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
