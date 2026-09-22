# contrib/pg_wait_event_tracing/Makefile

MODULE_big = pg_wait_event_tracing
OBJS = \
	$(WIN32RES) \
	pg_wait_event_tracing.o

EXTENSION = pg_wait_event_tracing
DATA = pg_wait_event_tracing--1.0.sql
PGFILEDESC = "pg_wait_event_tracing - statistics and trace collection for explicitly instrumented wait events"

REGRESS = pg_wait_event_tracing
REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/pg_wait_event_tracing/pg_wait_event_tracing.conf

# The tests need the module preloaded, which typical installcheck users
# will not have (shared_preload_libraries requires a server restart).
NO_INSTALLCHECK = 1

TAP_TESTS = 1
EXTRA_INSTALL = src/test/modules/injection_points
export enable_injection_points

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_wait_event_tracing
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
