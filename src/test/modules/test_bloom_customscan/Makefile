# src/test/modules/test_bloom_customscan/Makefile

MODULE_big = test_bloom_customscan
OBJS = \
	$(WIN32RES) \
	test_bloom_customscan.o
PGFILEDESC = "test_bloom_customscan - CustomScan consuming a hashjoin bloom filter"

EXTENSION = test_bloom_customscan
DATA = test_bloom_customscan--1.0.sql

REGRESS = test_bloom_customscan

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_bloom_customscan
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
