#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for storage/lmgr
#
# IDENTIFICATION
#    src/backend/storage/lmgr/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/storage/lmgr
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	condition_variable.o \
	deadlock.o \
	lmgr.o \
	lock.o \
	lwlock.o \
	predicate.o \
	proc.o \
	s_lock.o \

include $(top_srcdir)/src/backend/common.mk

lwlocknames.h: ../../../include/storage/lwlocklist.h ../../utils/activity/wait_event_names.txt generate-lwlocknames.pl
	$(PERL) $(srcdir)/generate-lwlocknames.pl $^

clean:
	rm -f lwlocknames.h
