# src/test/modules/test_tablespace_options/Makefile

MODULES = test_tablespace_options
PGFILEDESC = "test_tablespace_options - test extension-defined tablespace options"

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_tablespace_options
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
