# contrib/pg_stat_log/Makefile

MODULE_big = pg_stat_log
OBJS = \
	$(WIN32RES) \
	pg_stat_log.o

EXTENSION = pg_stat_log
DATA = pg_stat_log--0.1.sql
PGFILEDESC = "pg_stat_log - cumulative statistics about log messages"

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/pg_stat_log/pg_stat_log.conf
REGRESS = pg_stat_log

# Disabled because these tests require "shared_preload_libraries=pg_stat_log",
# which typical installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_stat_log
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# Generate the errcode-name lookup header from the in-tree errcodes.txt.
ERRCODES_TXT = $(top_srcdir)/src/backend/utils/errcodes.txt

pg_stat_log_errcodes.h: generate-errcode-names.pl $(ERRCODES_TXT)
	$(PERL) $(srcdir)/generate-errcode-names.pl --outfile $@ $(ERRCODES_TXT)

pg_stat_log.o: pg_stat_log_errcodes.h

EXTRA_CLEAN = pg_stat_log_errcodes.h
