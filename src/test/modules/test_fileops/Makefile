MODULE_big = test_fileops
OBJS = test_fileops.o
EXTENSION = test_fileops
DATA = test_fileops--1.0.sql
PGFILEDESC = "test_fileops - test transactional file operations"
REGRESS = test_fileops
TAP_TESTS = 1
ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_fileops
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
