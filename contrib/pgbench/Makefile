# contrib/pgbench/Makefile

MODULE_big = pgbench
OBJS = \
	$(WIN32RES) \
	pgbench.o \
	pgbench_funcs.o

EXTENSION = pgbench
DATA = pgbench--1.0.sql
PGFILEDESC = "pgbench - random distributions and benchmarking utilities"

REGRESS = pgbench

EXTRA_CLEAN = pgbench_funcs.c

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pgbench
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# pgbench_funcs.c is shared from src/common
pgbench_funcs.c: % : $(top_srcdir)/src/common/%
	rm -f $@ && $(LN_S) $< .
