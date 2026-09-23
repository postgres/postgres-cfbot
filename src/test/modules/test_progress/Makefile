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

# ... and the phases a command reports against the text the view gives them
# and the table the documentation lists them in.
export SYSTEM_VIEWS_SQL := $(abs_top_srcdir)/src/backend/catalog/system_views.sql
export MONITORING_SGML := $(abs_top_srcdir)/doc/src/sgml/monitoring.sgml
