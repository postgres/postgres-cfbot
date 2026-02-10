# contrib/vci/Makefile

MODULE_big = vci

OBJS = \
#	vci_main.o \
#	vci_read_guc.o \
#	vci_shmem.o \
	vci_supported_funcs.o \
	vci_supported_types.o
SUBDIRS = \
       executor \
       storage \
       utils

OBJS += \
        $(patsubst $(top_srcdir)/contrib/vci/%.c,%.o,$(foreach dir,$(SUBDIRS), $(sort $(wildcard $(top_srcdir)/contrib/vci/$(dir)/*.c))))

EXTENSION = vci
DATA = vci--1.0.sql

PG_CPPFLAGS = -I $(top_srcdir)/contrib/vci/include

REGRESS = vci bugs
REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/vci/vci.conf

# Disabled because these tests require "shared_preload_libraries=vci",
# which typical installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/vci
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
