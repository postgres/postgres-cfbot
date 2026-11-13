# contrib/vci/executor/Makefile

SUBOBJS = \
	vci_agg.o \
	vci_aggmergetranstype.o \
	vci_aggref.o \
	vci_executor.o \
	vci_fetch_column_store.o \
	vci_gather.o \
	vci_param.o \
	vci_plan.o \
	vci_planner.o \
	vci_planner_preanalyze.o \
	vci_plan_func.o \
	vci_scan.o \
	vci_sort.o \
	vci_vector_executor.o

EXTRA_CLEAN = SUBSYS.o $(SUBOBJS)

PG_CPPFLAGS = -I$(top_srcdir)/contrib/vci/include

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/vci/executor
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

override CFLAGS += $(CFLAGS_SL)
