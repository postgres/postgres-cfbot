# contrib/join_hardness/Makefile

MODULES = join_hardness
PGFILEDESC = "join_hardness - join search hardness estimator"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/join_hardness
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
