# contrib/amcheck/Makefile

MODULE_big	= amcheck
OBJS = \
	$(WIN32RES) \
	verify_common.o \
	verify_gin.o \
	verify_heapam.o \
	verify_nbtree.o

EXTENSION = amcheck
DATA = amcheck--1.2--1.3.sql amcheck--1.1--1.2.sql amcheck--1.0--1.1.sql amcheck--1.0.sql \
		amcheck--1.3--1.4.sql amcheck--1.4--1.5.sql amcheck--1.5--1.6.sql
PGFILEDESC = "amcheck - function for verifying relation integrity"

REGRESS = check check_btree check_gin check_heap

EXTRA_INSTALL = contrib/pg_walinspect contrib/pg_surgery
TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/amcheck
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
