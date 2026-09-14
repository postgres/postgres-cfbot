#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/flux
#
# IDENTIFICATION
#    src/backend/access/flux/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/access/flux
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	flux_handler.o \
	flux_tuple.o \
	flux_mvcc.o \
	flux_stats.o \
	flux_xlog.o \
	flux_fsm.o \
	flux_operations.o \
	flux_lock.o \
	flux_slot.o \
	flux_vm.o \
	flux_undo.o \
	flux_pbu.o \
	flux_pvs.o

include $(top_srcdir)/src/backend/common.mk