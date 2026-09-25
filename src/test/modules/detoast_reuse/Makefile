# src/test/modules/detoast_reuse/Makefile

EXTRA_INSTALL = src/test/modules/injection_points

REGRESS = detoast_reuse

# The injection points are cluster-wide, so disable installcheck
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/detoast_reuse
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# This test is conditional on enable_injection_points in the parent Makefile,
# but the buildfarm's misc-check step ignores that condition, so repeat it.
ifeq ($(enable_injection_points),yes)
include $(top_srcdir)/contrib/contrib-global.mk
else
check:
	@echo "injection points are disabled in this build"
endif

endif
