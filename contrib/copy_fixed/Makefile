# contrib/copy_fixed/Makefile

MODULE_big = copy_fixed
OBJS = \
	$(WIN32RES) \
	copy_fixed.o
PGFILEDESC = "copy_fixed - fixed-width COPY format"

REGRESS = copy_fixed

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/copy_fixed
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
