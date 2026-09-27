#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/undo
#
# IDENTIFICATION
#    src/backend/access/undo/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/access/undo
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	undo.o \
	undo_bufmgr.o \
	undo_xlog.o \
	undoapply.o \
	undoinsert.o \
	undolog.o \
	undorecord.o \
	undormgr.o \
	undostats.o \
	undoworker.o \
	xactundo.o \
	perbackend/pbu_undolog.o \
	perbackend/pbu_undofile.o \
	perbackend/pbu_undorecord.o \
	perbackend/pbu_undoinsert.o \
	perbackend/pbu_undoaction.o \
	perbackend/pbu_undoactionxlog.o \
	perbackend/pbu_undorequest.o \
	perbackend/pbu_undodiscard.o \
	perbackend/pbu_discardworker.o \
	perbackend/pbu_undoworker.o \
	perbackend/pbu_recovery.o \
	perbackend/pbu_helpers.o

include $(top_srcdir)/src/backend/common.mk
