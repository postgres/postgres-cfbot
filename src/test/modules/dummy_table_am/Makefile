# src/test/modules/dummy_table_am/Makefile

MODULES = dummy_table_am

EXTENSION = dummy_table_am
DATA = dummy_table_am--1.0.sql
PGFILEDESC = "dummy_table_am - table access method template"

REGRESS = dummy_table_am

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/dummy_table_am
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
