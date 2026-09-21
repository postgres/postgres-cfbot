# src/test/modules/test_progress/Makefile

TAP_TESTS = 1

# The test reads the server log, which is cluster-wide.
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_progress
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# The test checks its description of the progress parameters against this.
export PROGRESS_H := $(abs_top_srcdir)/src/include/commands/progress.h
