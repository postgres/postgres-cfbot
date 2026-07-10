# src/test/modules/test_ddl_deparse/Makefile

MODULE_big = test_ddl_deparse
OBJS = \
	$(WIN32RES) \
	test_ddl_deparse.o
PGFILEDESC = "test_ddl_deparse - regression testing for DDL deparsing"

EXTENSION = test_ddl_deparse
DATA = test_ddl_deparse--1.0.sql

# test_ddl_deparse must be first
REGRESS = test_ddl_deparse \
	create_extension \
	create_schema \
	create_type \
	create_conversion \
	create_domain \
	create_sequence \
	create_table \
	create_transform \
	alter_table \
	create_view \
	create_trigger \
	create_rule \
	comment_on \
	alter_function \
	alter_sequence \
	alter_ts_config \
	alter_type_enum \
	opfamily \
	defprivs \
	textsearch \
	matviews \
	deparse_create_table

TAP_TESTS = 1

EXTRA_INSTALL = contrib/pg_stat_statements

# needed by the TAP test to locate regress.so for the core regression run
REGRESS_SHLIB=$(abs_top_builddir)/src/test/regress/regress$(DLSUFFIX)
export REGRESS_SHLIB

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_ddl_deparse
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
