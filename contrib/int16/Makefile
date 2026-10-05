# contrib/int16/Makefile

MODULE_big = int16
OBJS = \
	$(WIN32RES) \
	int16.o \
	int16_numeric.o \
	int16_funcs.o

EXTENSION = int16
DATA = int16--1.0.sql
PGFILEDESC = "int16 - 128-bit signed integer data type"

HEADERS = int16.h

REGRESS = init int16 int16_overflow int16_numeric int16_funcs

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/int16
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
