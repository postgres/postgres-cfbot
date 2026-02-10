# contrib/vci/storage/Makefile

SUBOBJS = \
	vci_chunk.o \
	vci_columns.o \
	vci_columns_data.o \
	vci_fetch.o \
	vci_freelist.o \
	vci_index.o \
	vci_internal_view.o \
	vci_low_utils.o \
	vci_memory_entry.o \
	vci_ros.o \
	vci_ros_command.o \
	vci_ros_daemon.o \
	vci_tidcrid.o \
	vci_wos.o \
	vci_xact.o

EXTRA_CLEAN = SUBSYS.o $(SUBOBJS)

PG_CPPFLAGS = -I $(top_srcdir)/contrib/vci/include

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/vci/storage
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

override CFLAGS += $(CFLAGS_SL)
