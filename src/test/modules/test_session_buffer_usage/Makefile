# src/test/modules/test_session_buffer_usage/Makefile

MODULE_big = test_session_buffer_usage
OBJS = \
	$(WIN32RES) \
	test_session_buffer_usage.o

EXTENSION = test_session_buffer_usage
DATA = test_session_buffer_usage--1.0.sql
PGFILEDESC = "test_session_buffer_usage - show buffer usage statistics for the current session"

REGRESS = test_session_buffer_usage

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_session_buffer_usage
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
