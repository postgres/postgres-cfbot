# src/test/modules/test_copy_custom_format/Makefile

MODULE_big = test_copy_custom_format
OBJS = \
	$(WIN32RES) \
	test_copy_custom_format.o
PGFILEDESC = "test_copy_custom_format - test custom COPY FORMAT"

REGRESS = test_copy_custom_format

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_copy_custom_format
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
