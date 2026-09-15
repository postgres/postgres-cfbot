# contrib/pgtrashcan/Makefile

MODULE_big = pgtrashcan
OBJS = \
	$(WIN32RES) \
	pgtrashcan.o

EXTENSION = pgtrashcan
DATA = pgtrashcan--1.0.sql
PGFILEDESC = "pgtrashcan - intercept DROP TABLE and move tables to a trash schema"

REGRESS = pgtrashcan

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pgtrashcan
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
